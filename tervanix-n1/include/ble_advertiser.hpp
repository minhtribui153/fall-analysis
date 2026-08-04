#pragma once

#include <cstddef>

#include "esp_err.h"
#include "identity_store.hpp"
#include "imu/imu_sensor.hpp"

/**
 * @file ble_advertiser.hpp
 * @brief Provides BLE registration and telemetry transmission for the Nesso.
 */

namespace tervanix::ble {

/**
 * @brief Starts the BLE client used for gateway registration and telemetry.
 *
 * This function initialises NimBLE, retrieves the device hardware identifier,
 * scans for the `TVX-REG` gateway, and starts the asynchronous registration
 * process. After registration, the BLE connection remains available for IMU
 * telemetry writes.
 *
 * @param[in,out] identity_store The persistent store used to load and save the
 *                               UUID assigned by the gateway. The store must
 *                               remain valid while the BLE client is running.
 *
 * @return `ESP_OK` when the BLE client starts successfully; otherwise, an
 *         ESP-IDF error code describing the initialisation failure.
 */
esp_err_t start_registration_client(
    identity::IdentityStore& identity_store
);

/**
 * @brief Sends a batch of IMU samples to the connected BLE gateway.
 *
 * The samples are encoded into one fixed-size telemetry packet and sent to the
 * gateway using a GATT write without response. The packet includes the device
 * hardware identifier and the sequence number of its first sample.
 *
 * @param[in] samples Pointer to the first sample in a contiguous sample array.
 * @param[in] count Number of valid samples to send. The value must be between
 *                  1 and `protocol::telemetry_samples_per_packet`, inclusive.
 *
 * @return `ESP_OK` when the packet is queued successfully,
 *         `ESP_ERR_INVALID_ARG` when the sample pointer or count is invalid,
 *         `ESP_ERR_INVALID_STATE` when telemetry is not ready, or `ESP_FAIL`
 *         when the BLE write cannot be started.
 */
esp_err_t send_telemetry_batch(
    const sensor::ImuSample* samples,
    std::size_t count
);

/**
 * @brief Checks whether the Nesso currently has a BLE connection.
 *
 * @return `true` when connected to a gateway; otherwise, `false`.
 */
bool connected();

/**
 * @brief Checks whether gateway registration has completed successfully.
 *
 * @return `true` when a registration response has been accepted and its UUID
 *         has been stored; otherwise, `false`.
 */
bool registered();

/**
 * @brief Checks whether the BLE link is ready to transmit telemetry.
 *
 * Telemetry is ready only when the gateway is connected, registration is
 * complete, and the telemetry characteristic has been discovered.
 *
 * @return `true` when telemetry can be sent; otherwise, `false`.
 */
bool telemetry_ready();

}  // namespace tervanix::ble
