#include "offline_store.hpp"

#if defined(TERVANIX_BOARD_NESSO_N1)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace
{

    constexpr char TAG[] = "offline";
    constexpr char STORAGE_LABEL[] = "storage";
    constexpr std::uint8_t STORAGE_SUBTYPE = 0x82;

    constexpr std::uint32_t BANK_MAGIC = 0x4B425854;   // TXBK
    constexpr std::uint32_t RECORD_MAGIC = 0x524C5854; // TXLR
    constexpr std::uint32_t BANK_VERSION = 1;
    constexpr std::size_t SECTOR_SIZE = 4096;
    constexpr std::uint8_t TOMBSTONE_ACTIVE = 0xFF;
    constexpr std::uint8_t TOMBSTONE_DELETED = 0x00;

    struct BankHeader {
        std::uint32_t magic = BANK_MAGIC;
        std::uint32_t version = BANK_VERSION;
        std::uint32_t generation = 0;
        std::uint32_t reserved = 0;
    };

    struct StoredRecord {
        std::uint32_t magic = RECORD_MAGIC;
        std::uint32_t checksum = 0;
        std::uint8_t tombstone = TOMBSTONE_ACTIVE;
        std::uint8_t sample_count = 0;
        std::uint8_t has_prediction = 0;
        std::uint8_t is_unknown = 1;
        std::uint32_t reserved = 0;

        tervanix::mqtt::TelemetrySample
            samples[tervanix::offline::MAX_BATCH_SAMPLES]{};
        tervanix::har::Prediction prediction{};
    };

    const esp_partition_t *partition = nullptr;
    SemaphoreHandle_t mutex_handle = nullptr;

    std::size_t region_offset = 0;
    std::size_t bank_bytes = 0;
    std::size_t record_area_bytes = 0;
    std::uint32_t capacity = 0;

    int active_bank = 0;
    std::uint32_t active_generation = 0;
    std::uint32_t allocated_slots = 0;

    bool purge_unknown_first_policy = true;
    std::int32_t peeked_slot = -1;

    std::uint32_t purged_unknown = 0;
    std::uint32_t purged_oldest = 0;
    std::uint32_t queued_batches = 0;
    std::uint32_t queued_samples = 0;
    std::uint32_t queued_unknown_batches = 0;

    class Lock {
        public:
        Lock()
        {
            if (mutex_handle)
            {
                xSemaphoreTake(mutex_handle, portMAX_DELAY);
            }
        }
        ~Lock()
        {
            if (mutex_handle)
            {
                xSemaphoreGive(mutex_handle);
            }
        }
    };

    std::size_t bank_start(int bank)
    {
        return region_offset + static_cast<std::size_t>(bank) * bank_bytes;
    }

    std::size_t record_offset(int bank, std::uint32_t slot)
    {
        return bank_start(bank) + SECTOR_SIZE +
               static_cast<std::size_t>(slot) * sizeof(StoredRecord);
    }

    std::uint32_t checksum_record(const StoredRecord &input)
    {
        StoredRecord copy = input;
        copy.checksum = 0;
        copy.tombstone = TOMBSTONE_ACTIVE;

        const auto *bytes = reinterpret_cast<const std::uint8_t *>(&copy);

        // FNV-1a is used only as a corruption detector for the local flash
        // journal; authenticity is still provided by the MQTT AES-GCM layer.
        std::uint32_t hash = 2166136261u;
        for (std::size_t i = 0; i < sizeof(copy); ++i)
        {
            hash ^= bytes[i];
            hash *= 16777619u;
        }
        return hash;
    }

    bool header_valid(const BankHeader &header)
    {
        return header.magic == BANK_MAGIC && header.version == BANK_VERSION &&
               header.generation != 0;
    }

    bool record_valid(const StoredRecord &record)
    {
        return record.magic == RECORD_MAGIC && record.sample_count > 0 &&
               record.sample_count <= tervanix::offline::MAX_BATCH_SAMPLES &&
               record.checksum == checksum_record(record);
    }

    bool record_free(std::uint32_t magic) { return magic == 0xFFFFFFFFu; }

    bool prediction_unknown(const tervanix::offline::Batch &batch)
    {
        return !batch.has_prediction || batch.prediction.state[0] == '\0' ||
               std::strcmp(batch.prediction.state, "unknown") == 0;
    }

    StoredRecord encode(const tervanix::offline::Batch &batch)
    {
        StoredRecord record{};
        record.magic = RECORD_MAGIC;
        record.tombstone = TOMBSTONE_ACTIVE;
        record.sample_count = batch.sample_count;
        record.has_prediction = batch.has_prediction ? 1 : 0;
        record.is_unknown = prediction_unknown(batch) ? 1 : 0;

        std::memcpy(record.samples, batch.samples, sizeof(batch.samples));

        if (batch.has_prediction)
        {
            record.prediction = batch.prediction;
        }

        record.checksum = checksum_record(record);
        return record;
    }

    void decode(const StoredRecord &record, tervanix::offline::Batch &batch)
    {
        batch = {};
        batch.sample_count = record.sample_count;
        std::memcpy(batch.samples, record.samples, sizeof(batch.samples));
        batch.has_prediction = record.has_prediction != 0;
        if (batch.has_prediction)
        {
            batch.prediction = record.prediction;
        }
    }

    esp_err_t read_record(int bank, std::uint32_t slot, StoredRecord &record)
    {
        return esp_partition_read(
            partition, record_offset(bank, slot), &record, sizeof(record));
    }

    esp_err_t
    write_record(int bank, std::uint32_t slot, const StoredRecord &record)
    {
        return esp_partition_write(
            partition, record_offset(bank, slot), &record, sizeof(record));
    }

    esp_err_t commit_bank_header(int bank, std::uint32_t generation)
    {
        BankHeader header{};
        header.generation = generation;

        return esp_partition_write(
            partition, bank_start(bank), &header, sizeof(header));
    }

    esp_err_t erase_bank(int bank)
    {
        return esp_partition_erase_range(
            partition, bank_start(bank), bank_bytes);
    }

    esp_err_t scan_active_bank()
    {
        allocated_slots = 0;
        peeked_slot = -1;
        queued_batches = 0;
        queued_samples = 0;
        queued_unknown_batches = 0;

        for (std::uint32_t slot = 0; slot < capacity; ++slot)
        {
            std::uint32_t magic = 0;
            esp_err_t result = esp_partition_read(
                partition,
                record_offset(active_bank, slot),
                &magic,
                sizeof(magic));
            if (result != ESP_OK)
            {
                return result;
            }

            if (record_free(magic))
            {
                allocated_slots = slot;
                return ESP_OK;
            }

            allocated_slots = slot + 1;

            StoredRecord record{};
            result = read_record(active_bank, slot, record);
            if (result == ESP_OK && record_valid(record) &&
                record.tombstone == TOMBSTONE_ACTIVE)
            {
                ++queued_batches;
                queued_samples += record.sample_count;
                if (record.is_unknown != 0)
                {
                    ++queued_unknown_batches;
                }
            }
        }

        return ESP_OK;
    }

    esp_err_t compact_for_space()
    {
        const int destination = 1 - active_bank;

        ESP_LOGW(
            TAG,
            "Offline flash bank full; compacting. policy=%s",
            purge_unknown_first_policy ? "unknown-first" : "oldest-only");

        esp_err_t result = erase_bank(destination);
        if (result != ESP_OK)
        {
            return result;
        }

        bool has_unknown = false;
        if (purge_unknown_first_policy)
        {
            for (std::uint32_t slot = 0; slot < allocated_slots; ++slot)
            {
                StoredRecord record{};
                if (read_record(active_bank, slot, record) != ESP_OK)
                {
                    continue;
                }
                if (record_valid(record) &&
                    record.tombstone == TOMBSTONE_ACTIVE &&
                    record.is_unknown != 0)
                {
                    has_unknown = true;
                    break;
                }
            }
        }

        bool dropped_oldest = false;
        std::uint32_t destination_slot = 0;
        std::uint32_t destination_samples = 0;
        std::uint32_t destination_unknown = 0;
        std::uint32_t removed_unknown = 0;

        for (std::uint32_t slot = 0; slot < allocated_slots; ++slot)
        {
            StoredRecord record{};
            result = read_record(active_bank, slot, record);
            if (result != ESP_OK)
            {
                return result;
            }

            if (!record_valid(record) || record.tombstone != TOMBSTONE_ACTIVE)
            {
                continue;
            }

            if (has_unknown && record.is_unknown != 0)
            {
                ++removed_unknown;
                continue;
            }

            if (!has_unknown && !dropped_oldest)
            {
                dropped_oldest = true;
                continue;
            }

            if (destination_slot >= capacity)
            {
                return ESP_ERR_NO_MEM;
            }

            result = write_record(destination, destination_slot++, record);
            if (result != ESP_OK)
            {
                return result;
            }

            destination_samples += record.sample_count;
            if (record.is_unknown != 0)
            {
                ++destination_unknown;
            }
        }

        result = commit_bank_header(destination, active_generation + 1);
        if (result != ESP_OK)
        {
            return result;
        }

        // Destination is committed before source erase, so a power loss during
        // this erase still leaves at least one valid generation.
        result = erase_bank(active_bank);
        if (result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Old offline bank erase failed: %s",
                esp_err_to_name(result));
        }

        if (removed_unknown > 0)
        {
            purged_unknown += removed_unknown;
            ESP_LOGW(
                TAG,
                "Purged %u UNKNOWN offline batches before dropping valid "
                "history",
                static_cast<unsigned>(removed_unknown));
        }

        if (dropped_oldest)
        {
            ++purged_oldest;
            ESP_LOGW(
                TAG,
                "No UNKNOWN batch was available; purged oldest offline batch");
        }

        active_bank = destination;
        ++active_generation;
        allocated_slots = destination_slot;
        queued_batches = destination_slot;
        queued_samples = destination_samples;
        queued_unknown_batches = destination_unknown;
        peeked_slot = -1;

        return ESP_OK;
    }

    esp_err_t ensure_space()
    {
        if (allocated_slots < capacity)
        {
            return ESP_OK;
        }

        return compact_for_space();
    }

} // namespace

namespace tervanix::offline
{

    esp_err_t initialize(bool purge_unknown_first)
    {
        Lock lock;

        purge_unknown_first_policy = purge_unknown_first;

        if (!mutex_handle)
        {
            mutex_handle = xSemaphoreCreateMutex();
            if (!mutex_handle)
            {
                return ESP_ERR_NO_MEM;
            }
        }

        partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, STORAGE_SUBTYPE, STORAGE_LABEL);
        if (!partition)
        {
            ESP_LOGE(TAG, "storage partition not found");
            return ESP_ERR_NOT_FOUND;
        }

        if (partition->size < RESERVED_BYTES + 128 * 1024)
        {
            ESP_LOGE(
                TAG,
                "storage partition too small: %u bytes; offline reserve=%u",
                static_cast<unsigned>(partition->size),
                static_cast<unsigned>(RESERVED_BYTES));
            return ESP_ERR_INVALID_SIZE;
        }

        region_offset = partition->size - RESERVED_BYTES;
        bank_bytes = RESERVED_BYTES / 2;

        if (region_offset % SECTOR_SIZE != 0 || bank_bytes % SECTOR_SIZE != 0)
        {
            return ESP_ERR_INVALID_SIZE;
        }

        record_area_bytes = bank_bytes - SECTOR_SIZE;
        capacity = static_cast<std::uint32_t>(
            record_area_bytes / sizeof(StoredRecord));

        BankHeader headers[2]{};
        bool valid[2]{false, false};

        for (int bank = 0; bank < 2; ++bank)
        {
            if (esp_partition_read(
                    partition,
                    bank_start(bank),
                    &headers[bank],
                    sizeof(headers[bank])) == ESP_OK)
            {
                valid[bank] = header_valid(headers[bank]);
            }
        }

        if (!valid[0] && !valid[1])
        {
            ESP_LOGI(TAG, "Initializing fixed 512 KiB offline flash journal");
            ESP_RETURN_ON_ERROR(erase_bank(0), TAG, "bank0 erase failed");
            ESP_RETURN_ON_ERROR(erase_bank(1), TAG, "bank1 erase failed");
            ESP_RETURN_ON_ERROR(
                commit_bank_header(0, 1), TAG, "bank0 header failed");
            active_bank = 0;
            active_generation = 1;
        } else if (
            valid[0] &&
            (!valid[1] || headers[0].generation >= headers[1].generation))
        {
            active_bank = 0;
            active_generation = headers[0].generation;
        } else
        {
            active_bank = 1;
            active_generation = headers[1].generation;
        }

        ESP_RETURN_ON_ERROR(
            scan_active_bank(), TAG, "offline bank scan failed");

        ESP_LOGI(
            TAG,
            "Offline store ready: fixed=%u KiB active_bank=%d generation=%u "
            "record_bytes=%u capacity=%u allocated=%u",
            static_cast<unsigned>(RESERVED_BYTES / 1024),
            active_bank,
            static_cast<unsigned>(active_generation),
            static_cast<unsigned>(sizeof(StoredRecord)),
            static_cast<unsigned>(capacity),
            static_cast<unsigned>(allocated_slots));

        return ESP_OK;
    }

    esp_err_t enqueue(const Batch &batch)
    {
        if (!partition || batch.sample_count == 0 ||
            batch.sample_count > MAX_BATCH_SAMPLES)
        {
            return ESP_ERR_INVALID_ARG;
        }

        Lock lock;

        ESP_RETURN_ON_ERROR(ensure_space(), TAG, "offline compaction failed");

        const StoredRecord record = encode(batch);
        const esp_err_t result =
            write_record(active_bank, allocated_slots, record);

        if (result == ESP_OK)
        {
            ++allocated_slots;
            ++queued_batches;
            queued_samples += batch.sample_count;
            if (prediction_unknown(batch))
            {
                ++queued_unknown_batches;
            }
        }
        return result;
    }

    esp_err_t peek(Batch &batch)
    {
        if (!partition)
        {
            return ESP_ERR_INVALID_STATE;
        }

        Lock lock;

        for (std::uint32_t slot = 0; slot < allocated_slots; ++slot)
        {
            StoredRecord record{};
            const esp_err_t result = read_record(active_bank, slot, record);

            if (result != ESP_OK)
            {
                return result;
            }

            if (record_valid(record) && record.tombstone == TOMBSTONE_ACTIVE)
            {
                decode(record, batch);
                peeked_slot = static_cast<std::int32_t>(slot);
                return ESP_OK;
            }
        }

        peeked_slot = -1;
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ack_peeked()
    {
        if (!partition)
        {
            return ESP_ERR_INVALID_STATE;
        }

        Lock lock;

        if (peeked_slot < 0)
        {
            return ESP_ERR_INVALID_STATE;
        }

        StoredRecord record{};
        const std::uint32_t slot = static_cast<std::uint32_t>(peeked_slot);

        const esp_err_t read_result = read_record(active_bank, slot, record);
        if (read_result != ESP_OK)
        {
            return read_result;
        }

        const std::uint8_t deleted = TOMBSTONE_DELETED;
        const std::size_t offset = record_offset(active_bank, slot) +
                                   offsetof(StoredRecord, tombstone);

        const esp_err_t result =
            esp_partition_write(partition, offset, &deleted, sizeof(deleted));

        if (result == ESP_OK)
        {
            if (record_valid(record) && record.tombstone == TOMBSTONE_ACTIVE)
            {
                if (queued_batches > 0)
                {
                    --queued_batches;
                }
                queued_samples = queued_samples >= record.sample_count
                                     ? queued_samples - record.sample_count
                                     : 0;
                if (record.is_unknown != 0 && queued_unknown_batches > 0)
                {
                    --queued_unknown_batches;
                }
            }
            peeked_slot = -1;
        }
        return result;
    }

    bool empty()
    {
        Batch ignored{};
        return peek(ignored) == ESP_ERR_NOT_FOUND;
    }

    Stats stats()
    {
        Stats result{};
        if (!partition)
        {
            return result;
        }

        Lock lock;
        result.queued_batches = queued_batches;
        result.queued_samples = queued_samples;
        result.unknown_batches = queued_unknown_batches;
        result.purged_unknown_batches = purged_unknown;
        result.purged_oldest_batches = purged_oldest;
        result.capacity_batches = capacity;
        return result;
    }

} // namespace tervanix::offline

#else

namespace tervanix::offline
{
    esp_err_t initialize(bool) { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t enqueue(const Batch &) { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t peek(Batch &) { return ESP_ERR_NOT_SUPPORTED; }
    esp_err_t ack_peeked() { return ESP_ERR_NOT_SUPPORTED; }
    bool empty() { return true; }
    Stats stats() { return {}; }
} // namespace tervanix::offline

#endif
