#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace tervanix::identity
{

    using NodeUuid = std::array<std::uint8_t, 16>;
    inline constexpr std::size_t bearer_token_capacity = 64;
    using BearerToken = std::array<char, bearer_token_capacity>;

    class IdentityStore {
        public:
        esp_err_t begin();

        bool has_uuid() const;
        const NodeUuid &uuid() const;

        bool has_server_uuid() const;
        const NodeUuid &server_uuid() const;

        bool has_bearer_token() const;
        const char *bearer_token() const;

        // UUIDs are write-once during normal operation. Re-saving the same
        // value succeeds, but attempting to replace an existing value is
        // rejected.
        esp_err_t save_uuid_once(const NodeUuid &uuid);
        esp_err_t save_server_uuid_once(const NodeUuid &uuid);

        // The bearer token is deliberately replaceable. Re-approval can rotate
        // the MQTT credential while node_uuid and server_uuid remain immutable.
        esp_err_t save_bearer_token(const char *token);

        // Explicit factory-reset / credential reset helpers.
        esp_err_t clear_uuid();
        esp_err_t clear_server_uuid();
        esp_err_t clear_bearer_token();

        private:
        bool has_uuid_ = false;
        bool has_server_uuid_ = false;
        bool has_bearer_token_ = false;
        NodeUuid uuid_{};
        NodeUuid server_uuid_{};
        BearerToken bearer_token_{};
    };

} // namespace tervanix::identity
