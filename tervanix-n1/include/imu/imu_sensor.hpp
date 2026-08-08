#pragma once

#include "esp_err.h"
#include "telemetry.hpp"

/**
 * @file imu_sensor.hpp
 * @brief Provides BMI270 initialisation and six-axis IMU sampling.
 */

namespace tervanix::sensor
{

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
    * @brief Initialises the BMI270 IMU sensor.
    *
    * Sets up I2C and prepares the sensor for readings.
    *
    * @return ESP_OK on success, otherwise an ESP-IDF error code.
    */
    esp_err_t initialize_imu();

    /**
    * @brief Reads one sample from the BMI270.
    *
    * @param[out] out Stores the accelerometer and gyroscope values.
    *
    * @return ESP_OK on success, otherwise ESP_FAIL.
    */
    esp_err_t read_imu(ImuSample &out);

} // namespace tervanix::sensor
