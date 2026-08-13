#include "board_config.hpp"
#include "ble_advertiser.hpp"
#include "device_identity.hpp"
#include "identity_store.hpp"
#include "health_display.hpp"
#include "mqtt_publisher.hpp"
#include "wifi_manager.hpp"

#if TERVANIX_HAS_IMU
#include "imu/imu_sensor.hpp"
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

    constexpr char TAG[] = "tervanix";
    constexpr int SAMPLE_RATE_HZ = 100;
    constexpr int MQTT_BATCH_SIZE = 10;
    constexpr int SNTP_SYNC_WAIT_MS = 5000;
    constexpr std::time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01T00:00:00Z

    tervanix::identity::IdentityStore identity_store;
    std::atomic_bool time_synced {false};
    std::atomic_bool imu_ready {false};
    char status_device_id[18] {};
    char status_gateway_ip[16] {};

    esp_err_t initialize_nvs()
    {
        esp_err_t result = nvs_flash_init();
        if (
            result == ESP_ERR_NVS_NO_FREE_PAGES ||
            result == ESP_ERR_NVS_NEW_VERSION_FOUND
        ) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            result = nvs_flash_init();
        }
        return result;
    }

    void get_device_id(char* out, std::size_t out_size)
    {
        tervanix::identity::HardwareId id {};
        ESP_ERROR_CHECK(tervanix::identity::get_hardware_id(id));
        tervanix::identity::format_device_id(id, out, out_size);
    }

    void wait_for_registration()
    {
        std::uint32_t wait_seconds = 0;
        while (!tervanix::ble::registered()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ++wait_seconds;
            if (wait_seconds % 10 == 0) {
                ESP_LOGW(
                    TAG,
                    "Waiting for BLE registration before enabling Wi-Fi/MQTT (%us)",
                    static_cast<unsigned>(wait_seconds)
                );
            }
        }
        ESP_LOGI(TAG, "BLE registration verified; enabling Wi-Fi telemetry transport");
    }

    esp_err_t sync_time(const char* ntp_server_ip)
    {
        if (ntp_server_ip == nullptr || ntp_server_ip[0] == '\0') {
            ESP_LOGE(TAG, "SNTP server IP is empty");
            return ESP_ERR_INVALID_ARG;
        }

        ESP_LOGI(
            TAG,
            "Synchronizing system time from Raspberry Pi SNTP server %s",
            ntp_server_ip
        );

        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server_ip);

        const esp_err_t init_result = esp_netif_sntp_init(&config);
        if (init_result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to initialize SNTP against %s: %s",
                ntp_server_ip,
                esp_err_to_name(init_result)
            );
            return init_result;
        }

        unsigned attempt = 0;
        while (tervanix::wifi::is_connected()) {
            ++attempt;

            const esp_err_t result = esp_netif_sntp_sync_wait(
                pdMS_TO_TICKS(SNTP_SYNC_WAIT_MS)
            );

            const std::time_t now = std::time(nullptr);
            if (result == ESP_OK && now >= MIN_VALID_EPOCH) {
                std::tm utc {};
                gmtime_r(&now, &utc);

                char timestamp[32] {};
                std::strftime(
                    timestamp,
                    sizeof(timestamp),
                    "%Y-%m-%dT%H:%M:%SZ",
                    &utc
                );

                ESP_LOGI(
                    TAG,
                    "System time synchronized from Raspberry Pi %s: %s",
                    ntp_server_ip,
                    timestamp
                );
                time_synced.store(true);
                return ESP_OK;
            }

            ESP_LOGW(
                TAG,
                "Waiting for Raspberry Pi SNTP time from %s before MQTT (attempt %u)",
                ntp_server_ip,
                attempt
            );
        }

        ESP_LOGE(TAG, "Wi-Fi disconnected before SNTP synchronization completed");
        return ESP_ERR_INVALID_STATE;
    }


    esp_err_t wait_for_mqtt_connection()
    {
        unsigned wait_count = 0;

        while (tervanix::wifi::is_connected()) {
            if (tervanix::mqtt::is_connected()) {
                ESP_LOGI(TAG, "MQTT connection established; telemetry transport ready");
                return ESP_OK;
            }

            vTaskDelay(pdMS_TO_TICKS(100));
            ++wait_count;

            if (wait_count % 50 == 0) {
                ESP_LOGW(TAG, "Waiting for MQTT broker connection at Wi-Fi gateway");
            }
        }

        ESP_LOGE(TAG, "Wi-Fi disconnected before MQTT connection completed");
        return ESP_ERR_INVALID_STATE;
    }

#if TERVANIX_HAS_IMU
    void sensor_task(void*)
    {
        char device_id[18] {};
        get_device_id(device_id, sizeof(device_id));

        tervanix::mqtt::TelemetrySample batch[MQTT_BATCH_SIZE] {};
        int batch_count = 0;
        std::uint32_t sequence = 1;

        const TickType_t sample_period = pdMS_TO_TICKS(1000 / SAMPLE_RATE_HZ);
        TickType_t wake_time = xTaskGetTickCount();

        ESP_LOGI(TAG, "Publishing IMU as MQTT device_id=%s at %d Hz", device_id, SAMPLE_RATE_HZ);

        while (true) {
            if (tervanix::mqtt::auth_rejected()) {
                ESP_LOGE(
                    TAG,
                    "MQTT bearer token rejected; rebooting into BLE registration mode"
                );
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }

            tervanix::sensor::ImuSample raw {};
            if (tervanix::sensor::read_imu(raw) == ESP_OK) {
                batch[batch_count] = tervanix::mqtt::TelemetrySample {
                    .imu = raw,
                    .sequence = sequence++,
                    .source_uptime_ms = esp_timer_get_time() / 1000LL,
                };
                ++batch_count;

                if (batch_count == MQTT_BATCH_SIZE) {
                    const esp_err_t result = tervanix::mqtt::publish_batch(
                        batch,
                        batch_count
                    );

                    if (result == ESP_OK) {
                        batch_count = 0;
                    } else if (result == ESP_ERR_INVALID_STATE) {
                        // Preserve the current small batch while MQTT reconnects.
                        // Sampling continues, but we do not grow an unbounded queue.
                    } else {
                        ESP_LOGW(TAG, "MQTT batch publish failed: %s", esp_err_to_name(result));
                        batch_count = 0;
                    }
                }
            }

            // If a full batch is waiting while MQTT is down, overwrite it with
            // the newest samples rather than writing past the fixed-size buffer.
            if (batch_count >= MQTT_BATCH_SIZE && !tervanix::mqtt::is_connected()) {
                batch_count = 0;
            }

            vTaskDelayUntil(&wake_time, sample_period);
        }
    }
#endif

    void status_task(void*)
    {
        while (true) {
            const auto mqtt_stats = tervanix::mqtt::stats();

            ESP_LOGI(
                TAG,
                "BLE=%s registration=%s WiFi=%s TIME=%s MQTT=%s AUTH=%s token=%s worker_id=%s worker=%s node_uuid=%s server_uuid=%s",
                tervanix::ble::registered()
                    ? "COMPLETE"
                    : (tervanix::ble::connected() ? "CONNECTED" : "DISCONNECTED"),
                tervanix::ble::registered() ? "REGISTERED" : "WAITING",
                tervanix::wifi::is_connected() ? "CONNECTED" : "DISCONNECTED",
                time_synced.load() ? "SYNCED" : "WAITING",
                tervanix::mqtt::is_connected() ? "CONNECTED" : "DISCONNECTED",
                tervanix::mqtt::auth_rejected()
                    ? "REJECTED"
                    : (tervanix::mqtt::auth_accepted() ? "ACCEPTED" : "WAITING"),
                identity_store.has_bearer_token() ? "STORED" : "MISSING",
                identity_store.has_worker_context() ? identity_store.worker_id() : "MISSING",
                identity_store.has_worker_context() ? identity_store.worker_name() : "MISSING",
                identity_store.has_uuid() ? "STORED" : "MISSING",
                identity_store.has_server_uuid() ? "STORED" : "MISSING"
            );

            const tervanix::health::Snapshot snapshot {
                .ble_connected = tervanix::ble::connected(),
                .registered = tervanix::ble::registered(),
                .wifi_connected = tervanix::wifi::is_connected(),
                .time_synced = time_synced.load(),
                .mqtt_connected = tervanix::mqtt::is_connected(),
                .mqtt_auth_accepted = tervanix::mqtt::auth_accepted(),
                .mqtt_auth_rejected = tervanix::mqtt::auth_rejected(),
                .imu_ready = imu_ready.load(),
                .device_id = status_device_id,
                .gateway_ip = status_gateway_ip,
                .published_batches = mqtt_stats.published_batches,
                .published_samples = mqtt_stats.published_samples,
                .publish_failures = mqtt_stats.publish_failures,
                .last_publish_ms = mqtt_stats.last_publish_ms,
            };
            tervanix::health::render(snapshot, identity_store);

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

}  // namespace

extern "C" void app_main()
{
    ESP_ERROR_CHECK(initialize_nvs());
    ESP_ERROR_CHECK(identity_store.begin());

    esp_chip_info_t chip_info {};
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "Tervanix BLE-registration + Wi-Fi/MQTT firmware starting");
    ESP_LOGI(TAG, "Board: %s", TERVANIX_BOARD_NAME);
    ESP_LOGI(TAG, "CPU cores: %d", chip_info.cores);

    get_device_id(status_device_id, sizeof(status_device_id));

#if defined(TERVANIX_BOARD_NESSO_N1)
    const esp_err_t display_result = tervanix::health::initialize(identity_store);
    if (display_result != ESP_OK) {
        ESP_LOGW(TAG, "Nesso N1 health display unavailable: %s", esp_err_to_name(display_result));
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

    char mqtt_broker_ip[16] {};
    if (!tervanix::wifi::get_gateway_ip(mqtt_broker_ip, sizeof(mqtt_broker_ip))) {
        ESP_LOGE(TAG, "Could not obtain MQTT broker IP from Wi-Fi default gateway");
        return;
    }

    ESP_LOGI(TAG, "Using Wi-Fi gateway %s as MQTT broker", mqtt_broker_ip);
    std::strncpy(status_gateway_ip, mqtt_broker_ip, sizeof(status_gateway_ip) - 1);

    // Telemetry must never start with an unsynchronized wall clock.
    ESP_ERROR_CHECK(sync_time(mqtt_broker_ip));

    if (!identity_store.has_bearer_token() || !identity_store.has_worker_context()) {
        ESP_LOGE(TAG, "Registration completed without worker/session credentials");
        return;
    }

    const std::time_t current_time = std::time(nullptr);
    if (
        identity_store.token_expires_at() != 0 &&
        current_time >= static_cast<std::time_t>(identity_store.token_expires_at())
    ) {
        ESP_LOGW(TAG, "Local bearer session is expired; clearing session credentials and rebooting to BLE registration");
        identity_store.clear_session_credentials();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    ESP_LOGI(
        TAG,
        "Starting encrypted MQTT worker=%s name=%s session_expiry=%u",
        identity_store.worker_id(),
        identity_store.worker_name(),
        static_cast<unsigned>(identity_store.token_expires_at())
    );

    ESP_ERROR_CHECK(tervanix::mqtt::connect(
        mqtt_broker_ip,
        status_device_id,
        identity_store.worker_id(),
        identity_store.session_id(),
        identity_store.bearer_token()
    ));
    ESP_ERROR_CHECK(wait_for_mqtt_connection());

#if TERVANIX_HAS_IMU
    ESP_ERROR_CHECK(tervanix::sensor::initialize_imu());
    imu_ready.store(true);
    xTaskCreate(sensor_task, "sensor", 6144, nullptr, 5, nullptr);
#else
    ESP_LOGI(TAG, "No IMU configured for this board; MQTT transport is idle");
#endif
}
