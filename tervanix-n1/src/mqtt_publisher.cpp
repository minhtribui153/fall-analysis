#include "mqtt_publisher.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

namespace
{

    constexpr char TAG[] = "mqtt";
    constexpr std::size_t BROKER_URI_CAPACITY = 64;
    constexpr std::size_t TOPIC_CAPACITY = 128;
    constexpr std::size_t DEVICE_ID_CAPACITY = 18;
    constexpr std::size_t TOKEN_CAPACITY = 64;

    esp_mqtt_client_handle_t client = nullptr;
    std::atomic_bool connected{false};
    std::atomic_bool rejected{false};
    std::atomic_bool accepted{false};
    std::atomic<std::uint64_t> published_batches{0};
    std::atomic<std::uint64_t> published_samples{0};
    std::atomic<std::uint64_t> publish_failures{0};
    std::atomic<std::int64_t> last_publish_ms{0};
    char broker_uri[BROKER_URI_CAPACITY]{};
    char mqtt_device_id[DEVICE_ID_CAPACITY]{};
    char mqtt_bearer_token[TOKEN_CAPACITY]{};
    char auth_status_topic[TOPIC_CAPACITY]{};

    bool
    compact_device_id(const char *device_id, char *out, std::size_t out_size)
    {
        if (device_id == nullptr || out == nullptr || out_size < 13)
        {
            return false;
        }

        std::size_t write_index = 0;
        for (const char *p = device_id; *p != '\0'; ++p)
        {
            if (*p == ':')
            {
                continue;
            }
            if (write_index >= out_size - 1)
            {
                return false;
            }
            out[write_index++] = *p;
        }

        if (write_index != 12)
        {
            return false;
        }

        out[write_index] = '\0';
        return true;
    }

    bool build_topic(
        const char *device_id,
        const char *suffix,
        char *out,
        std::size_t out_size)
    {
        char compact[13]{};
        if (!compact_device_id(device_id, compact, sizeof(compact)))
        {
            return false;
        }

        const int written = std::snprintf(
            out, out_size, "tervanix/devices/%s/%s", compact, suffix);

        return written > 0 && static_cast<std::size_t>(written) < out_size;
    }

    void handle_auth_status(const esp_mqtt_event_handle_t event)
    {
        if (event == nullptr || event->topic == nullptr ||
            event->data == nullptr || event->topic_len <= 0 ||
            event->data_len <= 0)
        {
            return;
        }

        const std::size_t expected_len = std::strlen(auth_status_topic);
        if (static_cast<std::size_t>(event->topic_len) != expected_len ||
            std::memcmp(event->topic, auth_status_topic, expected_len) != 0)
        {
            return;
        }

        cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
        if (root == nullptr)
        {
            ESP_LOGW(TAG, "Invalid MQTT auth-status JSON");
            return;
        }

        const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");

        if (cJSON_IsString(status) && status->valuestring != nullptr)
        {
            if (std::strcmp(status->valuestring, "rejected") == 0)
            {
                rejected.store(true);
                accepted.store(false);
                ESP_LOGE(
                    TAG,
                    "Telemetry authorization rejected by server%s%s",
                    cJSON_IsString(reason) && reason->valuestring != nullptr
                        ? ": "
                        : "",
                    cJSON_IsString(reason) && reason->valuestring != nullptr
                        ? reason->valuestring
                        : "");
            } else if (std::strcmp(status->valuestring, "accepted") == 0)
            {
                accepted.store(true);
                if (rejected.exchange(false))
                {
                    ESP_LOGI(TAG, "Telemetry authorization accepted again");
                }
            }
        }

        cJSON_Delete(root);
    }

    void on_mqtt_event(
        void *,
        esp_event_base_t,
        std::int32_t event_id,
        void *event_data)
    {
        auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);

        switch (event_id)
        {
        case MQTT_EVENT_CONNECTED: {
            connected.store(true);
            ESP_LOGI(TAG, "Connected to %s as %s", broker_uri, mqtt_device_id);

            const int message_id =
                esp_mqtt_client_subscribe(client, auth_status_topic, 1);
            if (message_id < 0)
            {
                ESP_LOGW(TAG, "Could not subscribe to %s", auth_status_topic);
            } else
            {
                ESP_LOGI(TAG, "Subscribed to %s", auth_status_topic);
            }
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            connected.store(false);
            ESP_LOGW(TAG, "Disconnected; ESP-IDF MQTT client will reconnect");
            break;

        case MQTT_EVENT_DATA:
            handle_auth_status(event);
            break;

        case MQTT_EVENT_ERROR:
            connected.store(false);
            ESP_LOGE(TAG, "MQTT error event");
            break;

        default:
            break;
        }
    }

} // namespace

namespace tervanix::mqtt
{

    esp_err_t connect(
        const char *broker_ip,
        const char *device_id,
        const char *bearer_token)
    {
        if (broker_ip == nullptr || broker_ip[0] == '\0' ||
            device_id == nullptr || device_id[0] == '\0' ||
            bearer_token == nullptr || bearer_token[0] == '\0')
        {
            ESP_LOGE(TAG, "MQTT broker/device/token is missing");
            return ESP_ERR_INVALID_ARG;
        }

        if (client != nullptr)
        {
            return ESP_OK;
        }

        if (std::strlen(device_id) >= sizeof(mqtt_device_id) ||
            std::strlen(bearer_token) >= sizeof(mqtt_bearer_token))
        {
            return ESP_ERR_INVALID_SIZE;
        }

        std::strcpy(mqtt_device_id, device_id);
        std::strcpy(mqtt_bearer_token, bearer_token);

        if (!build_topic(
                mqtt_device_id,
                "auth/status",
                auth_status_topic,
                sizeof(auth_status_topic)))
        {
            ESP_LOGE(
                TAG,
                "Invalid device_id for MQTT auth topic: %s",
                mqtt_device_id);
            return ESP_ERR_INVALID_ARG;
        }

        const int uri_len = std::snprintf(
            broker_uri, sizeof(broker_uri), "mqtt://%s", broker_ip);
        if (uri_len <= 0 ||
            static_cast<std::size_t>(uri_len) >= sizeof(broker_uri))
        {
            ESP_LOGE(TAG, "MQTT broker IP is too long: %s", broker_ip);
            return ESP_ERR_INVALID_ARG;
        }

        esp_mqtt_client_config_t config{};
        config.broker.address.uri = broker_uri;
        config.credentials.username = mqtt_device_id;
        config.credentials.authentication.password = mqtt_bearer_token;

        client = esp_mqtt_client_init(&config);
        if (client == nullptr)
        {
            ESP_LOGE(TAG, "esp_mqtt_client_init failed");
            return ESP_FAIL;
        }

        const esp_err_t register_result = esp_mqtt_client_register_event(
            client,
            static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
            on_mqtt_event,
            nullptr);
        if (register_result != ESP_OK)
        {
            esp_mqtt_client_destroy(client);
            client = nullptr;
            return register_result;
        }

        const esp_err_t start_result = esp_mqtt_client_start(client);
        if (start_result != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "esp_mqtt_client_start failed: %s",
                esp_err_to_name(start_result));
            esp_mqtt_client_destroy(client);
            client = nullptr;
            return start_result;
        }

        ESP_LOGI(
            TAG,
            "MQTT client started; broker=%s (default port 1883); bearer auth "
            "enabled",
            broker_uri);
        return ESP_OK;
    }

    bool is_connected() { return connected.load(); }

    bool auth_rejected() { return rejected.load(); }

    bool auth_accepted() { return accepted.load(); }

    Stats stats()
    {
        return Stats{
            .published_batches = published_batches.load(),
            .published_samples = published_samples.load(),
            .publish_failures = publish_failures.load(),
            .last_publish_ms = last_publish_ms.load(),
        };
    }

    esp_err_t publish_batch(
        const TelemetrySample *samples,
        int count,
        const char *device_id)
    {
        if (samples == nullptr || count <= 0 || device_id == nullptr)
        {
            return ESP_ERR_INVALID_ARG;
        }

        if (client == nullptr || !connected.load() || rejected.load())
        {
            return ESP_ERR_INVALID_STATE;
        }

        if (std::strcmp(device_id, mqtt_device_id) != 0)
        {
            ESP_LOGE(
                TAG,
                "Refusing MQTT publish for unexpected device_id=%s",
                device_id);
            return ESP_ERR_INVALID_ARG;
        }

        char topic[TOPIC_CAPACITY]{};
        if (!build_topic(device_id, "telemetry", topic, sizeof(topic)))
        {
            ESP_LOGE(TAG, "Invalid device_id for MQTT topic: %s", device_id);
            return ESP_ERR_INVALID_ARG;
        }

        timeval now{};
        gettimeofday(&now, nullptr);
        const std::int64_t wall_now_ms =
            static_cast<std::int64_t>(now.tv_sec) * 1000LL +
            now.tv_usec / 1000LL;
        const std::int64_t uptime_now_ms = esp_timer_get_time() / 1000LL;

        cJSON *root = cJSON_CreateObject();
        cJSON *readings = cJSON_CreateArray();
        if (root == nullptr || readings == nullptr)
        {
            cJSON_Delete(root);
            cJSON_Delete(readings);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddNumberToObject(root, "schema_version", 1);
        cJSON_AddStringToObject(root, "device_id", device_id);
        cJSON_AddStringToObject(root, "bearer_token", mqtt_bearer_token);
        cJSON_AddItemToObject(root, "readings", readings);

        for (int i = 0; i < count; ++i)
        {
            const TelemetrySample &sample = samples[i];
            const std::int64_t sample_epoch_ms =
                wall_now_ms - (uptime_now_ms - sample.source_uptime_ms);

            cJSON *reading = cJSON_CreateObject();
            if (reading == nullptr)
            {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }

            cJSON_AddNumberToObject(reading, "sequence", sample.sequence);
            cJSON_AddNumberToObject(
                reading,
                "source_uptime_ms",
                static_cast<double>(sample.source_uptime_ms));
            cJSON_AddNumberToObject(
                reading, "ts", static_cast<double>(sample_epoch_ms));
            cJSON_AddNumberToObject(reading, "ax", sample.imu.ax);
            cJSON_AddNumberToObject(reading, "ay", sample.imu.ay);
            cJSON_AddNumberToObject(reading, "az", sample.imu.az);
            cJSON_AddNumberToObject(reading, "gx", sample.imu.gx);
            cJSON_AddNumberToObject(reading, "gy", sample.imu.gy);
            cJSON_AddNumberToObject(reading, "gz", sample.imu.gz);
            cJSON_AddItemToArray(readings, reading);
        }

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        if (payload == nullptr)
        {
            return ESP_ERR_NO_MEM;
        }

        const int message_id =
            esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
        cJSON_free(payload);

        if (message_id < 0)
        {
            publish_failures.fetch_add(1);
            ESP_LOGW(TAG, "MQTT publish failed for %s", topic);
            return ESP_FAIL;
        }

        published_batches.fetch_add(1);
        published_samples.fetch_add(static_cast<std::uint64_t>(count));
        last_publish_ms.store(esp_timer_get_time() / 1000LL);
        return ESP_OK;
    }

} // namespace tervanix::mqtt
