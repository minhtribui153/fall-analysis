#pragma once

#include "esp_err.h"

namespace tervanix::wifi {

    // Connects to WiFi in station mode and blocks until connected (or
    // gives up after a timeout). Call after NVS init, before anything
    // that needs network access.
    esp_err_t connect();

    bool is_connected();

}  // namespace tervanix::wifi
