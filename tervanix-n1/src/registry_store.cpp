#include "registry_store.hpp"
#include "nvs.h"
#include <cstring>
namespace
{
    constexpr char ns[] = "registry";
    constexpr char key[] = "uuids";
} // namespace
namespace tervanix::registry
{
    esp_err_t RegistryStore::begin()
    {
        nvs_handle_t h{};
        auto r = nvs_open(ns, NVS_READONLY, &h);
        if (r == ESP_ERR_NVS_NOT_FOUND)
            return ESP_OK;
        if (r != ESP_OK)
            return r;
        std::size_t bytes = sizeof(nodes_);
        r = nvs_get_blob(h, key, nodes_.data(), &bytes);
        nvs_close(h);
        if (r == ESP_ERR_NVS_NOT_FOUND)
            return ESP_OK;
        if (r != ESP_OK)
            return r;
        if (bytes % sizeof(identity::NodeUuid) != 0)
            return ESP_ERR_INVALID_SIZE;
        count_ = bytes / sizeof(identity::NodeUuid);
        if (count_ > max_nodes)
            return ESP_ERR_INVALID_SIZE;
        return ESP_OK;
    }
    esp_err_t
    RegistryStore::replace(const identity::NodeUuid *values, std::size_t count)
    {
        if (count > max_nodes)
            return ESP_ERR_NO_MEM;
        nvs_handle_t h{};
        auto r = nvs_open(ns, NVS_READWRITE, &h);
        if (r != ESP_OK)
            return r;
        r = nvs_set_blob(h, key, values, count * sizeof(identity::NodeUuid));
        if (r == ESP_OK)
            r = nvs_commit(h);
        nvs_close(h);
        if (r == ESP_OK)
        {
            nodes_.fill({});
            for (std::size_t i = 0; i < count; i++)
                nodes_[i] = values[i];
            count_ = count;
        }
        return r;
    }
    bool RegistryStore::contains(const identity::NodeUuid &value) const
    {
        for (std::size_t i = 0; i < count_; i++)
            if (nodes_[i] == value)
                return true;
        return false;
    }
} // namespace tervanix::registry
