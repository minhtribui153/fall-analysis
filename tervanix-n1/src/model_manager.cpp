#include "model_manager.hpp"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr char TAG[] = "model";
    constexpr char STORAGE_LABEL[] = "storage";
    constexpr std::uint8_t STORAGE_SUBTYPE = 0x82;
    constexpr std::uint32_t HEADER_MAGIC = 0x31585654;
    constexpr std::uint32_t HEADER_VERSION = 1;
    constexpr std::size_t MODEL_OFFSET = 4096, IO_CHUNK = 2048,
                          MANIFEST_MAX = 4096;
    struct StoredHeader {
        std::uint32_t magic, version, model_size;
        std::uint8_t sha256[32];
        char model_id[48];
        std::int32_t sample_rate_hz, window_samples, slide_samples, num_axes;
        float min_confidence;
    };
    struct Manifest {
        char model_id[48]{};
        char sha256[65]{};
        std::size_t size = 0;
        int sample_rate_hz = 25, window_samples = 50, slide_samples = 25,
            num_axes = 8;
        float min_confidence = .60f;
    };
    void uuid_text(
        const tervanix::identity::SessionUuid &v,
        char *out,
        std::size_t n)
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
    void hex(const std::uint8_t *b, std::size_t c, char *out, std::size_t n)
    {
        if (n < c * 2 + 1)
        {
            if (n)
                out[0] = 0;
            return;
        }
        for (std::size_t i = 0; i < c; ++i)
            std::snprintf(out + i * 2, n - i * 2, "%02x", b[i]);
        out[c * 2] = 0;
    }
    bool unhex(const char *s, std::uint8_t *out, std::size_t c)
    {
        if (!s || std::strlen(s) != c * 2)
            return false;
        for (std::size_t i = 0; i < c; ++i)
        {
            unsigned v = 0;
            if (std::sscanf(s + i * 2, "%2x", &v) != 1)
                return false;
            out[i] = static_cast<std::uint8_t>(v);
        }
        return true;
    }
    esp_err_t hmac_hex(const char *token, const char *msg, char out[65])
    {
        auto *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (!info)
            return ESP_FAIL;
        std::uint8_t d[32]{};
        if (mbedtls_md_hmac(
                info,
                reinterpret_cast<const unsigned char *>(token),
                std::strlen(token),
                reinterpret_cast<const unsigned char *>(msg),
                std::strlen(msg),
                d) != 0)
            return ESP_FAIL;
        hex(d, 32, out, 65);
        return ESP_OK;
    }
    bool equal_hex(const char *a, const char *b)
    {
        if (!a || !b || std::strlen(a) != 64 || std::strlen(b) != 64)
            return false;
        unsigned d = 0;
        for (int i = 0; i < 64; ++i)
            d |= std::tolower(static_cast<unsigned char>(a[i])) ^
                 std::tolower(static_cast<unsigned char>(b[i]));
        return d == 0;
    }
    esp_err_t open_get(
        const char *ip,
        std::uint16_t port,
        const char *path,
        const char *dev,
        const tervanix::identity::SessionUuid &sid,
        const char *token,
        esp_http_client_handle_t &client)
    {
        char st[37]{};
        uuid_text(sid, st, sizeof(st));
        char m[192]{};
        int mw = std::snprintf(m, sizeof(m), "v1|GET|%s|%s|%s", path, st, dev);
        if (mw <= 0 || static_cast<std::size_t>(mw) >= sizeof(m))
            return ESP_ERR_INVALID_SIZE;
        char sig[65]{};
        ESP_RETURN_ON_ERROR(hmac_hex(token, m, sig), TAG, "HMAC failed");
        char url[128]{};
        int uw =
            std::snprintf(url, sizeof(url), "http://%s:%u%s", ip, port, path);
        if (uw <= 0 || static_cast<std::size_t>(uw) >= sizeof(url))
            return ESP_ERR_INVALID_SIZE;
        esp_http_client_config_t cfg{};
        cfg.url = url;
        cfg.method = HTTP_METHOD_GET;
        cfg.timeout_ms = 10000;
        cfg.buffer_size = 2048;
        cfg.buffer_size_tx = 1024;
        client = esp_http_client_init(&cfg);
        if (!client)
            return ESP_ERR_NO_MEM;
        esp_http_client_set_header(client, "X-Tervanix-Session", st);
        esp_http_client_set_header(client, "X-Tervanix-Device", dev);
        esp_http_client_set_header(client, "X-Tervanix-Auth", sig);
        esp_err_t rc = esp_http_client_open(client, 0);
        if (rc != ESP_OK)
        {
            esp_http_client_cleanup(client);
            client = nullptr;
            return rc;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200)
        {
            ESP_LOGE(TAG, "%s HTTP %d", path, status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    esp_err_t fetch_manifest(
        const char *ip,
        std::uint16_t port,
        const char *dev,
        const tervanix::identity::SessionUuid &sid,
        const char *token,
        Manifest &mf)
    {
        esp_http_client_handle_t c = nullptr;
        ESP_RETURN_ON_ERROR(
            open_get(ip, port, "/v1/device/model/manifest", dev, sid, token, c),
            TAG,
            "manifest GET failed");

        // Do not put the 4 KiB HTTP response buffer on app_main's stack.
        // ESP-IDF's default main-task stack is small enough that this alone can
        // trip the stack canary before the first model HTTP request completes.
        auto *body = static_cast<char *>(std::malloc(MANIFEST_MAX));
        if (!body)
        {
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            return ESP_ERR_NO_MEM;
        }

        std::size_t used = 0;
        esp_err_t result = ESP_OK;

        while (used + 1 < MANIFEST_MAX)
        {
            const int r = esp_http_client_read(
                c, body + used, static_cast<int>(MANIFEST_MAX - used - 1));

            if (r < 0)
            {
                result = ESP_FAIL;
                break;
            }
            if (r == 0)
            {
                break;
            }
            used += static_cast<std::size_t>(r);
        }

        esp_http_client_close(c);
        esp_http_client_cleanup(c);

        if (result != ESP_OK)
        {
            std::free(body);
            return result;
        }

        body[used] = '\0';

        cJSON *root = cJSON_ParseWithLength(body, used);
        std::free(body);

        if (!root)
        {
            ESP_LOGE(TAG, "manifest JSON parse failed");
            return ESP_ERR_INVALID_ARG;
        }

        auto *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        auto *mac = cJSON_GetObjectItemCaseSensitive(root, "hmac_sha256");

        if (!cJSON_IsString(payload) || !payload->valuestring ||
            !cJSON_IsString(mac) || !mac->valuestring)
        {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        char expected[65]{};
        const esp_err_t hrc = hmac_hex(token, payload->valuestring, expected);
        if (hrc != ESP_OK || !equal_hex(expected, mac->valuestring))
        {
            cJSON_Delete(root);
            ESP_LOGE(TAG, "manifest HMAC invalid");
            return ESP_ERR_INVALID_CRC;
        }

        cJSON *d = cJSON_Parse(payload->valuestring);
        if (!d)
        {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        auto *id = cJSON_GetObjectItemCaseSensitive(d, "model_id");
        auto *sha = cJSON_GetObjectItemCaseSensitive(d, "sha256");
        auto *size = cJSON_GetObjectItemCaseSensitive(d, "size");
        auto *sr = cJSON_GetObjectItemCaseSensitive(d, "sample_rate_hz");
        auto *win = cJSON_GetObjectItemCaseSensitive(d, "window_samples");
        auto *slide = cJSON_GetObjectItemCaseSensitive(d, "slide_samples");
        auto *axes = cJSON_GetObjectItemCaseSensitive(d, "num_axes");
        auto *conf = cJSON_GetObjectItemCaseSensitive(d, "min_confidence");

        bool ok = cJSON_IsString(id) && id->valuestring &&
                  cJSON_IsString(sha) && sha->valuestring &&
                  cJSON_IsNumber(size) && cJSON_IsNumber(sr) &&
                  cJSON_IsNumber(win) && cJSON_IsNumber(slide) &&
                  cJSON_IsNumber(axes) && cJSON_IsNumber(conf);

        if (ok)
        {
            ok = std::strlen(id->valuestring) < sizeof(mf.model_id) &&
                 std::strlen(sha->valuestring) == 64 && size->valuedouble > 0 &&
                 sr->valueint == 25 && win->valueint == 50 &&
                 slide->valueint == 25 && axes->valueint == 8;
        }

        if (ok)
        {
            std::strcpy(mf.model_id, id->valuestring);
            std::strcpy(mf.sha256, sha->valuestring);
            mf.size = static_cast<std::size_t>(size->valuedouble);
            mf.sample_rate_hz = sr->valueint;
            mf.window_samples = win->valueint;
            mf.slide_samples = slide->valueint;
            mf.num_axes = axes->valueint;
            mf.min_confidence = static_cast<float>(conf->valuedouble);
        }

        cJSON_Delete(d);
        cJSON_Delete(root);
        return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *part()
    {
        return esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            static_cast<esp_partition_subtype_t>(STORAGE_SUBTYPE),
            STORAGE_LABEL);
    }
    esp_err_t sha_part(
        const esp_partition_t *p,
        std::size_t off,
        std::size_t size,
        std::uint8_t out[32])
    {
        auto *buf = static_cast<std::uint8_t *>(std::malloc(IO_CHUNK));
        if (!buf)
        {
            return ESP_ERR_NO_MEM;
        }

        mbedtls_sha256_context s{};
        mbedtls_sha256_init(&s);
        mbedtls_sha256_starts(&s, 0);

        esp_err_t result = ESP_OK;
        for (std::size_t x = 0; x < size;)
        {
            const std::size_t n = std::min(IO_CHUNK, size - x);
            result = esp_partition_read(p, off + x, buf, n);
            if (result != ESP_OK)
            {
                break;
            }

            mbedtls_sha256_update(&s, buf, n);
            x += n;
        }

        if (result == ESP_OK)
        {
            mbedtls_sha256_finish(&s, out);
        }

        mbedtls_sha256_free(&s);
        std::free(buf);
        return result;
    }
    bool header_match(const StoredHeader &h, const Manifest &m)
    {
        if (h.magic != HEADER_MAGIC || h.version != HEADER_VERSION ||
            h.model_size != m.size ||
            std::strncmp(h.model_id, m.model_id, sizeof(h.model_id)) != 0)
            return false;
        std::uint8_t e[32]{};
        return unhex(m.sha256, e, 32) && std::memcmp(h.sha256, e, 32) == 0;
    }
    esp_err_t download(
        const esp_partition_t *p,
        const Manifest &m,
        const char *ip,
        std::uint16_t port,
        const char *dev,
        const tervanix::identity::SessionUuid &sid,
        const char *token)
    {
        if (MODEL_OFFSET + m.size > p->size)
        {
            return ESP_ERR_INVALID_SIZE;
        }

        constexpr std::size_t FLASH_ERASE_SIZE = 4096;
        const std::size_t erase =
            ((MODEL_OFFSET + m.size + FLASH_ERASE_SIZE - 1) /
             FLASH_ERASE_SIZE) *
            FLASH_ERASE_SIZE;

        ESP_RETURN_ON_ERROR(
            esp_partition_erase_range(p, 0, erase), TAG, "erase failed");

        esp_http_client_handle_t c = nullptr;
        ESP_RETURN_ON_ERROR(
            open_get(ip, port, "/v1/device/model/binary", dev, sid, token, c),
            TAG,
            "binary GET failed");

        // Keep large network buffers off app_main's stack.
        auto *buf = static_cast<std::uint8_t *>(std::malloc(IO_CHUNK));
        if (!buf)
        {
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            return ESP_ERR_NO_MEM;
        }

        std::size_t written = 0;
        mbedtls_sha256_context s{};
        mbedtls_sha256_init(&s);
        mbedtls_sha256_starts(&s, 0);

        esp_err_t result = ESP_OK;

        while (written < m.size)
        {
            const int r = esp_http_client_read(
                c,
                reinterpret_cast<char *>(buf),
                static_cast<int>(std::min(IO_CHUNK, m.size - written)));

            if (r < 0)
            {
                result = ESP_FAIL;
                break;
            }
            if (r == 0)
            {
                break;
            }

            result = esp_partition_write(p, MODEL_OFFSET + written, buf, r);
            if (result != ESP_OK)
            {
                break;
            }

            mbedtls_sha256_update(&s, buf, static_cast<std::size_t>(r));
            written += static_cast<std::size_t>(r);
        }

        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        std::free(buf);

        std::uint8_t digest[32]{};
        if (result == ESP_OK)
        {
            mbedtls_sha256_finish(&s, digest);
        }
        mbedtls_sha256_free(&s);

        if (result != ESP_OK)
        {
            return result;
        }

        if (written != m.size)
        {
            ESP_LOGE(
                TAG,
                "model download truncated: expected=%u received=%u",
                static_cast<unsigned>(m.size),
                static_cast<unsigned>(written));
            return ESP_ERR_INVALID_SIZE;
        }

        char dh[65]{};
        hex(digest, 32, dh, sizeof(dh));
        if (!equal_hex(dh, m.sha256))
        {
            ESP_LOGE(
                TAG,
                "download SHA-256 mismatch: expected=%s actual=%s",
                m.sha256,
                dh);
            return ESP_ERR_INVALID_CRC;
        }

        StoredHeader h{};
        h.magic = HEADER_MAGIC;
        h.version = HEADER_VERSION;
        h.model_size = m.size;
        std::memcpy(h.sha256, digest, sizeof(digest));
        std::strncpy(h.model_id, m.model_id, sizeof(h.model_id) - 1);
        h.sample_rate_hz = m.sample_rate_hz;
        h.window_samples = m.window_samples;
        h.slide_samples = m.slide_samples;
        h.num_axes = m.num_axes;
        h.min_confidence = m.min_confidence;

        ESP_RETURN_ON_ERROR(
            esp_partition_write(p, 0, &h, sizeof(h)),
            TAG,
            "header write failed");

        ESP_LOGI(
            TAG,
            "downloaded model id=%s bytes=%u sha256=%s",
            m.model_id,
            static_cast<unsigned>(m.size),
            m.sha256);

        return ESP_OK;
    }
    esp_err_t load(
        const esp_partition_t *p,
        const Manifest &m,
        tervanix::model::Image &out)
    {
        auto *data = static_cast<std::uint8_t *>(std::malloc(m.size));
        if (!data)
            return ESP_ERR_NO_MEM;
        esp_err_t rc = esp_partition_read(p, MODEL_OFFSET, data, m.size);
        if (rc != ESP_OK)
        {
            std::free(data);
            return rc;
        }
        out.data = data;
        out.size = m.size;
        std::strncpy(out.sha256, m.sha256, sizeof(out.sha256) - 1);
        std::strncpy(out.model_id, m.model_id, sizeof(out.model_id) - 1);
        out.sample_rate_hz = m.sample_rate_hz;
        out.window_samples = m.window_samples;
        out.slide_samples = m.slide_samples;
        out.num_axes = m.num_axes;
        out.min_confidence = m.min_confidence;
        return ESP_OK;
    }
} // namespace
namespace tervanix::model
{
    esp_err_t ensure_current(
        const char *ip,
        std::uint16_t port,
        const char *dev,
        const identity::SessionUuid &sid,
        const char *token,
        Image &out)
    {
        if (!ip || !*ip || !dev || !*dev || !token || !*token)
            return ESP_ERR_INVALID_ARG;
        release(out);
        Manifest mf{};
        ESP_RETURN_ON_ERROR(
            fetch_manifest(ip, port, dev, sid, token, mf),
            TAG,
            "manifest verification failed");
        const esp_partition_t *p = part();
        if (!p)
        {
            ESP_LOGE(
                TAG,
                "partition %s subtype 0x%02x missing",
                STORAGE_LABEL,
                STORAGE_SUBTYPE);
            return ESP_ERR_NOT_FOUND;
        }
        StoredHeader h{};
        bool local = esp_partition_read(p, 0, &h, sizeof(h)) == ESP_OK &&
                     header_match(h, mf);
        if (local)
        {
            std::uint8_t d[32]{};
            ESP_RETURN_ON_ERROR(
                sha_part(p, MODEL_OFFSET, mf.size, d),
                TAG,
                "local hash failed");
            char hs[65]{};
            hex(d, 32, hs, 65);
            local = equal_hex(hs, mf.sha256);
        }
        if (!local)
        {
            ESP_LOGW(
                TAG,
                "model missing/outdated/corrupt; downloading %s",
                mf.sha256);
            ESP_RETURN_ON_ERROR(
                download(p, mf, ip, port, dev, sid, token),
                TAG,
                "model download failed");
        } else
            ESP_LOGI(TAG, "local model hash matches server: %s", mf.sha256);
        return load(p, mf, out);
    }
    void release(Image &image)
    {
        if (image.data)
        {
            std::free(image.data);
            image.data = nullptr;
        }
        image.size = 0;
        image.sha256[0] = 0;
        image.model_id[0] = 0;
    }
} // namespace tervanix::model
