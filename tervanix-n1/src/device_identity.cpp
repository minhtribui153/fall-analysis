#include "device_identity.hpp"

#include <cstdio>

#include "esp_mac.h"

namespace tervanix::identity
{

    esp_err_t get_hardware_id(HardwareId &out)
    {
        return esp_read_mac(out.data(), ESP_MAC_WIFI_STA);
    }

    void format_device_id(const HardwareId &id, char *out, std::size_t out_size)
    {
        std::snprintf(
            out,
            out_size,
            "%02X:%02X:%02X:%02X:%02X:%02X",
            id[0],
            id[1],
            id[2],
            id[3],
            id[4],
            id[5]);
    }

} // namespace tervanix::identity
