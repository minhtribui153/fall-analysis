#pragma once

#include <cstdint>

/**
 * @file registration_protocol.hpp
 * @brief Defines the binary BLE protocol used to register a Nesso node.
 */

namespace tervanix::protocol {

/** @brief Current version of the BLE registration packet format. */
inline constexpr std::uint8_t registration_version = 1;

/** @brief Flag indicating that a request contains a previously assigned UUID. */
inline constexpr std::uint8_t flag_has_uuid = 1U << 0;

/**
 * @brief Identifies the result of a gateway registration request.
 */
enum class RegistrationStatus : std::uint8_t {
    /** @brief The gateway rejected the registration request. */
    Error = 0,

    /** @brief The gateway accepted the UUID already stored by the node. */
    ExistingUuidAccepted = 1,

    /** @brief The gateway generated and assigned a new UUID to the node. */
    NewUuidAssigned = 2,
};

/**
 * @brief Binary request written by the Nesso to the registration characteristic.
 *
 * The structure is packed so its in-memory representation matches the
 * 28-byte BLE wire format exactly.
 */
struct __attribute__((packed)) RegistrationRequest {
    /** @brief Registration protocol version used by this request. */
    std::uint8_t version;

    /** @brief Bit mask containing registration request flags. */
    std::uint8_t flags;

    /** @brief Raw six-byte hardware identifier of the requesting node. */
    std::uint8_t hardware_id[6];

    /**
     * @brief Previously assigned UUID, or all zeroes when no UUID is available.
     */
    std::uint8_t current_uuid[16];

    /** @brief Random value used to match the response to this request. */
    std::uint32_t nonce;
};

/**
 * @brief Binary response read by the Nesso from the registration characteristic.
 *
 * The structure is packed so its in-memory representation matches the
 * 21-byte BLE wire format exactly.
 */
struct __attribute__((packed)) RegistrationResponse {
    /** @brief Outcome of the registration request. */
    RegistrationStatus status;

    /** @brief UUID accepted or newly assigned by the gateway. */
    std::uint8_t assigned_uuid[16];

    /** @brief Nonce copied from the corresponding registration request. */
    std::uint32_t nonce;
};

static_assert(sizeof(RegistrationRequest) == 28);
static_assert(sizeof(RegistrationResponse) == 21);

}  // namespace tervanix::protocol
