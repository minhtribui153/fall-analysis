#include "mqtt_publisher.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "har_inference.hpp"
#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mqtt_client.h"
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

namespace
{
    constexpr char TAG[] = "mqtt";

    constexpr size_t URI = 64;
    constexpr size_t TOP = 128;
    constexpr size_t DEV = 18;
    constexpr size_t WID = 13;
    constexpr size_t TOK = 64;
    constexpr size_t SID = 37;

    esp_mqtt_client_handle_t client = nullptr;

    std::atomic_bool connected{false};
    std::atomic_bool rejected{false};
    std::atomic_bool accepted{false};
    std::atomic<uint64_t> batches{0};
    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> states{0};
    std::atomic<uint64_t> fails{0};
    std::atomic<int64_t> lastpub{0};

    char uri[URI]{};
    char device[DEV]{};
    char worker[WID]{};
    char session[SID]{};
    char token[TOK]{};
    char telemetry[TOP]{};
    char state_topic[TOP]{};
    char auth_topic[TOP]{};

    void
    uuid_text(const tervanix::identity::SessionUuid &v, char *out, size_t n)
    {
        std::snprintf(
            out,
            n,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%"
            "02x",
            v[0],
            v[1],
            v[2],
            v[3],
            v[4],
            v[5],
            v[6],
            v[7],
            v[8],
            v[9],
            v[10],
            v[11],
            v[12],
            v[13],
            v[14],
            v[15]);
    }

    bool topic(const char *suffix, char *out, size_t n)
    {
        char compact[33]{};
        size_t j = 0;

        for (const char *p = session; *p; ++p)
            if (*p != '-')
                compact[j++] = std::toupper((unsigned char)*p);
        int w =
            std::snprintf(out, n, "tervanix/sessions/%s/%s", compact, suffix);

        return w > 0 && (size_t)w < n;
    }

    bool b64(const uint8_t *in, size_t len, char *&out)
    {
        size_t req = 0;
        mbedtls_base64_encode(nullptr, 0, &req, in, len);

        out = (char *)std::malloc(req + 1);
        if (!out)
            return false;

        size_t w = 0;
        if (mbedtls_base64_encode((unsigned char *)out, req + 1, &w, in, len) !=
            0)
        {
            std::free(out);
            out = nullptr;
            return false;
        }

        out[w] = 0;
        return true;
    }

    void key(uint8_t out[32])
    {
        mbedtls_sha256_context c{};
        mbedtls_sha256_init(&c);
        mbedtls_sha256_starts(&c, 0);
        mbedtls_sha256_update(
            &c, (const unsigned char *)token, std::strlen(token));
        mbedtls_sha256_finish(&c, out);
        mbedtls_sha256_free(&c);
    }

    int64_t epoch(int64_t up)
    {
        timeval now{};

        gettimeofday(&now, nullptr);
        int64_t wall = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000,
                cur = esp_timer_get_time() / 1000;

        return wall - (cur - up);
    }

    cJSON *pred_json(const tervanix::har::Prediction &p)
    {
        cJSON *o = cJSON_CreateObject();
        if (!o)
            return nullptr;

        cJSON_AddNumberToObject(
            o, "prediction_sequence", (double)p.prediction_sequence);
        cJSON_AddNumberToObject(
            o, "source_uptime_ms", (double)p.source_uptime_ms);
        cJSON_AddNumberToObject(o, "ts", (double)epoch(p.source_uptime_ms));
        cJSON_AddStringToObject(o, "state", p.state);
        cJSON_AddNumberToObject(o, "confidence", p.confidence);
        cJSON_AddStringToObject(o, "model_sha256", p.model_sha256);
        cJSON_AddStringToObject(o, "source", "nesso_tflite");

        return o;
    }

    esp_err_t encrypt_publish(
        const char *top,
        const char *type,
        cJSON *root,
        int qos,
        bool retain)
    {
        if (!root)
            return ESP_ERR_INVALID_ARG;

        if (!client || !connected.load() || rejected.load())
        {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_STATE;
        }

        char *plain = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        if (!plain)
            return ESP_ERR_NO_MEM;

        size_t plen = std::strlen(plain);

        uint8_t k[32]{};
        key(k);

        uint8_t nonce[12]{}, tag[16]{};
        esp_fill_random(nonce, sizeof(nonce));

        auto *ct = (uint8_t *)std::malloc(plen);
        if (!ct)
        {
            std::memset(k, 0, sizeof(k));
            cJSON_free(plain);
            return ESP_ERR_NO_MEM;
        }

        char aad[128]{};
        int alen = std::snprintf(
            aad, sizeof(aad), "tervanix|v2|%s|%s|%s", session, worker, type);

        mbedtls_gcm_context g{};
        mbedtls_gcm_init(&g);

        int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, k, 256);
        if (rc == 0)
            rc = mbedtls_gcm_crypt_and_tag(
                &g,
                MBEDTLS_GCM_ENCRYPT,
                plen,
                nonce,
                sizeof(nonce),
                (const unsigned char *)aad,
                alen,
                (const unsigned char *)plain,
                ct,
                sizeof(tag),
                tag);

        mbedtls_gcm_free(&g);
        std::memset(k, 0, sizeof(k));

        cJSON_free(plain);

        if (rc != 0)
        {
            std::free(ct);
            fails++;
            return ESP_FAIL;
        }

        char *nb = nullptr, *cb = nullptr, *tb = nullptr;
        bool ok = b64(nonce, sizeof(nonce), nb) && b64(ct, plen, cb) &&
                  b64(tag, sizeof(tag), tb);
        std::free(ct);

        if (!ok)
        {
            std::free(nb);
            std::free(cb);
            std::free(tb);
            fails++;
            return ESP_ERR_NO_MEM;
        }

        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "schema_version", 2);
        cJSON_AddStringToObject(e, "alg", "A256GCM");
        cJSON_AddStringToObject(e, "message_type", type);
        cJSON_AddStringToObject(e, "session_id", session);
        cJSON_AddStringToObject(e, "worker_id", worker);
        cJSON_AddStringToObject(e, "nonce", nb);
        cJSON_AddStringToObject(e, "ciphertext", cb);
        cJSON_AddStringToObject(e, "tag", tb);

        std::free(nb);
        std::free(cb);
        std::free(tb);

        char *payload = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);

        if (!payload)
            return ESP_ERR_NO_MEM;

        int mid = esp_mqtt_client_publish(
            client, top, payload, 0, qos, retain ? 1 : 0);
        cJSON_free(payload);

        if (mid < 0)
        {
            fails++;
            return ESP_FAIL;
        }

        lastpub = esp_timer_get_time() / 1000;
        return ESP_OK;
    }

    void auth(const esp_mqtt_event_handle_t ev)
    {
        if (!ev || !ev->topic || !ev->data)
            return;

        size_t n = std::strlen(auth_topic);
        if ((size_t)ev->topic_len != n ||
            std::memcmp(ev->topic, auth_topic, n) != 0)
            return;

        cJSON *r = cJSON_ParseWithLength(ev->data, ev->data_len);
        if (!r)
            return;

        auto *s = cJSON_GetObjectItemCaseSensitive(r, "status");
        auto *reason = cJSON_GetObjectItemCaseSensitive(r, "reason");
        if (cJSON_IsString(s) && s->valuestring)
        {
            if (std::strcmp(s->valuestring, "rejected") == 0)
            {
                rejected = true;
                accepted = false;
                ESP_LOGE(
                    TAG,
                    "session rejected%s%s",
                    cJSON_IsString(reason) && reason->valuestring ? ": " : "",
                    cJSON_IsString(reason) && reason->valuestring
                        ? reason->valuestring
                        : "");
            } else if (std::strcmp(s->valuestring, "accepted") == 0)
            {
                accepted = true;
                rejected = false;
            }
        }
        cJSON_Delete(r);
    }

    void event(void *, esp_event_base_t, int32_t id, void *data)
    {
        auto *ev = (esp_mqtt_event_handle_t)data;
        switch (id)
        {
        case MQTT_EVENT_CONNECTED:
            connected = true;
            esp_mqtt_client_subscribe(client, auth_topic, 1);
            ESP_LOGI(TAG, "connected session=%s worker=%s", session, worker);
            break;
        case MQTT_EVENT_DISCONNECTED:
            connected = false;
            break;
        case MQTT_EVENT_DATA:
            auth(ev);
            break;
        case MQTT_EVENT_ERROR:
            connected = false;
            break;
        default:
            break;
        }
    }
} // namespace

namespace tervanix::mqtt
{
    esp_err_t connect(
        const char *ip,
        const char *dev,
        const char *wid,
        const identity::SessionUuid &sid,
        const char *bearer)
    {
        if (!ip || !*ip || !dev || !*dev || !wid || !*wid || !bearer ||
            !*bearer)
            return ESP_ERR_INVALID_ARG;

        if (client)
            return ESP_OK;

        if (std::strlen(dev) >= sizeof(device) ||
            std::strlen(wid) >= sizeof(worker) ||
            std::strlen(bearer) >= sizeof(token))
            return ESP_ERR_INVALID_SIZE;

        std::strcpy(device, dev);
        std::strcpy(worker, wid);
        std::strcpy(token, bearer);
        uuid_text(sid, session, sizeof(session));

        if (!topic("telemetry", telemetry, sizeof(telemetry)) ||
            !topic("state", state_topic, sizeof(state_topic)) ||
            !topic("auth/status", auth_topic, sizeof(auth_topic)))
            return ESP_ERR_INVALID_SIZE;

        int w = std::snprintf(uri, sizeof(uri), "mqtt://%s", ip);
        if (w <= 0 || (size_t)w >= sizeof(uri))
            return ESP_ERR_INVALID_SIZE;

        esp_mqtt_client_config_t cfg{};
        cfg.broker.address.uri = uri;
        cfg.credentials.username = session;

        client = esp_mqtt_client_init(&cfg);
        if (!client)
            return ESP_FAIL;

        esp_err_t rc = esp_mqtt_client_register_event(
            client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, event, nullptr);

        if (rc == ESP_OK)
            rc = esp_mqtt_client_start(client);

        if (rc != ESP_OK)
        {
            esp_mqtt_client_destroy(client);
            client = nullptr;
            return rc;
        }

        ESP_LOGI(
            TAG, "MQTT started telemetry=%s state=%s", telemetry, state_topic);

        return ESP_OK;
    }

    bool is_connected() { return connected.load(); }
    bool auth_rejected() { return rejected.load(); }
    bool auth_accepted() { return accepted.load(); }

    Stats stats()
    {
        Stats result{};
        result.published_batches = batches.load();
        result.published_samples = samples.load();
        result.published_states = states.load();
        result.publish_failures = fails.load();
        result.last_publish_ms = lastpub.load();
        return result;
    }

    esp_err_t publish_batch(
        const TelemetrySample *ss,
        int count,
        const har::Prediction *p)
    {
        if (!ss || count <= 0)
            return ESP_ERR_INVALID_ARG;

        cJSON *root = cJSON_CreateObject();
        cJSON *arr = cJSON_CreateArray();
        if (!root || !arr)
        {
            cJSON_Delete(root);
            cJSON_Delete(arr);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(root, "session_id", session);
        cJSON_AddStringToObject(root, "worker_id", worker);
        cJSON_AddStringToObject(root, "device_id", device);
        cJSON_AddItemToObject(root, "readings", arr);

        for (int i = 0; i < count; ++i)
        {
            cJSON *r = cJSON_CreateObject();
            if (!r)
            {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }

            cJSON_AddNumberToObject(r, "sequence", ss[i].sequence);
            cJSON_AddNumberToObject(
                r, "source_uptime_ms", (double)ss[i].source_uptime_ms);
            cJSON_AddNumberToObject(
                r,
                "ts",
                (double)(ss[i].timestamp_ms > 0
                             ? ss[i].timestamp_ms
                             : epoch(ss[i].source_uptime_ms)));
            cJSON_AddNumberToObject(r, "ax", ss[i].imu.ax);
            cJSON_AddNumberToObject(r, "ay", ss[i].imu.ay);
            cJSON_AddNumberToObject(r, "az", ss[i].imu.az);
            cJSON_AddNumberToObject(r, "gx", ss[i].imu.gx);
            cJSON_AddNumberToObject(r, "gy", ss[i].imu.gy);
            cJSON_AddNumberToObject(r, "gz", ss[i].imu.gz);
            cJSON_AddItemToArray(arr, r);
        }

        if (p)
        {
            cJSON *po = pred_json(*p);
            if (!po)
            {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }

            cJSON_AddItemToObject(root, "prediction", po);
        }

        esp_err_t rc = encrypt_publish(telemetry, "telemetry", root, 1, false);
        if (rc == ESP_OK)
        {
            batches++;
            samples += (uint64_t)count;
        }

        return rc;
    }

    esp_err_t publish_state(const har::Prediction &p)
    {
        cJSON *root = cJSON_CreateObject();
        if (!root)
            return ESP_ERR_NO_MEM;

        cJSON_AddStringToObject(root, "session_id", session);
        cJSON_AddStringToObject(root, "worker_id", worker);
        cJSON_AddStringToObject(root, "device_id", device);

        cJSON *po = pred_json(p);
        if (!po)
        {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddItemToObject(root, "prediction", po);

        esp_err_t rc = encrypt_publish(state_topic, "state", root, 1, true);
        if (rc == ESP_OK)
            states++;

        return rc;
    }
} // namespace tervanix::mqtt
