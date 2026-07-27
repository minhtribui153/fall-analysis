#include "mqtt_publisher.hpp"

#include <cstdint>

#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_timer.h"
#include <sys/time.h>
#include <cstring>

namespace {

    constexpr char TAG[] = "mqtt";

    // Set via 'pio run -t menuconfig' -> Tervanix MQTT, or directly in
    // sdkconfig.nesso_n1. Must be the host's LAN IP, not "mosquitto" --
    // that hostname only resolves inside the docker compose network.
    // Still mqtt:// (unencrypted); switch to mqtts:// once there's a
    // cert to point at.
    constexpr char MQTT_BROKER_URI[] = CONFIG_TERVANIX_MQTT_BROKER_URI;
    constexpr char MQTT_TOPIC[] = "sensor-readings";

    esp_mqtt_client_handle_t client = nullptr;

    void on_mqtt_event(
        void* argument,
        esp_event_base_t event_base,
        std::int32_t event_id,
        void* event_data)
    {
        (void)argument;
        (void)event_base;
        (void)event_data;

        switch (event_id) {
            case MQTT_EVENT_CONNECTED:
                ESP_LOGI(TAG, "Connected to broker");
                break;
            case MQTT_EVENT_DISCONNECTED:
                ESP_LOGW(TAG, "Disconnected from broker -- client will auto-reconnect");
                break;
            case MQTT_EVENT_ERROR:
                ESP_LOGE(TAG, "MQTT error event");
                break;
            default:
                break;
        }
    }

}  // namespace

namespace tervanix::mqtt {

    esp_err_t connect()
    {
        if (std::strstr(MQTT_BROKER_URI, "CHANGE_ME") != nullptr) {
            ESP_LOGW(
                TAG,
                "MQTT_BROKER_URI still has its placeholder value -- set "
                "TERVANIX_MQTT_BROKER_URI via menuconfig"
            );
        }

        esp_mqtt_client_config_t config {};
        config.broker.address.uri = MQTT_BROKER_URI;

        client = esp_mqtt_client_init(&config);
        if (client == nullptr) {
            ESP_LOGE(TAG, "esp_mqtt_client_init failed");
            return ESP_FAIL;
        }

        esp_mqtt_client_register_event(
            client,
            static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
            on_mqtt_event,
            nullptr);

        const esp_err_t result = esp_mqtt_client_start(client);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(result));
            return result;
        }

        ESP_LOGI(TAG, "MQTT client started, connecting to %s", MQTT_BROKER_URI);
        return ESP_OK;
    }

    void publish_batch(
        const tervanix::sensor::ImuSample* samples,
        int count,
        const char* device_id)
    {
        if (client == nullptr || count <= 0) {
            return;
        }

        cJSON* array = cJSON_CreateArray();

        // Real wall-clock epoch milliseconds, via SNTP (main.cpp calls
        // sync_time() once after WiFi connects). If sync hasn't finished
        // yet, gettimeofday() reads as an obviously-wrong ~1970 value
        // rather than a plausible-but-wrong boot-relative one -- easier
        // to spot and filter downstream (`WHERE ts < '2000-01-01'`).
        struct timeval now_tv {};
        gettimeofday(&now_tv, nullptr);
        const std::int64_t now_ms =
            static_cast<std::int64_t>(now_tv.tv_sec) * 1000 +
            now_tv.tv_usec / 1000;

        for (int i = 0; i < count; ++i) {
            cJSON* reading = cJSON_CreateObject();
            cJSON_AddStringToObject(reading, "device_id", device_id);
            cJSON_AddNumberToObject(reading, "ts", static_cast<double>(now_ms - (count - 1 - i) * 10));
            cJSON_AddNumberToObject(reading, "ax", samples[i].ax);
            cJSON_AddNumberToObject(reading, "ay", samples[i].ay);
            cJSON_AddNumberToObject(reading, "az", samples[i].az);
            cJSON_AddNumberToObject(reading, "gx", samples[i].gx);
            cJSON_AddNumberToObject(reading, "gy", samples[i].gy);
            cJSON_AddNumberToObject(reading, "gz", samples[i].gz);
            cJSON_AddItemToArray(array, reading);
        }

        char* payload = cJSON_PrintUnformatted(array);
        if (payload != nullptr) {
            esp_mqtt_client_publish(client, MQTT_TOPIC, payload, 0, 1, 0);
            cJSON_free(payload);
        } else {
            ESP_LOGW(TAG, "Failed to serialize reading batch");
        }

        cJSON_Delete(array);
    }

}  // namespace tervanix::mqtt
