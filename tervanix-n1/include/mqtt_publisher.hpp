#pragma once

#include "esp_err.h"
#include "imu/imu_sensor.hpp"

/**
 * @file mqtt_publisher.hpp
 * @brief Provides the legacy direct MQTT telemetry publisher.
 */

namespace tervanix::mqtt {

    /**
     * @brief Starts the MQTT client using the configured broker URI.
     *
     * The function creates the ESP-MQTT client and starts its asynchronous
     * connection process. Network connectivity must already be available.
     *
     * @return `ESP_OK` when the client starts successfully; otherwise, an
     *         ESP-IDF error code describing the initialisation failure.
     */
    esp_err_t connect();

    /**
     * @brief Publishes a batch of IMU readings as one JSON array.
     *
     * Each reading is timestamped and added to a single MQTT message to reduce
     * broker and network overhead compared with publishing one message for each
     * 100 Hz sample.
     *
     * @param[in] samples Pointer to the first sample in the batch.
     * @param[in] count Number of samples to include in the JSON array.
     * @param[in] device_id Null-terminated identifier attached to each reading.
     */
    void publish_batch(const sensor::ImuSample* samples, int count, const char* device_id);

}  // namespace tervanix::mqtt
