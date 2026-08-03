#pragma once

#include "esp_err.h"

namespace tervanix::wifi
{

    /**
     * @brief Initialize connection to WiFi
     * @details Connects to WiFi in station mode and blocks until
     * connected (or gives up after a timeout). Only call this
     * function after NVS init before anything that needs network access.
     */
    esp_err_t connect();

    bool is_connected();

    /**
     * @brief Gets the connected WiFi Gateway IP Address
     * @param out The returned output buffer of the IP Address
     * @param out_size The output buffer size to print out the IP Address
     */
    esp_err_t get_gateway_ip(char *out, size_t out_size);

} // namespace tervanix::wifi
