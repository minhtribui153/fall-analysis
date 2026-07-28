#pragma once

#include <cstddef>

#include "esp_err.h"
#include "identity_store.hpp"
#include "imu/imu_sensor.hpp"

namespace tervanix::ble {

// Starts the BLE client that scans for the PC/Raspberry Pi gateway named
// TVX-REG, registers this Nesso, and keeps the connection open for telemetry.
esp_err_t start_registration_client(
    identity::IdentityStore& identity_store
);

// Sends a small binary IMU packet to the gateway over the telemetry
// characteristic. count must be <= protocol::telemetry_samples_per_packet.
esp_err_t send_telemetry_batch(
    const sensor::ImuSample* samples,
    std::size_t count
);

bool connected();
bool registered();
bool telemetry_ready();

}  // namespace tervanix::ble
