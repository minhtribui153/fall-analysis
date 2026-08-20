#include "ble_advertiser.hpp"
#include "board_config.hpp"
#include "device_identity.hpp"
#include "har_inference.hpp"
#include "health_display.hpp"
#include "identity_store.hpp"
#include "model_manager.hpp"
#include "mqtt_publisher.hpp"
#include "offline_store.hpp"
#include "wifi_manager.hpp"

#if TERVANIX_HAS_IMU
#include "imu/imu_sensor.hpp"
#endif

#if defined(TERVANIX_BOARD_ESP32_DEV)
#include "fake_csv_sensor.hpp"
#include "imu/imu_sensor.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{

    constexpr char TAG[] = "tervanix";
    constexpr int MQTT_BATCH_SIZE = 10;
    constexpr int SNTP_SYNC_WAIT_MS = 5000;
    constexpr std::uint16_t MODEL_SERVER_PORT = 8765;
    constexpr std::time_t MIN_VALID_EPOCH = 1704067200; // 2024-01-01T00:00:00Z

    tervanix::identity::IdentityStore identity_store;
    std::atomic_bool time_synced{false};
    std::atomic_bool imu_ready{false};
    std::atomic_bool model_ready{false};
    std::atomic_bool prediction_ready{false};
    tervanix::model::Image active_model{};
    tervanix::har::Prediction latest_prediction{};
    char status_device_id[18]{};
    char status_gateway_ip[16]{};

    esp_err_t initialize_nvs()
    {
        esp_err_t result = nvs_flash_init();
        if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
            result == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            result = nvs_flash_init();
        }
        return result;
    }

    void get_device_id(char *out, std::size_t out_size)
    {
        tervanix::identity::HardwareId id{};
        ESP_ERROR_CHECK(tervanix::identity::get_hardware_id(id));
        tervanix::identity::format_device_id(id, out, out_size);
    }

    void wait_for_registration()
    {
        std::uint32_t wait_seconds = 0;
        while (!tervanix::ble::registered())
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ++wait_seconds;
            if (wait_seconds % 10 == 0)
            {
                ESP_LOGW(
                    TAG,
                    "Waiting for BLE registration before enabling Wi-Fi/MQTT "
                    "(%us)",
                    static_cast<unsigned>(wait_seconds));
            }
        }
        ESP_LOGI(
            TAG,
            "BLE registration verified; enabling Wi-Fi telemetry transport");
    }

    esp_err_t sync_time(const char *ntp_server_ip)
    {
        if (ntp_server_ip == nullptr || ntp_server_ip[0] == '\0')
        {
            ESP_LOGE(TAG, "SNTP server IP is empty");
            return ESP_ERR_INVALID_ARG;
        }

        ESP_LOGI(
            TAG,
            "Synchronizing system time from Raspberry Pi SNTP server %s",
            ntp_server_ip);

        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server_ip);

        const esp_err_t init_result = esp_netif_sntp_init(&config);
        if (init_result != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Failed to initialize SNTP against %s: %s",
                ntp_server_ip,
                esp_err_to_name(init_result));
            return init_result;
        }

        unsigned attempt = 0;
        while (tervanix::wifi::is_connected())
        {
            ++attempt;

            const esp_err_t result =
                esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_SYNC_WAIT_MS));

            const std::time_t now = std::time(nullptr);
            if (result == ESP_OK && now >= MIN_VALID_EPOCH)
            {
                std::tm utc{};
                gmtime_r(&now, &utc);

                char timestamp[32]{};
                std::strftime(
                    timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);

                ESP_LOGI(
                    TAG,
                    "System time synchronized from Raspberry Pi %s: %s",
                    ntp_server_ip,
                    timestamp);
                time_synced.store(true);
                return ESP_OK;
            }

            ESP_LOGW(
                TAG,
                "Waiting for Raspberry Pi SNTP time from %s before MQTT "
                "(attempt %u)",
                ntp_server_ip,
                attempt);
        }

        ESP_LOGE(
            TAG, "Wi-Fi disconnected before SNTP synchronization completed");
        return ESP_ERR_INVALID_STATE;
    }

#if TERVANIX_HAS_IMU
    std::uint32_t reserve_sequence_block()
    {
        constexpr std::uint32_t BLOCK = 1000000U;

        nvs_handle_t handle = 0;
        if (nvs_open("telemetry", NVS_READWRITE, &handle) != ESP_OK)
        {
            return 1;
        }

        std::uint32_t next = 1;
        nvs_get_u32(handle, "seq_next", &next);

        const std::uint32_t reserved_next =
            next > UINT32_MAX - BLOCK ? 1 : next + BLOCK;

        if (nvs_set_u32(handle, "seq_next", reserved_next) == ESP_OK)
        {
            nvs_commit(handle);
        }

        nvs_close(handle);
        return next;
    }

    std::int64_t wall_time_ms()
    {
        timeval now{};
        gettimeofday(&now, nullptr);
        return static_cast<std::int64_t>(now.tv_sec) * 1000LL +
               now.tv_usec / 1000LL;
    }

    void offline_uploader_task(void *)
    {
        while (true)
        {
            if (tervanix::mqtt::is_connected() &&
                !tervanix::mqtt::auth_rejected())
            {
                tervanix::offline::Batch stored{};
                if (tervanix::offline::peek(stored) == ESP_OK)
                {
                    const esp_err_t publish_result =
                        tervanix::mqtt::publish_batch(
                            stored.samples,
                            stored.sample_count,
                            stored.has_prediction ? &stored.prediction
                                                  : nullptr);

                    if (publish_result == ESP_OK)
                    {
                        // The MQTT client accepted the QoS1 message into its
                        // outbox.  Only then tombstone the flash copy.
                        const esp_err_t ack_result =
                            tervanix::offline::ack_peeked();

                        if (ack_result != ESP_OK)
                        {
                            ESP_LOGW(
                                TAG,
                                "Offline replay ack failed: %s",
                                esp_err_to_name(ack_result));
                        }

                        if (stored.has_prediction)
                        {
                            tervanix::mqtt::publish_state(stored.prediction);
                        }

                        // Yield between replay batches so live MQTT traffic and
                        // the sensor task are not starved.
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }

    void sensor_task(void *)
    {
        char device_id[18]{};
        get_device_id(device_id, sizeof(device_id));

        constexpr int MQTT_BATCH_SIZE = tervanix::offline::MAX_BATCH_SAMPLES;
        tervanix::mqtt::TelemetrySample batch[MQTT_BATCH_SIZE]{};
        int batch_count = 0;

        std::uint32_t sequence = reserve_sequence_block();

        tervanix::har::Prediction current_prediction{};
        bool has_current_prediction = false;

        const int configured_period_ms =
            std::max(10, active_model.inference_sample_period_ms);

        const TickType_t sample_period = pdMS_TO_TICKS(configured_period_ms);

        TickType_t wake_time = xTaskGetTickCount();

        ESP_LOGI(
            TAG,
            "Nesso IMU pipeline: %d ms/sample (~%.1f Hz), HAR window=%d ms, "
            "offline flash=%u KiB",
            configured_period_ms,
            1000.0 / configured_period_ms,
            active_model.inference_window_ms,
            static_cast<unsigned>(tervanix::offline::RESERVED_BYTES / 1024));

        while (true)
        {
            if (tervanix::mqtt::auth_rejected())
            {
                ESP_LOGE(
                    TAG,
                    "MQTT bearer token rejected; rebooting into BLE "
                    "registration mode");
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }

            tervanix::sensor::ImuSample raw{};
            if (tervanix::sensor::read_imu(raw) == ESP_OK)
            {
                const std::int64_t uptime_ms = esp_timer_get_time() / 1000LL;

                batch[batch_count] = tervanix::mqtt::TelemetrySample{
                    .imu = raw,
                    .sequence = sequence++,
                    .source_uptime_ms = uptime_ms,
                    .timestamp_ms = wall_time_ms(),
                };
                ++batch_count;

                tervanix::har::Prediction prediction{};
                if (tervanix::har::add_sample(raw, uptime_ms, prediction))
                {
                    latest_prediction = prediction;
                    prediction_ready.store(true);

                    current_prediction = prediction;
                    has_current_prediction = true;

                    if (tervanix::mqtt::is_connected())
                    {
                        const esp_err_t state_result =
                            tervanix::mqtt::publish_state(prediction);

                        if (state_result != ESP_OK &&
                            state_result != ESP_ERR_INVALID_STATE)
                        {
                            ESP_LOGW(
                                TAG,
                                "MQTT state publish failed: %s",
                                esp_err_to_name(state_result));
                        }
                    }
                }

                if (batch_count == MQTT_BATCH_SIZE)
                {
                    esp_err_t publish_result = ESP_ERR_INVALID_STATE;

                    if (tervanix::mqtt::is_connected())
                    {
                        publish_result = tervanix::mqtt::publish_batch(
                            batch,
                            batch_count,
                            has_current_prediction ? &current_prediction
                                                   : nullptr);
                    }

                    if (publish_result != ESP_OK)
                    {
                        tervanix::offline::Batch stored{};
                        stored.sample_count =
                            static_cast<std::uint8_t>(batch_count);

                        std::memcpy(stored.samples, batch, sizeof(batch));

                        stored.has_prediction = has_current_prediction;

                        if (has_current_prediction)
                        {
                            stored.prediction = current_prediction;
                        }

                        const esp_err_t store_result =
                            tervanix::offline::enqueue(stored);

                        if (store_result != ESP_OK)
                        {
                            ESP_LOGE(
                                TAG,
                                "Offline flash enqueue failed: %s",
                                esp_err_to_name(store_result));
                        }
                    }

                    batch_count = 0;
                }
            }

            vTaskDelayUntil(&wake_time, sample_period);
        }
    }
#endif

#if defined(TERVANIX_BOARD_ESP32_DEV)

    std::uint32_t reserve_fake_sequence_block()
    {
        constexpr std::uint32_t BLOCK = 1000000U;

        nvs_handle_t handle = 0;

        if (nvs_open("fake_csv", NVS_READWRITE, &handle) != ESP_OK)
        {
            return 1;
        }

        std::uint32_t next = 1;

        nvs_get_u32(handle, "seq_next", &next);

        const std::uint32_t reserved_next =
            next > UINT32_MAX - BLOCK ? 1 : next + BLOCK;

        if (nvs_set_u32(handle, "seq_next", reserved_next) == ESP_OK)
        {
            nvs_commit(handle);
        }

        nvs_close(handle);

        return next;
    }

    std::int64_t fake_wall_time_ms()
    {
        timeval now{};

        gettimeofday(&now, nullptr);

        return static_cast<std::int64_t>(now.tv_sec) * 1000LL +
               now.tv_usec / 1000LL;
    }

    void fake_csv_task(void *)
    {
        constexpr int BATCH_SIZE = 10;

        tervanix::mqtt::TelemetrySample batch[BATCH_SIZE]{};

        int batch_count = 0;

        std::uint32_t sequence = reserve_fake_sequence_block();

        // New wall-clock baseline for each replay cycle.
        std::int64_t playback_epoch_ms = 0;

        tervanix::har::Prediction current_prediction{};
        bool has_current_prediction = false;

        ESP_LOGI(
            TAG,
            "ESP32 Dev fake recording playback ready: %u CSV rows",
            static_cast<unsigned>(tervanix::fake_sensor::row_count()));

        while (true)
        {
            if (tervanix::mqtt::auth_rejected())
            {
                ESP_LOGE(
                    TAG,
                    "MQTT bearer token rejected; "
                    "rebooting into BLE registration mode");

                vTaskDelay(pdMS_TO_TICKS(2000));

                esp_restart();
            }

            tervanix::fake_sensor::PlaybackSample playback{};

            const esp_err_t read_result =
                tervanix::fake_sensor::read_next(playback);

            if (read_result != ESP_OK)
            {
                ESP_LOGW(
                    TAG,
                    "Fake CSV read failed: %s",
                    esp_err_to_name(read_result));

                vTaskDelay(pdMS_TO_TICKS(100));

                continue;
            }

            if (playback.cycle_start)
            {
                // Ignore historical logged_at values and make the recording
                // start "now".
                playback_epoch_ms = fake_wall_time_ms();

                ESP_LOGI(
                    TAG,
                    "Fake recording cycle started at %lld",
                    static_cast<long long>(playback_epoch_ms));
            } else if (playback.delay_from_previous_ms > 0)
            {
                // Preserve the original recording's relative timing.
                vTaskDelay(pdMS_TO_TICKS(playback.delay_from_previous_ms));
            }

            const std::int64_t uptime_ms = esp_timer_get_time() / 1000LL;

            const std::int64_t generated_timestamp_ms =
                playback_epoch_ms +
                static_cast<std::int64_t>(playback.relative_ms);

            // Feed the fake IMU sample into the SAME HAR/TFLite path used
            // by the Nesso N1.
            tervanix::har::Prediction prediction{};

            if (tervanix::har::add_sample(playback.imu, uptime_ms, prediction))
            {
                current_prediction = prediction;
                has_current_prediction = true;

                latest_prediction = prediction;
                prediction_ready.store(true);

                ESP_LOGI(
                    TAG,
                    "Fake HAR prediction: %s %.3f",
                    prediction.state,
                    prediction.confidence);
            }

            batch[batch_count] = tervanix::mqtt::TelemetrySample{
                .imu = playback.imu,
                .sequence = sequence++,
                .source_uptime_ms = uptime_ms,
                .timestamp_ms = generated_timestamp_ms,
            };

            ++batch_count;

            if (batch_count == BATCH_SIZE)
            {
                const esp_err_t publish_result = tervanix::mqtt::publish_batch(
                    batch,
                    batch_count,
                    has_current_prediction ? &current_prediction : nullptr);

                if (publish_result != ESP_OK)
                {
                    ESP_LOGW(
                        TAG,
                        "Fake CSV MQTT batch publish failed: %s",
                        esp_err_to_name(publish_result));
                }

                batch_count = 0;
            }
        }
    }

#endif

    void status_task(void *)
    {
        while (true)
        {
            const auto mqtt_stats = tervanix::mqtt::stats();
            const auto offline_stats = tervanix::offline::stats();

            ESP_LOGI(
                TAG,
                "BLE=%s registration=%s WiFi=%s TIME=%s MQTT=%s AUTH=%s "
                "MODEL=%s STATE=%s OFFLINE=%u/%u token=%s worker_id=%s "
                "worker=%s node_uuid=%s server_uuid=%s",
                tervanix::ble::registered()
                    ? "COMPLETE"
                    : (tervanix::ble::connected() ? "CONNECTED"
                                                  : "DISCONNECTED"),
                tervanix::ble::registered() ? "REGISTERED" : "WAITING",
                tervanix::wifi::is_connected() ? "CONNECTED" : "DISCONNECTED",
                time_synced.load() ? "SYNCED" : "WAITING",
                tervanix::mqtt::is_connected() ? "CONNECTED" : "DISCONNECTED",
                tervanix::mqtt::auth_rejected()
                    ? "REJECTED"
                    : (tervanix::mqtt::auth_accepted() ? "ACCEPTED"
                                                       : "WAITING"),
                model_ready.load() ? "READY" : "WAITING",
                prediction_ready.load() ? latest_prediction.state : "WAITING",
                static_cast<unsigned>(offline_stats.queued_batches),
                static_cast<unsigned>(offline_stats.capacity_batches),
                identity_store.has_bearer_token() ? "STORED" : "MISSING",
                identity_store.has_worker_context() ? identity_store.worker_id()
                                                    : "MISSING",
                identity_store.has_worker_context()
                    ? identity_store.worker_name()
                    : "MISSING",
                identity_store.has_uuid() ? "STORED" : "MISSING",
                identity_store.has_server_uuid() ? "STORED" : "MISSING");

            const tervanix::health::Snapshot snapshot{
                .ble_connected = tervanix::ble::connected(),
                .registered = tervanix::ble::registered(),
                .wifi_connected = tervanix::wifi::is_connected(),
                .time_synced = time_synced.load(),
                .mqtt_connected = tervanix::mqtt::is_connected(),
                .mqtt_auth_accepted = tervanix::mqtt::auth_accepted(),
                .mqtt_auth_rejected = tervanix::mqtt::auth_rejected(),
                .imu_ready = imu_ready.load(),
                .model_ready = model_ready.load(),
                .prediction_ready = prediction_ready.load(),
                .prediction_state =
                    prediction_ready.load() ? latest_prediction.state : nullptr,
                .prediction_confidence = prediction_ready.load()
                                             ? latest_prediction.confidence
                                             : 0.0f,
                .model_sha256 =
                    model_ready.load() ? active_model.sha256 : nullptr,
                .device_id = status_device_id,
                .gateway_ip = status_gateway_ip,
                .published_batches = mqtt_stats.published_batches,
                .published_samples = mqtt_stats.published_samples,
                .published_states = mqtt_stats.published_states,
                .publish_failures = mqtt_stats.publish_failures,
                .last_publish_ms = mqtt_stats.last_publish_ms,
            };
            tervanix::health::render(snapshot, identity_store);

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

} // namespace

extern "C" void app_main()
{
    ESP_ERROR_CHECK(initialize_nvs());
    ESP_ERROR_CHECK(identity_store.begin());

    esp_chip_info_t chip_info{};
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "Tervanix BLE-registration + Wi-Fi/MQTT firmware starting");
    ESP_LOGI(TAG, "Board: %s", TERVANIX_BOARD_NAME);
    ESP_LOGI(TAG, "CPU cores: %d", chip_info.cores);

    get_device_id(status_device_id, sizeof(status_device_id));

#if defined(TERVANIX_BOARD_NESSO_N1)
    const esp_err_t display_result =
        tervanix::health::initialize(identity_store);
    if (display_result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Nesso N1 health display unavailable: %s",
            esp_err_to_name(display_result));
    }
#endif

    ESP_ERROR_CHECK(tervanix::ble::start_registration_client(identity_store));
    xTaskCreate(status_task, "status", 3072, nullptr, 4, nullptr);

    // BLE has one job: establish/verify the immutable node registration.
    // No sensor sample is ever written to a BLE GATT characteristic.
    wait_for_registration();

    // BLE is only needed to register/verify the node. Stop and deinitialize
    // NimBLE completely before bringing up the Wi-Fi telemetry transport.
    ESP_ERROR_CHECK(tervanix::ble::stop_registration_client());

    ESP_ERROR_CHECK(tervanix::wifi::connect());

    char mqtt_broker_ip[16]{};
    if (!tervanix::wifi::get_gateway_ip(mqtt_broker_ip, sizeof(mqtt_broker_ip)))
    {
        ESP_LOGE(
            TAG, "Could not obtain MQTT broker IP from Wi-Fi default gateway");
        return;
    }

    ESP_LOGI(TAG, "Using Wi-Fi gateway %s as MQTT broker", mqtt_broker_ip);
    std::strncpy(
        status_gateway_ip, mqtt_broker_ip, sizeof(status_gateway_ip) - 1);

    // Telemetry must never start with an unsynchronized wall clock.
    ESP_ERROR_CHECK(sync_time(mqtt_broker_ip));

    if (!identity_store.has_bearer_token() ||
        !identity_store.has_worker_context())
    {
        ESP_LOGE(
            TAG, "Registration completed without worker/session credentials");
        return;
    }

    const std::time_t current_time = std::time(nullptr);
    if (identity_store.token_expires_at() != 0 &&
        current_time >=
            static_cast<std::time_t>(identity_store.token_expires_at()))
    {
        ESP_LOGW(
            TAG,
            "Local bearer session is expired; clearing session credentials and "
            "rebooting to BLE registration");
        identity_store.clear_session_credentials();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    // Model distribution is board-independent.  ESP32Dev can therefore be
    // used to test model manifest/download/integrity before moving to Nesso N1.
    // TFLite inference itself is enabled only on builds with an IMU.
    ESP_LOGI(
        TAG,
        "Checking HAR model on Raspberry Pi %s:%u",
        mqtt_broker_ip,
        static_cast<unsigned>(MODEL_SERVER_PORT));

    while (tervanix::wifi::is_connected())
    {
        const esp_err_t rc = tervanix::model::ensure_current(
            mqtt_broker_ip,
            MODEL_SERVER_PORT,
            status_device_id,
            identity_store.session_id(),
            identity_store.bearer_token(),
            active_model);

        if (rc == ESP_OK)
        {
            model_ready.store(true);
            ESP_LOGI(
                TAG,
                "HAR model verified/downloaded id=%s bytes=%u sha256=%s",
                active_model.model_id,
                static_cast<unsigned>(active_model.size),
                active_model.sha256);

#if defined(TERVANIX_BOARD_NESSO_N1) || defined(TERVANIX_BOARD_ESP32_DEV)
            const esp_err_t har_result =
                tervanix::har::initialize(active_model);

            if (har_result == ESP_OK)
            {
                ESP_LOGI(TAG, "HAR inference initialized");
            } else
            {
                ESP_LOGE(
                    TAG,
                    "HAR initialization failed: %s; staying online "
                    "instead of reboot-looping",
                    esp_err_to_name(har_result));
            }
#else
            ESP_LOGI(
                TAG,
                "HAR model storage verified; inference disabled on this board "
                "because no IMU is configured");
#endif
            break;
        }

        ESP_LOGW(
            TAG,
            "HAR model unavailable/invalid (%s); retrying in 2 seconds",
            esp_err_to_name(rc));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (!model_ready.load())
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi disconnected before a valid HAR model could be obtained");
        return;
    }

    ESP_LOGI(
        TAG,
        "Starting encrypted MQTT worker=%s name=%s session_expiry=%u",
        identity_store.worker_id(),
        identity_store.worker_name(),
        static_cast<unsigned>(identity_store.token_expires_at()));

    ESP_ERROR_CHECK(
        tervanix::mqtt::connect(
            mqtt_broker_ip,
            status_device_id,
            identity_store.worker_id(),
            identity_store.session_id(),
            identity_store.bearer_token()));
    // MQTT may be unavailable even while Wi-Fi/model service is healthy.
    // Do not block sensing on broker availability: samples are written to the
    // fixed flash store until the MQTT client reconnects.
#if TERVANIX_HAS_IMU
    ESP_ERROR_CHECK(
        tervanix::offline::initialize(
            active_model.offline_purge_unknown_first));

    ESP_ERROR_CHECK(tervanix::sensor::initialize_imu());
    imu_ready.store(true);

    xTaskCreate(
        offline_uploader_task, "offline-upload", 6144, nullptr, 4, nullptr);

    // Telemetry transport must not depend on HAR inference being ready.
    // The MQTT/Postgres side only returns auth/status after it receives and
    // authenticates an encrypted telemetry message.  Gating sensor_task on
    // har::ready() created a deadlock:
    //
    //   HAR invalid/not-ready -> no telemetry -> no auth/status -> AUTH=WAITING
    //
    // Always start IMU sampling/telemetry once the IMU is ready.  If HAR is
    // unavailable, har::add_sample() simply returns false and raw IMU samples
    // continue to MQTT/offline flash without a prediction object.
    if (!tervanix::har::ready())
    {
        ESP_LOGW(
            TAG,
            "HAR inference is not ready; starting raw IMU telemetry anyway. "
            "STATE will remain WAITING until HAR initializes successfully.");
    }

    xTaskCreate(sensor_task, "sensor", 8192, nullptr, 5, nullptr);
#elif defined(TERVANIX_BOARD_ESP32_DEV)
    ESP_ERROR_CHECK(tervanix::fake_sensor::initialize());

    imu_ready.store(true);

    xTaskCreate(fake_csv_task, "fake-csv", 6144, nullptr, 5, nullptr);
#else
    ESP_LOGI(
        TAG, "No physical or fake sensor source configured for this board");
#endif
}
