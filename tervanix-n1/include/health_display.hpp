#pragma once

#include <cstdint>

#include "esp_err.h"
#include "identity_store.hpp"

namespace tervanix::health {

    struct Snapshot {
        bool ble_connected = false;
        bool registered = false;
        bool wifi_connected = false;
        bool time_synced = false;
        bool mqtt_connected = false;
        bool mqtt_auth_accepted = false;
        bool mqtt_auth_rejected = false;
        bool imu_ready = false;

        const char* device_id = nullptr;
        const char* gateway_ip = nullptr;

        std::uint64_t published_batches = 0;
        std::uint64_t published_samples = 0;
        std::uint64_t publish_failures = 0;
        std::int64_t last_publish_ms = 0;
    };

    // Native ESP-IDF driver for the built-in Nesso N1 ST7789P3 LCD.
    // Worker ID/name/UUID and bearer-session expiry are read from IdentityStore.
    // On non-Nesso builds these functions are no-ops.
    esp_err_t initialize(const identity::IdentityStore& identity_store);
    void render(const Snapshot& snapshot, const identity::IdentityStore& identity_store);
    bool available();

}  // namespace tervanix::health