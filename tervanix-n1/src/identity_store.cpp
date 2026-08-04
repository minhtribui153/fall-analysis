#include "identity_store.hpp"
#include "nvs.h"
#include <cstddef>
#include <cstring>

namespace
{
    constexpr char ns[] = "identity";
    constexpr char node_key[] = "node_uuid";
    constexpr char server_key[] = "server_uuid";
    esp_err_t read_blob(
        nvs_handle_t h,
        const char *key,
        tervanix::identity::NodeUuid &out,
        bool &present)
    {
        std::size_t n = out.size();
        auto r = nvs_get_blob(h, key, out.data(), &n);
        if (r == ESP_ERR_NVS_NOT_FOUND)
        {
            out.fill(0);
            present = false;
            return ESP_OK;
        }
        if (r != ESP_OK)
            return r;
        if (n != out.size())
            return ESP_ERR_INVALID_SIZE;
        present = true;
        return ESP_OK;
    }

    esp_err_t write_once(
        const char *key,
        const tervanix::identity::NodeUuid &value,
        tervanix::identity::NodeUuid &cached,
        bool &present)
    {
        if (present)
            return cached == value ? ESP_OK : ESP_ERR_INVALID_STATE;
        nvs_handle_t h{};
        auto r = nvs_open(ns, NVS_READWRITE, &h);
        if (r != ESP_OK)
            return r;
        r = nvs_set_blob(h, key, value.data(), value.size());
        if (r == ESP_OK)
            r = nvs_commit(h);
        nvs_close(h);
        if (r == ESP_OK)
        {
            cached = value;
            present = true;
        }
        return r;
    }
} // namespace

namespace tervanix::identity
{
    esp_err_t IdentityStore::begin()
    {
        nvs_handle_t h{};
        auto r = nvs_open(ns, NVS_READONLY, &h);
        if (r == ESP_ERR_NVS_NOT_FOUND)
            return ESP_OK;
        if (r != ESP_OK)
            return r;
        r = read_blob(h, node_key, uuid_, has_uuid_);
        if (r == ESP_OK)
            r = read_blob(h, server_key, server_uuid_, has_server_uuid_);
        nvs_close(h);
        return r;
    }
    bool IdentityStore::has_uuid() const { return has_uuid_; }
    const NodeUuid &IdentityStore::uuid() const { return uuid_; }
    bool IdentityStore::has_server_uuid() const { return has_server_uuid_; }
    const NodeUuid &IdentityStore::server_uuid() const { return server_uuid_; }
    esp_err_t IdentityStore::save_uuid_once(const NodeUuid &v)
    {
        return write_once(node_key, v, uuid_, has_uuid_);
    }
    esp_err_t IdentityStore::save_server_uuid_once(const NodeUuid &v)
    {
        return write_once(server_key, v, server_uuid_, has_server_uuid_);
    }
    esp_err_t IdentityStore::clear_all()
    {
        nvs_handle_t h{};
        auto r = nvs_open(ns, NVS_READWRITE, &h);
        if (r != ESP_OK)
            return r;
        r = nvs_erase_all(h);
        if (r == ESP_OK)
            r = nvs_commit(h);
        nvs_close(h);
        if (r == ESP_OK)
        {
            uuid_.fill(0);
            server_uuid_.fill(0);
            has_uuid_ = has_server_uuid_ = false;
        }
        return r;
    }
} // namespace tervanix::identity
