#pragma once

#include <cstddef>

#include "esp_err.h"
#include "identity_store.hpp"
#include "imu/imu_sensor.hpp"

namespace tervanix::ble
{

    /**
     * @brief Starts the BLE client used for gateway registration and telemetry.
     *
     * This function initialises NimBLE, retrieves the device hardware
     * identifier, scans for the `TVX-REG` gateway, and starts the asynchronous
     * registration process. After registration, the BLE connection remains
     * available for IMU telemetry writes.
     *
     * @param[in,out] identity_store The persistent store used to load and save the
     *                               UUID assigned by the gateway. The store
     * must remain valid while the BLE client is running.
     *
     * @return `ESP_OK` when the BLE client starts successfully; otherwise, an
     *         ESP-IDF error code describing the initialisation failure.
     */
    esp_err_t
    start_registration_client(identity::IdentityStore &identity_store);

    /**
     * @brief Stops the BLE client.
     * @return `ESP_OK` when the BLE client stops successfully; otherwise, an
     *         ESP-IDF error code.
     */
    esp_err_t stop_registration_client();

    bool connected();
    bool registered();
    /**
     * @brief Sends a batch of IMU samples over BLE.
     *
     * @param[in] samples IMU samples to send.
     * @param[in] count Number of samples in the batch.
     *
     * @return ESP_OK on success, otherwise an ESP-IDF error code.
     */
    esp_err_t
    send_telemetry_batch(const sensor::ImuSample *samples, size_t count);

    /**
     * @brief Checks if the BLE gateway is connected.
     *
     * @return true if connected, otherwise false.
     */
    bool connected();

    /**
     * @brief Checks if BLE registration is complete.
     *
     * @return true if registered, otherwise false.
     */
    bool registered();

    /**
     * @brief Checks if BLE telemetry can be sent.
     *
     * @return true if telemetry is ready, otherwise false.
     */
    bool telemetry_ready();

} // namespace tervanix::ble
