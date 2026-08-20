#include "fake_csv_sensor.hpp"

#if defined(TERVANIX_BOARD_ESP32_DEV)

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "esp_log.h"
#include "esp_spiffs.h"

namespace
{

    constexpr char TAG[] = "fake-csv";

    constexpr char BASE_PATH[] = "/spiffs";
    constexpr char CSV_PATH[] = "/spiffs/fake_imu.csv";

    // Existing ESP32 Dev partition:
    // storage, data, spiffs, ..., 1920K
    constexpr char PARTITION_LABEL[] = "storage";

    constexpr std::size_t LINE_BUFFER_SIZE = 256;

    FILE *csv_file = nullptr;
    std::size_t total_rows = 0;
    bool initialized = false;

    bool next_row_starts_cycle = true;
    std::uint64_t first_device_ts_ms = 0;
    std::uint64_t previous_device_ts_ms = 0;

    bool skip_field(char *&cursor)
    {
        if (!cursor || *cursor == '\0')
        {
            return false;
        }

        char *comma = std::strchr(cursor, ',');

        if (!comma)
        {
            return false;
        }

        cursor = comma + 1;
        return true;
    }

    bool parse_u64_field(char *&cursor, std::uint64_t &value)
    {
        if (!cursor || *cursor == '\0')
        {
            return false;
        }

        errno = 0;

        char *end = nullptr;

        const unsigned long long parsed = std::strtoull(cursor, &end, 10);

        if (end == cursor || errno == ERANGE || *end != ',')
        {
            return false;
        }

        value = static_cast<std::uint64_t>(parsed);

        cursor = end + 1;

        return true;
    }

    bool parse_float_field(char *&cursor, float &value)
    {
        if (!cursor || *cursor == '\0')
        {
            return false;
        }

        errno = 0;

        char *end = nullptr;

        value = std::strtof(cursor, &end);

        if (end == cursor || errno == ERANGE || *end != ',')
        {
            return false;
        }

        cursor = end + 1;

        return true;
    }

    void copy_label(char *cursor, char *output, std::size_t output_size)
    {
        if (!output || output_size == 0)
        {
            return;
        }

        output[0] = '\0';

        if (!cursor)
        {
            return;
        }

        char *end = cursor;

        while (*end != '\0' && *end != '\r' && *end != '\n' && *end != ',')
        {
            ++end;
        }

        const std::size_t length = static_cast<std::size_t>(end - cursor);

        const std::size_t copy_length =
            length < output_size - 1 ? length : output_size - 1;

        std::memcpy(output, cursor, copy_length);

        output[copy_length] = '\0';
    }

    bool parse_row(
        char *line,
        std::uint64_t &device_ts_ms,
        tervanix::sensor::ImuSample &imu,
        char *label,
        std::size_t label_size)
    {
        char *cursor = line;

        // CSV schema:
        //
        // logged_at,
        // mac,
        // device_ts_ms,
        // ax,
        // ay,
        // az,
        // gx,
        // gy,
        // gz,
        // label

        // Ignore historical wall-clock timestamp.
        if (!skip_field(cursor))
        {
            return false;
        }

        // Ignore original MAC.
        if (!skip_field(cursor))
        {
            return false;
        }

        // Keep only for replay timing.
        if (!parse_u64_field(cursor, device_ts_ms))
        {
            return false;
        }

        if (!parse_float_field(cursor, imu.ax) ||
            !parse_float_field(cursor, imu.ay) ||
            !parse_float_field(cursor, imu.az) ||
            !parse_float_field(cursor, imu.gx) ||
            !parse_float_field(cursor, imu.gy) ||
            !parse_float_field(cursor, imu.gz))
        {
            return false;
        }

        copy_label(cursor, label, label_size);

        return true;
    }

    esp_err_t rewind_to_first_data_row()
    {
        if (!csv_file)
        {
            return ESP_ERR_INVALID_STATE;
        }

        std::rewind(csv_file);

        char header[LINE_BUFFER_SIZE]{};

        if (!std::fgets(header, sizeof(header), csv_file))
        {
            return ESP_ERR_INVALID_SIZE;
        }

        next_row_starts_cycle = true;
        first_device_ts_ms = 0;
        previous_device_ts_ms = 0;

        return ESP_OK;
    }

    std::size_t count_data_rows()
    {
        if (!csv_file)
        {
            return 0;
        }

        if (rewind_to_first_data_row() != ESP_OK)
        {
            return 0;
        }

        std::size_t count = 0;

        char line[LINE_BUFFER_SIZE]{};

        while (std::fgets(line, sizeof(line), csv_file))
        {
            if (line[0] != '\0' && line[0] != '\r' && line[0] != '\n')
            {
                ++count;
            }
        }

        rewind_to_first_data_row();

        return count;
    }

} // namespace

namespace tervanix::fake_sensor
{

    esp_err_t initialize()
    {
        if (initialized)
        {
            return ESP_OK;
        }

        esp_vfs_spiffs_conf_t config{};

        config.base_path = BASE_PATH;

        config.partition_label = PARTITION_LABEL;

        config.max_files = 2;

        // Do not erase the uploaded CSV if mounting fails.
        config.format_if_mount_failed = false;

        const esp_err_t mount_result = esp_vfs_spiffs_register(&config);

        if (mount_result != ESP_OK && mount_result != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(
                TAG, "SPIFFS mount failed: %s", esp_err_to_name(mount_result));

            return mount_result;
        }

        std::size_t total = 0;
        std::size_t used = 0;

        if (esp_spiffs_info(PARTITION_LABEL, &total, &used) == ESP_OK)
        {
            ESP_LOGI(
                TAG,
                "SPIFFS storage: used=%u total=%u",
                static_cast<unsigned>(used),
                static_cast<unsigned>(total));
        }

        csv_file = std::fopen(CSV_PATH, "r");

        if (!csv_file)
        {
            ESP_LOGE(TAG, "Could not open %s", CSV_PATH);

            ESP_LOGE(
                TAG,
                "Place fake_imu.csv in data/ and run "
                "'pio run -e esp32_dev -t uploadfs'");

            return ESP_ERR_NOT_FOUND;
        }

        total_rows = count_data_rows();

        if (total_rows == 0)
        {
            std::fclose(csv_file);

            csv_file = nullptr;

            ESP_LOGE(TAG, "fake_imu.csv contains no data rows");

            return ESP_ERR_INVALID_SIZE;
        }

        initialized = true;

        ESP_LOGI(
            TAG,
            "Fake recording ready: %s rows=%u",
            CSV_PATH,
            static_cast<unsigned>(total_rows));

        ESP_LOGI(
            TAG,
            "Ignoring old logged_at values; "
            "device_ts_ms is used only for replay timing");

        return ESP_OK;
    }

    esp_err_t read_next(PlaybackSample &output)
    {
        if (!initialized || !csv_file)
        {
            return ESP_ERR_INVALID_STATE;
        }

        char line[LINE_BUFFER_SIZE]{};

        for (std::size_t attempt = 0; attempt < total_rows + 1; ++attempt)
        {
            if (!std::fgets(line, sizeof(line), csv_file))
            {
                ESP_LOGI(TAG, "Recording finished; restarting playback");

                const esp_err_t rewind_result = rewind_to_first_data_row();

                if (rewind_result != ESP_OK)
                {
                    ESP_LOGE(
                        TAG,
                        "CSV rewind failed: %s",
                        esp_err_to_name(rewind_result));

                    return rewind_result;
                }

                if (!std::fgets(line, sizeof(line), csv_file))
                {
                    return ESP_FAIL;
                }
            }

            std::uint64_t device_ts_ms = 0;

            PlaybackSample sample{};

            if (!parse_row(
                    line,
                    device_ts_ms,
                    sample.imu,
                    sample.label,
                    sizeof(sample.label)))
            {
                ESP_LOGW(TAG, "Skipping malformed CSV row");

                continue;
            }

            if (next_row_starts_cycle)
            {
                first_device_ts_ms = device_ts_ms;

                previous_device_ts_ms = device_ts_ms;

                sample.relative_ms = 0;
                sample.delay_from_previous_ms = 0;
                sample.cycle_start = true;

                next_row_starts_cycle = false;
            } else
            {
                sample.cycle_start = false;

                if (device_ts_ms < previous_device_ts_ms ||
                    device_ts_ms < first_device_ts_ms)
                {
                    ESP_LOGW(
                        TAG,
                        "CSV device timestamp moved backwards; "
                        "starting a new playback timing cycle");

                    first_device_ts_ms = device_ts_ms;

                    previous_device_ts_ms = device_ts_ms;

                    sample.relative_ms = 0;
                    sample.delay_from_previous_ms = 0;
                    sample.cycle_start = true;
                } else
                {
                    sample.relative_ms = device_ts_ms - first_device_ts_ms;

                    const std::uint64_t delta =
                        device_ts_ms - previous_device_ts_ms;

                    sample.delay_from_previous_ms = static_cast<std::uint32_t>(
                        delta > std::numeric_limits<std::uint32_t>::max()
                            ? std::numeric_limits<std::uint32_t>::max()
                            : delta);

                    previous_device_ts_ms = device_ts_ms;
                }
            }

            output = sample;

            return ESP_OK;
        }

        return ESP_FAIL;
    }

    std::size_t row_count() { return total_rows; }

} // namespace tervanix::fake_sensor

#endif
