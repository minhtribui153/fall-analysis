#pragma once

#include <array>
#include <cstdint>

#include "esp_err.h"

namespace tervanix::identity {

using NodeUuid = std::array<std::uint8_t, 16>;

class IdentityStore {
public:
    esp_err_t begin();

    bool has_uuid() const;
    const NodeUuid& uuid() const;

    esp_err_t save_uuid(const NodeUuid& uuid);
    esp_err_t clear_uuid();

private:
    bool has_uuid_ = false;
    NodeUuid uuid_ {};
};

}  // namespace tervanix::identity