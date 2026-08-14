#pragma once

#include <cstdint>

#include "esp_err.h"
#include "identity_store.hpp"

namespace tervanix::health
{

    struct Snapshot {
        bool ble_connected = false;
        bool registered = false;
        bool wifi_connected = false;
        bool time_synced = false;
        bool mqtt_connected = false;
        bool mqtt_auth_accepted = false;
        bool mqtt_auth_rejected = false;
        bool imu_ready = false;
        bool model_ready = false;
        bool prediction_ready = false;
        const char *prediction_state = nullptr;
        float prediction_confidence = 0.0f;
        const char *model_sha256 = nullptr;

        const char *device_id = nullptr;
        const char *gateway_ip = nullptr;

        std::uint64_t published_batches = 0;
        std::uint64_t published_samples = 0;
        std::uint64_t published_states = 0;
        std::uint64_t publish_failures = 0;
        std::int64_t last_publish_ms = 0;
    };

    // Native ESP-IDF UI for the built-in Nesso N1 ST7789P3 LCD.
    //
    // The UI is deliberately split into several large-text pages:
    //   KEY1 -> next page
    //   KEY2 -> previous page
    //
    // Battery percentage is read from the on-board BQ27220 fuel gauge.
    // On non-Nesso builds these functions are no-ops.
    esp_err_t initialize(const identity::IdentityStore &identity_store);
    void render(
        const Snapshot &snapshot,
        const identity::IdentityStore &identity_store);
    bool available();

} // namespace tervanix::health
