#include "mqtt_publisher.hpp"

#include <cstdint>
#include <string>

#include "cJSON.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include <sys/time.h>

namespace
{

    constexpr char TAG[] = "mqtt";

    // Set via 'pio run -t menuconfig' -> Tervanix MQTT, or directly in
    // sdkconfig.nesso_n1. Must be the host's LAN IP, not "mosquitto" --
    // that hostname only resolves inside the docker compose network.
    // Still mqtt:// (unencrypted); switch to mqtts:// once there's a
    // cert to point at.
    constexpr char MQTT_BROKER_URI[] = CONFIG_TERVANIX_MQTT_BROKER_URI;
    constexpr char MQTT_TOPIC[] = "sensor-readings";

    esp_mqtt_client_handle_t client = nullptr;
    bool mqtt_connected = false;

    void on_mqtt_event(
        void *argument,
        esp_event_base_t event_base,
        std::int32_t event_id,
        void *event_data)
    {
        (void)argument;
        (void)event_base;
        (void)event_data;

        switch (event_id)
        {
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;
            ESP_LOGI(TAG, "Connected to broker");
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            ESP_LOGW(
                TAG, "Disconnected from broker -- client will auto-reconnect");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error event");
            break;
        default:
            break;
        }
    }

} // namespace

namespace tervanix::mqtt
{

    esp_err_t connect(const char *broker_host, std::uint16_t broker_port)
    {
        if (broker_host == nullptr || broker_host[0] == '\0')
        {
            return ESP_ERR_INVALID_ARG;
        }

        static char broker_uri[96];

        const int written = std::snprintf(
            broker_uri,
            sizeof(broker_uri),
            "mqtt://%s:%u",
            broker_host,
            static_cast<unsigned>(broker_port));

        if (written < 0 ||
            static_cast<std::size_t>(written) >= sizeof(broker_uri))
        {
            return ESP_ERR_INVALID_SIZE;
        }

        esp_mqtt_client_config_t config{};

        config.broker.address.uri = broker_uri;

        client = esp_mqtt_client_init(&config);

        if (client == nullptr)
        {
            return ESP_ERR_NO_MEM;
        }

        ESP_ERROR_CHECK(esp_mqtt_client_register_event(
            client, MQTT_EVENT_ANY, &on_mqtt_event, nullptr));

        return esp_mqtt_client_start(client);
    }

    void publish_batch(
        const tervanix::telemetry::ImuSample *samples,
        int count,
        const char *device_id)
    {
        if (client == nullptr || count <= 0)
        {
            return;
        }

        cJSON *array = cJSON_CreateArray();

        timeval now_tv{};
        gettimeofday(&now_tv, nullptr);
        const std::int64_t now_ms =
            static_cast<std::int64_t>(now_tv.tv_sec) * 1000 +
            now_tv.tv_usec / 1000;

        for (int i = 0; i < count; ++i)
        {
            cJSON *reading = cJSON_CreateObject();
            cJSON_AddStringToObject(reading, "device_id", device_id);
            cJSON_AddNumberToObject(
                reading,
                "ts",
                static_cast<double>(now_ms - (count - 1 - i) * 10));
            cJSON_AddNumberToObject(reading, "ax", samples[i].ax);
            cJSON_AddNumberToObject(reading, "ay", samples[i].ay);
            cJSON_AddNumberToObject(reading, "az", samples[i].az);
            cJSON_AddNumberToObject(reading, "gx", samples[i].gx);
            cJSON_AddNumberToObject(reading, "gy", samples[i].gy);
            cJSON_AddNumberToObject(reading, "gz", samples[i].gz);
            cJSON_AddItemToArray(array, reading);
        }

        char *payload = cJSON_PrintUnformatted(array);
        if (payload != nullptr)
        {
            esp_mqtt_client_publish(client, MQTT_TOPIC, payload, 0, 1, 0);
            cJSON_free(payload);
        } else
        {
            ESP_LOGW(TAG, "Failed to serialize reading batch");
        }

        cJSON_Delete(array);
    }

    bool connected() { return mqtt_connected; }

    void publish_peer_sample(const telemetry::PeerSample &peer)
    {
        if (client == nullptr || !mqtt_connected)
            return;
        cJSON *reading = cJSON_CreateObject();
        cJSON_AddStringToObject(reading, "device_id", peer.device_id);
        cJSON_AddNumberToObject(
            reading, "ts", static_cast<double>(peer.timestamp_ms));
        cJSON_AddNumberToObject(reading, "ax", peer.sample.ax);
        cJSON_AddNumberToObject(reading, "ay", peer.sample.ay);
        cJSON_AddNumberToObject(reading, "az", peer.sample.az);
        cJSON_AddNumberToObject(reading, "gx", peer.sample.gx);
        cJSON_AddNumberToObject(reading, "gy", peer.sample.gy);
        cJSON_AddNumberToObject(reading, "gz", peer.sample.gz);
        char *payload = cJSON_PrintUnformatted(reading);
        if (payload)
        {
            esp_mqtt_client_publish(client, MQTT_TOPIC, payload, 0, 1, 0);
            cJSON_free(payload);
        }
        cJSON_Delete(reading);
    }

} // namespace tervanix::mqtt
