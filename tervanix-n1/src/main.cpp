#include "ble_advertiser.hpp"
#include "identity_store.hpp"
#include "imu/imu_sensor.hpp"
#include "telemetry_protocol.hpp"

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char TAG[] = "tervanix";
constexpr int SAMPLE_RATE_HZ = 100;
constexpr int BATCH_SIZE =
    static_cast<int>(tervanix::protocol::telemetry_samples_per_packet);

tervanix::identity::IdentityStore identity_store;

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

void wait_for_ble_registration()
{
    ESP_LOGI(TAG, "Waiting for BLE gateway registration");

    while (!tervanix::ble::registered()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "BLE registration confirmed; telemetry can start");
}

void sensorSamplingTask(void* parameter)
{
    (void)parameter;

    tervanix::sensor::ImuSample batch[BATCH_SIZE];
    int batch_index = 0;

    const TickType_t sample_period =
        pdMS_TO_TICKS(1000 / SAMPLE_RATE_HZ);

    while (true) {
        tervanix::sensor::ImuSample sample {};

        if (tervanix::sensor::read_imu(sample) == ESP_OK) {
            batch[batch_index++] = sample;

            if (batch_index >= BATCH_SIZE) {
                if (tervanix::ble::telemetry_ready()) {
                    const esp_err_t result =
                        tervanix::ble::send_telemetry_batch(
                            batch,
                            BATCH_SIZE
                        );

                    if (
                        result != ESP_OK &&
                        result != ESP_ERR_INVALID_STATE
                    ) {
                        ESP_LOGW(
                            TAG,
                            "BLE telemetry send failed: %s",
                            esp_err_to_name(result)
                        );
                    }
                }

                // If BLE is temporarily disconnected, this batch is dropped.
                // The client automatically scans/reconnects and resumes sending
                // after registration is confirmed again.
                batch_index = 0;
            }
        }

        vTaskDelay(sample_period);
    }
}

void statusTask(void* parameter)
{
    (void)parameter;

    while (true) {
        ESP_LOGI(
            TAG,
            "BLE=%s registration=%s telemetry=%s stored_uuid=%s",
            tervanix::ble::connected()
                ? "CONNECTED"
                : "DISCONNECTED",
            tervanix::ble::registered()
                ? "REGISTERED"
                : "WAITING",
            tervanix::ble::telemetry_ready()
                ? "READY"
                : "PAUSED",
            identity_store.has_uuid()
                ? "YES"
                : "NO"
        );

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

}  // namespace

extern "C" void app_main()
{
    ESP_ERROR_CHECK(initialize_nvs());
    ESP_ERROR_CHECK(identity_store.begin());

    esp_chip_info_t chip_info {};
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "Tervanix BLE telemetry firmware starting");
    ESP_LOGI(TAG, "Board: %s", TERVANIX_BOARD_NAME);
    ESP_LOGI(TAG, "CPU cores: %d", chip_info.cores);
    ESP_LOGI(
        TAG,
        "Stored UUID present: %s",
        identity_store.has_uuid() ? "yes" : "no"
    );

    // The Nesso is now BLE-only. It connects to TVX-REG, registers, and
    // sends IMU packets to the gateway. The gateway handles MQTT/Kafka.
    ESP_ERROR_CHECK(
        tervanix::ble::start_registration_client(identity_store)
    );

    xTaskCreate(statusTask, "status", 3072, nullptr, 4, nullptr);

    wait_for_ble_registration();

    ESP_ERROR_CHECK(tervanix::sensor::initialize_imu());

    xTaskCreate(
        sensorSamplingTask,
        "sensor_sampling",
        4096,
        nullptr,
        5,
        nullptr
    );
}
