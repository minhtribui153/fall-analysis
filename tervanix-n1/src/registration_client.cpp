#include "registration_client.hpp"
#include "cJSON.h"
#include "device_identity.hpp"
#include "esp_http_client.h"
#include "esp_log.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stddef.h>
#include <string>

namespace
{
    constexpr char TAG[] = "wifi-register";

    struct HttpBuffer {
        std::string body;
    };

    esp_err_t event_handler(esp_http_client_event_t *e)
    {
        if (e->event_id == HTTP_EVENT_ON_DATA && e->data && e->data_len > 0)
            static_cast<HttpBuffer *>(e->user_data)
                ->body.append(static_cast<const char *>(e->data), e->data_len);
        return ESP_OK;
    }

    std::string uuid_text(const tervanix::identity::NodeUuid &u)
    {
        char out[37];
        std::snprintf(
            out,
            sizeof(out),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%"
            "02x",
            u[0],
            u[1],
            u[2],
            u[3],
            u[4],
            u[5],
            u[6],
            u[7],
            u[8],
            u[9],
            u[10],
            u[11],
            u[12],
            u[13],
            u[14],
            u[15]);
        return std::string(out);
    }

    bool parse_uuid(const char *s, tervanix::identity::NodeUuid &out)
    {
        unsigned v[16];
        if (!s || sscanf(
                      s,
                      "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
                      &v[0],
                      &v[1],
                      &v[2],
                      &v[3],
                      &v[4],
                      &v[5],
                      &v[6],
                      &v[7],
                      &v[8],
                      &v[9],
                      &v[10],
                      &v[11],
                      &v[12],
                      &v[13],
                      &v[14],
                      &v[15]) != 16)
            return false;
        for (int i = 0; i < 16; i++)
            out[i] = static_cast<uint8_t>(v[i]);
        return true;
    }

    esp_err_t request(
        const char *url,
        esp_http_client_method_t method,
        const char *body,
        HttpBuffer &response,
        const char *dev = nullptr,
        const char *node = nullptr,
        const char *server = nullptr)
    {
        esp_http_client_config_t cfg{};
        cfg.url = url;
        cfg.event_handler = event_handler;
        cfg.user_data = &response;
        cfg.timeout_ms = 10000;

        auto client = esp_http_client_init(&cfg);
        if (!client)
            return ESP_ERR_NO_MEM;
        esp_http_client_set_method(client, method);

        if (body)
        {
            esp_http_client_set_header(
                client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, body, std::strlen(body));
        }

        if (dev)
            esp_http_client_set_header(client, "X-Tervanix-Device-ID", dev);
        if (node)
            esp_http_client_set_header(client, "X-Tervanix-Node-UUID", node);
        if (server)
            esp_http_client_set_header(
                client, "X-Tervanix-Server-UUID", server);
        auto r = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (r != ESP_OK)
            return r;
        return status >= 200 && status < 300 ? ESP_OK
                                             : ESP_ERR_INVALID_RESPONSE;
    }
} // namespace

namespace tervanix::registration
{
    esp_err_t verify_over_wifi(
        identity::IdentityStore &identity_store,
        registry::RegistryStore &registry_store,
        const char *server_ip,
        uint16_t server_port)
    {
        char register_url[96]{};
        char registry_url[96]{};

        int result = std::snprintf(
            register_url,
            sizeof(register_url),
            "http://%s:%u/v1/register",
            server_ip,
            static_cast<unsigned>(server_port));

        int result2 = std::snprintf(
            registry_url,
            sizeof(registry_url),
            "http://%s:%u/v1/registry",
            server_ip,
            static_cast<unsigned>(server_port));

        if (result < 0 || static_cast<size_t>(result) >= sizeof(register_url) ||
            result2 < 0 || static_cast<size_t>(result2) >= sizeof(registry_url))
        {
            return ESP_ERR_INVALID_SIZE;
        }

        identity::HardwareId raw{};

        auto r = identity::get_hardware_id(raw);

        if (r != ESP_OK)
            return r;

        char device[18]{};
        identity::format_device_id(raw, device, sizeof(device));

        std::string node =
            identity_store.has_uuid() ? uuid_text(identity_store.uuid()) : "";
        std::string server = identity_store.has_server_uuid()
                                 ? uuid_text(identity_store.server_uuid())
                                 : "";

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "device_id", device);

        if (!node.empty())
            cJSON_AddStringToObject(root, "current_uuid", node.c_str());
        if (!server.empty())
            cJSON_AddStringToObject(root, "server_uuid", server.c_str());

        char *encoded = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!encoded)
            return ESP_ERR_NO_MEM;

        HttpBuffer reg_response;
        r = request(register_url, HTTP_METHOD_POST, encoded, reg_response);

        cJSON_free(encoded);

        if (r != ESP_OK)
        {
            ESP_LOGE(TAG, "Registration rejected or unavailable");
            return r;
        }

        cJSON *reg = cJSON_Parse(reg_response.body.c_str());

        if (!reg)
            return ESP_ERR_INVALID_RESPONSE;

        auto status = cJSON_GetObjectItemCaseSensitive(reg, "status");
        auto reason = cJSON_GetObjectItemCaseSensitive(reg, "reason");
        auto node_json = cJSON_GetObjectItemCaseSensitive(reg, "node_uuid");
        auto server_json = cJSON_GetObjectItemCaseSensitive(reg, "server_uuid");

        if (!status || !cJSON_IsNumber(status))
        {
            cJSON_Delete(reg);
            return ESP_ERR_INVALID_RESPONSE;
        }

        const int status_value = status->valueint;
        const char *reason_text =
            cJSON_IsString(reason) ? reason->valuestring : "unspecified";

        if (status_value == 5)
        {
            ESP_LOGW(
                TAG, "Registration pending admin approval: %s", reason_text);
            cJSON_Delete(reg);
            return ESP_ERR_NOT_FINISHED;
        }

        if (status_value == 3 || status_value == 4 || status_value == 6)
        {
            ESP_LOGE(
                TAG,
                "Registration rejected status=%d reason=%s",
                status_value,
                reason_text);
            cJSON_Delete(reg);
            return ESP_ERR_INVALID_STATE;
        }

        identity::NodeUuid received_node{}, received_server{};
        const bool valid =
            (status_value == 1 || status_value == 2) &&
            cJSON_IsString(node_json) && cJSON_IsString(server_json) &&
            parse_uuid(node_json->valuestring, received_node) &&
            parse_uuid(server_json->valuestring, received_server);
        cJSON_Delete(reg);

        if (!valid)
            return ESP_ERR_INVALID_RESPONSE;
        if ((r = identity_store.save_uuid_once(received_node)) != ESP_OK)
            return r;
        if ((r = identity_store.save_server_uuid_once(received_server)) !=
            ESP_OK)
            return r;

        node = uuid_text(identity_store.uuid());
        server = uuid_text(identity_store.server_uuid());

        HttpBuffer list_response;
        r = request(
            registry_url,
            HTTP_METHOD_GET,
            nullptr,
            list_response,
            device,
            node.c_str(),
            server.c_str());
        if (r != ESP_OK)
            return r;
        cJSON *doc = cJSON_Parse(list_response.body.c_str());
        if (!doc)
            return ESP_ERR_INVALID_RESPONSE;
        auto server_check =
            cJSON_GetObjectItemCaseSensitive(doc, "server_uuid");
        identity::NodeUuid parsed_server{};
        if (!cJSON_IsString(server_check) ||
            !parse_uuid(server_check->valuestring, parsed_server) ||
            parsed_server != identity_store.server_uuid())
        {
            cJSON_Delete(doc);
            return ESP_ERR_INVALID_STATE;
        }
        auto nodes = cJSON_GetObjectItemCaseSensitive(doc, "nodes");
        if (!cJSON_IsArray(nodes))
        {
            cJSON_Delete(doc);
            return ESP_ERR_INVALID_RESPONSE;
        }
        std::array<identity::NodeUuid, registry::max_nodes> accepted{};
        size_t count = 0;
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, nodes)
        {
            if (count >= accepted.size())
                break;
            auto value = cJSON_GetObjectItemCaseSensitive(item, "node_uuid");
            if (cJSON_IsString(value) &&
                parse_uuid(value->valuestring, accepted[count]))
                count++;
        }
        cJSON_Delete(doc);
        r = registry_store.replace(accepted.data(), count);
        if (r == ESP_OK)
            ESP_LOGI(
                TAG,
                "Verified registrar and stored %u peer UUIDs",
                static_cast<unsigned>(count));
        return r;
    }
} // namespace tervanix::registration
