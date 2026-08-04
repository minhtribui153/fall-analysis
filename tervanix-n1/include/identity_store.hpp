#pragma once

#include "esp_err.h"
#include <array>
#include <cstdint>

namespace tervanix::identity
{
    using NodeUuid = std::array<std::uint8_t, 16>;

    class IdentityStore {
        public:
        esp_err_t begin();
        esp_err_t save_uuid_once(const NodeUuid &uuid);
        esp_err_t save_server_uuid_once(const NodeUuid &uuid);
        esp_err_t clear_all();

        bool has_uuid() const;
        const NodeUuid &uuid() const;
        bool has_server_uuid() const;
        const NodeUuid &server_uuid() const;

        private:
        bool has_uuid_ = false;
        bool has_server_uuid_ = false;
        NodeUuid uuid_{};
        NodeUuid server_uuid_{};
    };
} // namespace tervanix::identity
