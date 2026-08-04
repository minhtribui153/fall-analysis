#pragma once

#include <cstdint>

namespace tervanix::protocol
{
    inline constexpr std::uint8_t registration_version = 2;
    inline constexpr std::uint8_t flag_has_uuid = 1U << 0;
    inline constexpr std::uint8_t flag_has_server_uuid = 1U << 1;
    enum class RegistrationStatus : std::uint8_t {
        Error = 0,
        ExistingUuidAccepted = 1,
        NewUuidAssigned = 2,
        IdentityRejected = 3,
        ServerRejected = 4,
    };
    struct __attribute__((packed)) RegistrationRequest {
        std::uint8_t version;
        std::uint8_t flags;
        std::uint8_t hardware_id[6];
        std::uint8_t current_uuid[16];
        std::uint8_t expected_server_uuid[16];
        std::uint32_t nonce;
    };
    struct __attribute__((packed)) RegistrationResponse {
        RegistrationStatus status;
        std::uint8_t assigned_uuid[16];
        std::uint8_t server_uuid[16];
        std::uint32_t nonce;
    };
    static_assert(sizeof(RegistrationRequest) == 44);
    static_assert(sizeof(RegistrationResponse) == 37);
} // namespace tervanix::protocol
