#include "ble_advertiser.hpp"
#include "identity_store.hpp"

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

namespace {

constexpr char TAG[] = "tervanix";
constexpr int SAMPLE_RATE_HZ = 100;
constexpr int BATCH_SIZE = 10;   // publish every 10 samples (10 messages/sec, not 100)

// How long to wait for BLE registration to finish before giving up and
// falling back to whatever UUID (if any) is already stored in NVS from a
// previous run. This matters now more than it used to: the Kafka ->
// Postgres side of the pipeline (kafka_to_postgres.py) looks up this
// device's node_uuid from node_registrations, which only has a row once
// BLE registration has completed at least once -- readings published
// before that will just get dropped with a warning on the consumer side.
constexpr int BLE_REGISTRATION_TIMEOUT_MS = 20000;
constexpr int BLE_POLL_INTERVAL_MS = 200;

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

// Uses the board's own WiFi MAC address as a stable device_id -- same
// identifier convention (and now the same column name) as
// devices.identification.device_id / node_registrations.device_id in the
// backend schema, and doesn't need any separate provisioning step to
// assign one.
void getDeviceId(char* out, size_t out_size)
{
    std::uint8_t mac[6] {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Blocks (with a bounded timeout) until BLE registration finishes, so
// WiFi/MQTT never start publishing readings under a device_id that
// node_registrations doesn't know about yet -- BLE registration has to
// happen first. If a UUID is already stored from a previous boot, it's
// fine to fall through on timeout (the registration server just
// re-confirms it rather than minting a new one), but a first-ever boot
// with no stored UUID and no successful registration means readings from
// this session won't have anywhere to land in Postgres.
void wait_for_ble_registration()
{
    int waited_ms = 0;

    while (
        !tervanix::ble::registered() &&
        waited_ms < BLE_REGISTRATION_TIMEOUT_MS
    ) {
        vTaskDelay(pdMS_TO_TICKS(BLE_POLL_INTERVAL_MS));
        waited_ms += BLE_POLL_INTERVAL_MS;
    }

    if (tervanix::ble::registered()) {
        ESP_LOGI(TAG, "BLE registration confirmed");
        return;
    }

    if (identity_store.has_uuid()) {
        ESP_LOGW(
            TAG,
            "BLE registration timed out after %dms; continuing with "
            "previously stored UUID",
            BLE_REGISTRATION_TIMEOUT_MS
        );
        return;
    }

    ESP_LOGE(
        TAG,
        "BLE registration timed out after %dms and no UUID is stored yet "
        "-- readings published this session won't match a row in "
        "node_registrations until registration succeeds",
        BLE_REGISTRATION_TIMEOUT_MS
    );
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

void statusTask(void* parameter)
{
    (void)parameter;

    while (true) {
        ESP_LOGI(
            TAG,
            "BLE=%s registration=%s stored_uuid=%s",
            tervanix::ble::connected()
                ? "CONNECTED"
                : "DISCONNECTED",
            tervanix::ble::registered()
                ? "REGISTERED"
                : "WAITING",
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

    ESP_LOGI(TAG, "Tervanix starting");
    ESP_LOGI(TAG, "Board: %s", TERVANIX_BOARD_NAME);
    ESP_LOGI(TAG, "CPU cores: %d", chip_info.cores);
    ESP_LOGI(
        TAG,
        "Stored UUID present: %s",
        identity_store.has_uuid() ? "yes" : "no"
    );

    // BLE registration goes first, on purpose: it's what assigns (or
    // re-confirms) this device's UUID in node_registrations on the
    // registration server, which the Kafka -> Postgres side of the
    // pipeline now depends on to accept readings for this device_id.
    // WiFi, MQTT, and sensor sampling only start once that's settled (or
    // we've given up waiting and fallen back to a previously stored
    // UUID -- see wait_for_ble_registration()).
    ESP_ERROR_CHECK(
        tervanix::ble::start_registration_client(identity_store)
    );

    xTaskCreate(statusTask, "status", 3072, nullptr, 4, nullptr);

    wait_for_ble_registration();

    ESP_ERROR_CHECK(tervanix::wifi::connect());
    ESP_ERROR_CHECK(tervanix::mqtt::connect());
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
