#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

/**
 * @file device_identity.hpp
 * @brief Provides the hardware identity used to identify a Nesso device.
 */

namespace tervanix::identity {

/**
 * @brief Represents the device's six-byte hardware identifier.
 *
 * The identifier is derived from the ESP32-C6 station MAC address and is used
 * consistently by BLE registration, telemetry packets, and the database's
 * `hardware_id` field.
 */
using HardwareId = std::array<std::uint8_t, 6>;

/**
 * @brief Retrieves the hardware identifier for the current device.
 *
 * This function reads the ESP32-C6 station MAC address and stores its six raw
 * bytes in `out`.
 *
 * @param[out] out The buffer that receives the six-byte hardware identifier.
 *
 * @return `ESP_OK` when the identifier is read successfully; otherwise, an
 *         ESP-IDF error code describing the failure.
 */
esp_err_t get_hardware_id(HardwareId& out);

/**
 * @brief Formats a hardware identifier as a readable MAC-address string.
 *
 * This function converts `id` to the uppercase format
 * `AA:BB:CC:DD:EE:FF`, which can be used as the recognised `hardware_id`
 * value throughout the data pipeline.
 *
 * @param[in] id The unformatted six-byte hardware identifier.
 * @param[out] out The output buffer that receives the formatted string.
 * @param[in] out_size The total size of the output buffer in bytes.
 *
 * @note `out_size` should be at least 18 bytes: 17 visible characters and
 *       one null terminator.
 */
void format_device_id(const HardwareId& id, char* out, std::size_t out_size);

}  // namespace tervanix::identity
