#pragma once

#include "esp_err.h"

namespace tervanix::sensor {

    struct ImuSample {
        float ax;
        float ay;
        float az;
        float gx;
        float gy;
        float gz;
    };

    // Sets up the I2C bus and initializes the BMI270. Call once at startup,
    // after NVS init.
    esp_err_t initialize_imu();

    // Reads one sample. Returns ESP_FAIL if the sensor hasn't been
    // initialized yet or the read failed -- caller should skip publishing
    // that sample rather than send garbage.
    esp_err_t read_imu(ImuSample& out);

}  // namespace tervanix::sensor
