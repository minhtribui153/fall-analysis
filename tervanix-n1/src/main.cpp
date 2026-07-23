#include <cinttypes>

#include "ble_advertiser.hpp"
#include "wifi_manager.hpp"
#include "mqtt_publisher.hpp"
#include "imu_sensor.hpp"

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef TERVANIX_BOARD_NAME
#define TERVANIX_BOARD_NAME "Unknown"
#endif

namespace {

constexpr char TAG[] = "tervanix";
constexpr int SAMPLE_RATE_HZ = 100;
constexpr int BATCH_SIZE = 10;   // publish every 10 samples (10 messages/sec, not 100)

esp_err_t initializeNvs()
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

// Uses the board's own WiFi MAC address as a stable device_id -- same
// identifier convention as devices.identification in the backend schema,
// and doesn't need any separate provisioning step to assign one.
void getDeviceId(char* out, size_t out_size)
{
    std::uint8_t mac[6] {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void sensorSamplingTask(void* parameter)
{
    (void)parameter;

    char device_id[18] {};
    getDeviceId(device_id, sizeof(device_id));
    ESP_LOGI(TAG, "Device ID: %s", device_id);

    tervanix::sensor::ImuSample batch[BATCH_SIZE];
    int batch_index = 0;

    const TickType_t sample_period = pdMS_TO_TICKS(1000 / SAMPLE_RATE_HZ);

    while (true) {
        tervanix::sensor::ImuSample sample {};
        if (tervanix::sensor::read_imu(sample) == ESP_OK) {
            batch[batch_index++] = sample;

            if (batch_index >= BATCH_SIZE) {
                tervanix::mqtt::publish_batch(batch, BATCH_SIZE, device_id);
                batch_index = 0;
            }
        }
        // a failed read just gets skipped rather than retried immediately --
        // worth keeping an eye on the "imu" log tag if this happens a lot,
        // since it usually means an I2C bus issue rather than a one-off blip

        vTaskDelay(sample_period);
    }
}

}  // namespace

extern "C" void app_main()
{
    ESP_ERROR_CHECK(initializeNvs());

    esp_chip_info_t chipInfo {};
    esp_chip_info(&chipInfo);

    ESP_LOGI(TAG, "Tervanix starting");
    ESP_LOGI(TAG, "Board: %s", TERVANIX_BOARD_NAME);
    ESP_LOGI(TAG, "CPU cores: %d", chipInfo.cores);

    // BLE stays up alongside WiFi/MQTT for now -- not strictly needed for
    // the Kafka pipeline anymore since this board publishes directly, but
    // useful to keep for local debugging/config until that's confirmed
    // it's safe to drop.
    ESP_ERROR_CHECK(
        tervanix::ble::start_advertising()
    );

    ESP_ERROR_CHECK(tervanix::wifi::connect());
    ESP_ERROR_CHECK(tervanix::mqtt::connect());
    ESP_ERROR_CHECK(tervanix::sensor::initialize_imu());

    xTaskCreate(sensorSamplingTask, "sensor_sampling", 4096, nullptr, 5, nullptr);

    uint32_t elapsedSeconds = 0;

    while (true) {
        ESP_LOGI(
            TAG,
            "Alive: %" PRIu32 " pings",
            elapsedSeconds++
        );

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
