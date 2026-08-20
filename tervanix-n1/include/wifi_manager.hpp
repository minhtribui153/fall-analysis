#pragma once

#include "esp_err.h"

/**
 * @file wifi_manager.hpp
 * @brief Provides the legacy Wi-Fi station connection helper.
 */

namespace tervanix::wifi
{

    /**
     * @brief Connects the node to the configured Wi-Fi network.
     *
     * This function initialises the ESP-IDF network stack, starts Wi-Fi in
     * station mode, and blocks until the device connects or the retry timeout
     * is reached.
     *
     * @return `ESP_OK` when the station obtains a connection,
     *         `ESP_ERR_INVALID_STATE` when the configured SSID is empty, or
     *         `ESP_FAIL` when the connection attempts are exhausted.
     */
    esp_err_t connect();

    /**
     * @brief Checks the latest Wi-Fi connection state recorded by the manager.
     *
     * @return `true` when the station is connected; otherwise, `false`.
     */
    bool is_connected();

    /**
     * @brief Returns the IPv4 default-gateway address supplied by DHCP
     * @param out The returned output buffer of the IP Address
     * @param out_size The output buffer size to print out the IP Address
     */
    bool get_gateway_ip(char *out, size_t out_size);

} // namespace tervanix::wifi
