#pragma once

#include "esp_err.h"

/**
 * @file imu_sensor.hpp
 * @brief Provides BMI270 initialisation and six-axis IMU sampling.
 */

namespace tervanix::sensor {

    /**
     * @brief Contains one accelerometer and gyroscope sample.
     *
     * The fall-detection pipeline expects acceleration values in `g` and
     * angular velocity values in degrees per second.
     */
    struct ImuSample {
        /** @brief Acceleration along the X-axis, expected in `g`. */
        float ax;

        /** @brief Acceleration along the Y-axis, expected in `g`. */
        float ay;

        /** @brief Acceleration along the Z-axis, expected in `g`. */
        float az;

        /** @brief Angular velocity around the X-axis, expected in degrees/s. */
        float gx;

        /** @brief Angular velocity around the Y-axis, expected in degrees/s. */
        float gy;

        /** @brief Angular velocity around the Z-axis, expected in degrees/s. */
        float gz;
    };

    /**
     * @brief Initialises the I2C bus and the onboard BMI270 IMU.
     *
     * The accelerometer and gyroscope are configured for the sampling rate and
     * measurement ranges required by the telemetry pipeline. This function
     * should be called once before `read_imu()`.
     *
     * @return `ESP_OK` when the sensor is ready; otherwise, an ESP-IDF error
     *         code describing the initialisation failure.
     */
    esp_err_t initialize_imu();

    /**
     * @brief Reads one accelerometer and gyroscope sample from the BMI270.
     *
     * @param[out] out The structure that receives the six sensor-axis values.
     *
     * @return `ESP_OK` when the complete sample is read successfully;
     *         otherwise, `ESP_FAIL` when the sensor is not initialised or a
     *         sensor read fails.
     *
     * @pre `initialize_imu()` must have completed successfully.
     */
    esp_err_t read_imu(ImuSample& out);

}  // namespace tervanix::sensor
