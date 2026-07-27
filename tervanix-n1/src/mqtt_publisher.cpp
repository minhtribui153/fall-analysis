#include "mqtt_publisher.hpp"

#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_timer.h"
#include <cstdint>

namespace {

    constexpr char TAG[] = "mqtt";

    // TODO: replace with the real broker address once it's up. Using
    // mqtt:// (unencrypted) for now to keep things simple while getting
    // the pipeline working -- switch to mqtts:// once there's a cert to
    // point at, since sensor data going out in the clear over WiFi isn't
    // great long-term.
    constexpr char MQTT_BROKER_URI[] = "mqtt://REPLACE_ME:1883";
    constexpr char MQTT_TOPIC[] = "sensor-readings";

    esp_mqtt_client_handle_t client = nullptr;

    void on_mqtt_event(
        void* argument,
        esp_event_base_t event_base,
        int32_t event_id,
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

        // epoch milliseconds -- esp_timer gives microseconds since boot,
        // not wall-clock time. Real wall-clock timestamps need SNTP set
        // up (not included here yet) so the backend can make sense of
        // when readings actually happened relative to each other across
        // devices/restarts.
        const int64_t now_ms = esp_timer_get_time() / 1000;

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
