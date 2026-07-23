#include "identity_store.hpp"

#include <cstddef>

#include "nvs.h"

namespace {

constexpr char nvs_namespace[] = "identity";
constexpr char uuid_key[] = "node_uuid";

}  // namespace

namespace tervanix::identity {

esp_err_t IdentityStore::begin()
{
    nvs_handle_t handle {};

    esp_err_t result = nvs_open(
        nvs_namespace,
        NVS_READONLY,
        &handle
    );

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        uuid_.fill(0);
        has_uuid_ = false;
        return ESP_OK;
    }

    if (result != ESP_OK) {
        return result;
    }

    std::size_t size = uuid_.size();

    result = nvs_get_blob(
        handle,
        uuid_key,
        uuid_.data(),
        &size
    );

    nvs_close(handle);

    if (
        result == ESP_ERR_NVS_NOT_FOUND ||
        size != uuid_.size()
    ) {
        uuid_.fill(0);
        has_uuid_ = false;
        return ESP_OK;
    }

    if (result != ESP_OK) {
        return result;
    }

    has_uuid_ = true;
    return ESP_OK;
}

bool IdentityStore::has_uuid() const
{
    return has_uuid_;
}

const NodeUuid& IdentityStore::uuid() const
{
    return uuid_;
}

esp_err_t IdentityStore::save_uuid(
    const NodeUuid& uuid)
{
    nvs_handle_t handle {};

    esp_err_t result = nvs_open(
        nvs_namespace,
        NVS_READWRITE,
        &handle
    );

    if (result != ESP_OK) {
        return result;
    }

    result = nvs_set_blob(
        handle,
        uuid_key,
        uuid.data(),
        uuid.size()
    );

    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);

    if (result == ESP_OK) {
        uuid_ = uuid;
        has_uuid_ = true;
    }

    return result;
}

esp_err_t IdentityStore::clear_uuid()
{
    nvs_handle_t handle {};

    esp_err_t result = nvs_open(
        nvs_namespace,
        NVS_READWRITE,
        &handle
    );

    if (result != ESP_OK) {
        return result;
    }

    result = nvs_erase_key(handle, uuid_key);

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    }

    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);

    if (result == ESP_OK) {
        uuid_.fill(0);
        has_uuid_ = false;
    }

    return result;
}

}  // namespace tervanix::identity