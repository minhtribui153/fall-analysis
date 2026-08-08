#pragma once

#include <cstdint>

namespace tervanix::protocol
{
    /** @brief Current BLE registration protocol version. */
    inline constexpr std::uint8_t registration_version = 2;
    /** @brief Shows that the request contains a node UUID. */
    inline constexpr std::uint8_t flag_has_uuid = 1U << 0;
    /** @brief Shows that the request contains a gateway UUID. */
    inline constexpr std::uint8_t flag_has_server_uuid = 1U << 1;


    /**
    * @brief Defines the possible registration results.
    */
    enum class RegistrationStatus : std::uint8_t {
        Error = 0,
        ExistingUuidAccepted = 1,
        NewUuidAssigned = 2,
        IdentityRejected = 3,
        ServerRejected = 4,
    };


    /**
    * @brief Stores the registration data sent to the gateway.
    */
    struct __attribute__((packed)) RegistrationRequest {
        /** @brief Registration protocol version. */
        std::uint8_t version;

        /** @brief Shows which optional UUIDs are included. */
        std::uint8_t flags;

        /** @brief Hardware ID of the device. */
        std::uint8_t hardware_id[6];

        /** @brief Previously assigned node UUID. */
        std::uint8_t current_uuid[16];

        /** @brief Previously known gateway UUID. */
        std::uint8_t expected_server_uuid[16];

        /** @brief Random value used to match the response. */
        std::uint32_t nonce;
    };


    /**
    * @brief Stores the registration result from the gateway.
    */
    struct __attribute__((packed)) RegistrationResponse {
        /** @brief Result of the registration request. */
        RegistrationStatus status;

        /** @brief UUID assigned to the device. */
        std::uint8_t assigned_uuid[16];

        /** @brief UUID of the registration gateway. */
        std::uint8_t server_uuid[16];

        /** @brief Nonce copied from the request. */
        std::uint32_t nonce;
    };
    static_assert(sizeof(RegistrationRequest) == 44);
    static_assert(sizeof(RegistrationResponse) == 37);
} // namespace tervanix::protocol
