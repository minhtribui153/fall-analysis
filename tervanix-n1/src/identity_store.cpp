#include "identity_store.hpp"

#include <cstddef>
#include <cstring>

#include "nvs.h"

namespace
{

    constexpr char nvs_namespace[] = "identity";
    constexpr char node_uuid_key[] = "node_uuid";
    constexpr char server_uuid_key[] = "server_uuid";
    constexpr char bearer_token_key[] = "mqtt_token";

    esp_err_t load_optional_uuid(
        nvs_handle_t handle,
        const char *key,
        tervanix::identity::NodeUuid &out,
        bool &present)
    {
        std::size_t size = out.size();
        const esp_err_t result = nvs_get_blob(handle, key, out.data(), &size);

        if (result == ESP_ERR_NVS_NOT_FOUND)
        {
            out.fill(0);
            present = false;
            return ESP_OK;
        }

        if (result != ESP_OK)
        {
            return result;
        }

        if (size != out.size())
        {
            out.fill(0);
            present = false;
            return ESP_ERR_INVALID_SIZE;
        }

        present = true;
        return ESP_OK;
    }

    esp_err_t load_optional_token(
        nvs_handle_t handle,
        tervanix::identity::BearerToken &out,
        bool &present)
    {
        std::size_t size = 0;
        esp_err_t result =
            nvs_get_str(handle, bearer_token_key, nullptr, &size);

        if (result == ESP_ERR_NVS_NOT_FOUND)
        {
            out.fill('\0');
            present = false;
            return ESP_OK;
        }
        if (result != ESP_OK)
        {
            return result;
        }

        if (size <= 1 || size > out.size())
        {
            out.fill('\0');
            present = false;
            return ESP_ERR_INVALID_SIZE;
        }

        result = nvs_get_str(handle, bearer_token_key, out.data(), &size);
        if (result != ESP_OK)
        {
            out.fill('\0');
            present = false;
            return result;
        }

        out.back() = '\0';
        present = true;
        return ESP_OK;
    }

    esp_err_t save_uuid_once_impl(
        const char *key,
        tervanix::identity::NodeUuid &stored,
        bool &present,
        const tervanix::identity::NodeUuid &requested)
    {
        if (present)
        {
            return stored == requested ? ESP_OK : ESP_ERR_INVALID_STATE;
        }

        nvs_handle_t handle{};
        esp_err_t result = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
        if (result != ESP_OK)
        {
            return result;
        }

        result = nvs_set_blob(handle, key, requested.data(), requested.size());
        if (result == ESP_OK)
        {
            result = nvs_commit(handle);
        }

        nvs_close(handle);

        if (result == ESP_OK)
        {
            stored = requested;
            present = true;
        }

        return result;
    }

    esp_err_t clear_uuid_key(
        const char *key,
        tervanix::identity::NodeUuid &stored,
        bool &present)
    {
        nvs_handle_t handle{};
        esp_err_t result = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
        if (result == ESP_ERR_NVS_NOT_FOUND)
        {
            stored.fill(0);
            present = false;
            return ESP_OK;
        }
        if (result != ESP_OK)
        {
            return result;
        }

        result = nvs_erase_key(handle, key);
        if (result == ESP_ERR_NVS_NOT_FOUND)
        {
            result = ESP_OK;
        }
        if (result == ESP_OK)
        {
            result = nvs_commit(handle);
        }

        nvs_close(handle);

        if (result == ESP_OK)
        {
            stored.fill(0);
            present = false;
        }

        return result;
    }

} // namespace

namespace tervanix::identity
{

    esp_err_t IdentityStore::begin()
    {
        nvs_handle_t handle{};
        esp_err_t result = nvs_open(nvs_namespace, NVS_READONLY, &handle);

        if (result == ESP_ERR_NVS_NOT_FOUND)
        {
            uuid_.fill(0);
            server_uuid_.fill(0);
            bearer_token_.fill('\0');
            has_uuid_ = false;
            has_server_uuid_ = false;
            has_bearer_token_ = false;
            return ESP_OK;
        }
        if (result != ESP_OK)
        {
            return result;
        }

        result = load_optional_uuid(handle, node_uuid_key, uuid_, has_uuid_);
        if (result == ESP_OK)
        {
            result = load_optional_uuid(
                handle, server_uuid_key, server_uuid_, has_server_uuid_);
        }
        if (result == ESP_OK)
        {
            result =
                load_optional_token(handle, bearer_token_, has_bearer_token_);
        }

        nvs_close(handle);
        return result;
    }

    bool IdentityStore::has_uuid() const { return has_uuid_; }

    const NodeUuid &IdentityStore::uuid() const { return uuid_; }

    bool IdentityStore::has_server_uuid() const { return has_server_uuid_; }

    const NodeUuid &IdentityStore::server_uuid() const { return server_uuid_; }

    bool IdentityStore::has_bearer_token() const { return has_bearer_token_; }

    const char *IdentityStore::bearer_token() const
    {
        return bearer_token_.data();
    }

    esp_err_t IdentityStore::save_uuid_once(const NodeUuid &uuid)
    {
        return save_uuid_once_impl(node_uuid_key, uuid_, has_uuid_, uuid);
    }

    esp_err_t IdentityStore::save_server_uuid_once(const NodeUuid &uuid)
    {
        return save_uuid_once_impl(
            server_uuid_key, server_uuid_, has_server_uuid_, uuid);
    }

    esp_err_t IdentityStore::save_bearer_token(const char *token)
    {
        if (token == nullptr)
        {
            return ESP_ERR_INVALID_ARG;
        }

        const std::size_t length = std::strlen(token);
        if (length == 0 || length >= bearer_token_.size())
        {
            return ESP_ERR_INVALID_SIZE;
        }

        nvs_handle_t handle{};
        esp_err_t result = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
        if (result != ESP_OK)
        {
            return result;
        }

        result = nvs_set_str(handle, bearer_token_key, token);
        if (result == ESP_OK)
        {
            result = nvs_commit(handle);
        }
        nvs_close(handle);

        if (result == ESP_OK)
        {
            bearer_token_.fill('\0');
            std::memcpy(bearer_token_.data(), token, length);
            has_bearer_token_ = true;
        }

        return result;
    }

    esp_err_t IdentityStore::clear_uuid()
    {
        return clear_uuid_key(node_uuid_key, uuid_, has_uuid_);
    }

    esp_err_t IdentityStore::clear_server_uuid()
    {
        return clear_uuid_key(server_uuid_key, server_uuid_, has_server_uuid_);
    }

    esp_err_t IdentityStore::clear_bearer_token()
    {
        nvs_handle_t handle{};
        esp_err_t result = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
        if (result == ESP_ERR_NVS_NOT_FOUND)
        {
            bearer_token_.fill('\0');
            has_bearer_token_ = false;
            return ESP_OK;
        }
        if (result != ESP_OK)
        {
            return result;
        }

        result = nvs_erase_key(handle, bearer_token_key);
        if (result == ESP_ERR_NVS_NOT_FOUND)
        {
            result = ESP_OK;
        }
        if (result == ESP_OK)
        {
            result = nvs_commit(handle);
        }
        nvs_close(handle);

        if (result == ESP_OK)
        {
            bearer_token_.fill('\0');
            has_bearer_token_ = false;
        }

        return result;
    }

} // namespace tervanix::identity
