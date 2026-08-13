#include "identity_store.hpp"

#include <cstring>

#include "nvs.h"

namespace
{
    constexpr char ns[] = "identity";
    constexpr char worker_uuid_key[] = "node_uuid";
    constexpr char server_uuid_key[] = "server_uuid";
    constexpr char token_key[] = "mqtt_token";
    constexpr char worker_id_key[] = "worker_id";
    constexpr char worker_name_key[] = "worker_name";
    constexpr char session_id_key[] = "session_id";
    constexpr char expires_key[] = "token_exp";

    template <std::size_t N>
    esp_err_t load_blob(
        nvs_handle_t h,
        const char *key,
        std::array<std::uint8_t, N> &out,
        bool &present)
    {
        std::size_t size = out.size();
        const esp_err_t rc = nvs_get_blob(h, key, out.data(), &size);
        if (rc == ESP_ERR_NVS_NOT_FOUND)
        {
            out.fill(0);
            present = false;
            return ESP_OK;
        }
        if (rc != ESP_OK)
            return rc;
        if (size != out.size())
        {
            out.fill(0);
            present = false;
            return ESP_ERR_INVALID_SIZE;
        }
        present = true;
        return ESP_OK;
    }

    template <std::size_t N>
    esp_err_t load_string(
        nvs_handle_t h,
        const char *key,
        std::array<char, N> &out,
        bool &present)
    {
        std::size_t size = 0;
        esp_err_t rc = nvs_get_str(h, key, nullptr, &size);
        if (rc == ESP_ERR_NVS_NOT_FOUND)
        {
            out.fill('\0');
            present = false;
            return ESP_OK;
        }
        if (rc != ESP_OK)
            return rc;
        if (size <= 1 || size > out.size())
        {
            out.fill('\0');
            present = false;
            return ESP_ERR_INVALID_SIZE;
        }
        rc = nvs_get_str(h, key, out.data(), &size);
        if (rc == ESP_OK)
        {
            out.back() = '\0';
            present = true;
        }
        return rc;
    }

    esp_err_t open_rw(nvs_handle_t &h)
    {
        return nvs_open(ns, NVS_READWRITE, &h);
    }

    esp_err_t erase_key_commit(const char *key)
    {
        nvs_handle_t h{};
        esp_err_t rc = open_rw(h);
        if (rc == ESP_ERR_NVS_NOT_FOUND)
            return ESP_OK;
        if (rc != ESP_OK)
            return rc;
        rc = nvs_erase_key(h, key);
        if (rc == ESP_ERR_NVS_NOT_FOUND)
            rc = ESP_OK;
        if (rc == ESP_OK)
            rc = nvs_commit(h);
        nvs_close(h);
        return rc;
    }
} // namespace

namespace tervanix::identity
{

    esp_err_t IdentityStore::begin()
    {
        nvs_handle_t h{};
        esp_err_t rc = nvs_open(ns, NVS_READONLY, &h);
        if (rc == ESP_ERR_NVS_NOT_FOUND)
            return ESP_OK;
        if (rc != ESP_OK)
            return rc;

        rc = load_blob(h, worker_uuid_key, uuid_, has_uuid_);
        if (rc == ESP_OK)
            rc = load_blob(h, server_uuid_key, server_uuid_, has_server_uuid_);
        if (rc == ESP_OK)
            rc = load_string(h, token_key, bearer_token_, has_bearer_token_);

        bool has_worker_id = false;
        bool has_worker_name = false;
        bool has_session = false;
        if (rc == ESP_OK)
            rc = load_string(h, worker_id_key, worker_id_, has_worker_id);
        if (rc == ESP_OK)
            rc = load_string(h, worker_name_key, worker_name_, has_worker_name);
        if (rc == ESP_OK)
            rc = load_blob(h, session_id_key, session_id_, has_session);
        if (rc == ESP_OK)
        {
            std::uint32_t expiry = 0;
            const esp_err_t expiry_rc = nvs_get_u32(h, expires_key, &expiry);
            if (expiry_rc == ESP_OK)
                token_expires_at_ = expiry;
            else if (expiry_rc != ESP_ERR_NVS_NOT_FOUND)
                rc = expiry_rc;
        }
        nvs_close(h);
        has_worker_context_ =
            has_worker_id && has_session && token_expires_at_ != 0;
        (void)has_worker_name;
        return rc;
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
    bool IdentityStore::has_worker_context() const
    {
        return has_worker_context_;
    }
    const char *IdentityStore::worker_id() const { return worker_id_.data(); }
    const char *IdentityStore::worker_name() const
    {
        return worker_name_.data();
    }
    const SessionUuid &IdentityStore::session_id() const { return session_id_; }
    std::uint32_t IdentityStore::token_expires_at() const
    {
        return token_expires_at_;
    }

    esp_err_t IdentityStore::save_uuid(const NodeUuid &value)
    {
        nvs_handle_t h{};
        esp_err_t rc = open_rw(h);
        if (rc != ESP_OK)
            return rc;
        rc = nvs_set_blob(h, worker_uuid_key, value.data(), value.size());
        if (rc == ESP_OK)
        {
            rc = nvs_commit(h);
        }
        nvs_close(h);
        if (rc == ESP_OK)
        {
            uuid_ = value;
            has_uuid_ = true;
        }
        return rc;
    }

    esp_err_t IdentityStore::save_server_uuid_once(const NodeUuid &value)
    {
        if (has_server_uuid_)
            return server_uuid_ == value ? ESP_OK : ESP_ERR_INVALID_STATE;
        nvs_handle_t h{};
        esp_err_t rc = open_rw(h);
        if (rc != ESP_OK)
            return rc;
        rc = nvs_set_blob(h, server_uuid_key, value.data(), value.size());
        if (rc == ESP_OK)
        {
            rc = nvs_commit(h);
        }
        nvs_close(h);
        if (rc == ESP_OK)
        {
            server_uuid_ = value;
            has_server_uuid_ = true;
        }
        return rc;
    }

    esp_err_t IdentityStore::save_bearer_token(const char *token)
    {
        if (!token)
            return ESP_ERR_INVALID_ARG;
        const std::size_t len = std::strlen(token);
        if (len == 0 || len >= bearer_token_.size())
            return ESP_ERR_INVALID_SIZE;
        nvs_handle_t h{};
        esp_err_t rc = open_rw(h);
        if (rc != ESP_OK)
            return rc;
        rc = nvs_set_str(h, token_key, token);
        if (rc == ESP_OK)
        {
            rc = nvs_commit(h);
        }
        nvs_close(h);
        if (rc == ESP_OK)
        {
            bearer_token_.fill('\0');
            std::memcpy(bearer_token_.data(), token, len);
            has_bearer_token_ = true;
        }
        return rc;
    }

    esp_err_t IdentityStore::save_worker_context(
        const char *id,
        const SessionUuid &sid,
        std::uint32_t expires)
    {
        if (!id || !*id || std::strlen(id) >= worker_id_.size() || expires == 0)
            return ESP_ERR_INVALID_ARG;
        nvs_handle_t h{};
        esp_err_t rc = open_rw(h);
        if (rc != ESP_OK)
            return rc;
        rc = nvs_set_str(h, worker_id_key, id);
        if (rc == ESP_OK)
            rc = nvs_set_blob(h, session_id_key, sid.data(), sid.size());
        if (rc == ESP_OK)
            rc = nvs_set_u32(h, expires_key, expires);
        if (rc == ESP_OK)
        {
            rc = nvs_commit(h);
        }
        nvs_close(h);
        if (rc == ESP_OK)
        {
            worker_id_.fill('\0');
            std::strncpy(worker_id_.data(), id, worker_id_.size() - 1);
            session_id_ = sid;
            token_expires_at_ = expires;
            has_worker_context_ = true;
        }
        return rc;
    }

    esp_err_t IdentityStore::save_worker_name(const char *name)
    {
        if (!name)
            return ESP_ERR_INVALID_ARG;
        const std::size_t len = std::strlen(name);
        if (len >= worker_name_.size())
            return ESP_ERR_INVALID_SIZE;
        nvs_handle_t h{};
        esp_err_t rc = open_rw(h);
        if (rc != ESP_OK)
            return rc;
        rc = nvs_set_str(h, worker_name_key, name);
        if (rc == ESP_OK)
        {
            rc = nvs_commit(h);
        }
        nvs_close(h);
        if (rc == ESP_OK)
        {
            worker_name_.fill('\0');
            std::memcpy(worker_name_.data(), name, len);
        }
        return rc;
    }

    esp_err_t IdentityStore::clear_uuid()
    {
        const esp_err_t rc = erase_key_commit(worker_uuid_key);
        if (rc == ESP_OK)
        {
            uuid_.fill(0);
            has_uuid_ = false;
        }
        return rc;
    }

    esp_err_t IdentityStore::clear_server_uuid()
    {
        const esp_err_t rc = erase_key_commit(server_uuid_key);
        if (rc == ESP_OK)
        {
            server_uuid_.fill(0);
            has_server_uuid_ = false;
        }
        return rc;
    }

    esp_err_t IdentityStore::clear_bearer_token()
    {
        const esp_err_t rc = erase_key_commit(token_key);
        if (rc == ESP_OK)
        {
            bearer_token_.fill('\0');
            has_bearer_token_ = false;
        }
        return rc;
    }

    esp_err_t IdentityStore::clear_worker_context()
    {
        esp_err_t rc = erase_key_commit(worker_id_key);
        if (rc == ESP_OK)
            rc = erase_key_commit(worker_name_key);
        if (rc == ESP_OK)
            rc = erase_key_commit(session_id_key);
        if (rc == ESP_OK)
            rc = erase_key_commit(expires_key);
        if (rc == ESP_OK)
        {
            worker_id_.fill('\0');
            worker_name_.fill('\0');
            session_id_.fill(0);
            token_expires_at_ = 0;
            has_worker_context_ = false;
        }
        return rc;
    }

    esp_err_t IdentityStore::clear_session_credentials()
    {
        esp_err_t rc = clear_bearer_token();
        if (rc == ESP_OK)
            rc = clear_worker_context();
        return rc;
    }

} // namespace tervanix::identity
