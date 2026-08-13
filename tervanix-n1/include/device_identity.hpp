#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace tervanix::identity
{

    /** @brief Stores the six-byte hardware ID. */
    using HardwareId = std::array<std::uint8_t, 6>;

    /**
    * @brief Gets the hardware ID of the device.
    *
    * @param[out] out Stores the six-byte hardware ID.
    *
    * @return ESP_OK on success, otherwise an ESP-IDF error code.
    */
    esp_err_t get_hardware_id(HardwareId &out);

    /**
    * @brief Formats the hardware ID as a MAC address.
    *
    * @param[in] id Hardware ID to format.
    * @param[out] out Stores the formatted hardware ID.
    * @param[in] out_size Size of the output buffer.
    */
    void
    format_device_id(const HardwareId &id, char *out, std::size_t out_size);

} // namespace tervanix::identity
