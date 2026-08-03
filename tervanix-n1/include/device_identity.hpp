#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace tervanix::identity
{

    using HardwareId = std::array<std::uint8_t, 6>;

    /**
     * @brief Gets the device's MAC address.
     * @param out The output buffer to return the MAC Address
     */
    esp_err_t get_hardware_id(HardwareId &out);

    /**
     * @brief Formats to an identifiable device ID
     *
     * This function will format `id` into a recognised MAC Address
     * string format as `AA:BB:CC:DD:EE:FF`. The `out_size` should be at
     * least 18 characters (17 characters + `null` terminator)
     *
     * @param id The array type hardware ID
     * @param out The result output buffer
     * @param out_size The result output buffer size
     */
    void
    format_device_id(const HardwareId &id, char *out, std::size_t out_size);

} // namespace tervanix::identity
