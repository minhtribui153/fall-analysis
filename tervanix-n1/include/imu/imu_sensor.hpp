#pragma once

#include "esp_err.h"
#include "telemetry.hpp"

namespace tervanix::sensor
{

    using ImuSample = telemetry::ImuSample;

    /**
     * @brief Initializes BMI270 IMU
     * @details This function will set up the I2C bus and initializes the BMI270
     * IMU sensor.
     *
     * This should be called once at statup after NVS initialization.
     * @returns An error that can possibly happen while initializing the IMU.
     */
    esp_err_t initialize_imu();

    /**
     * @brief Reads an IMU sample
     * @returns A `ESP_FAIL` error if the sensor hasn't been initialized yet
     * or reading failed.
     */
    esp_err_t read_imu(ImuSample &out);

} // namespace tervanix::sensor
