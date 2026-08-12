#pragma once

#include "esp_err.h"

namespace tervanix::sensor
{

    struct ImuSample {
        float ax;
        float ay;
        float az;
        float gx;
        float gy;
        float gz;
    };

    // Initializes the BMI270 on the Nesso N1's shared internal I2C bus.
    // The bus itself is owned by nesso_board_io and is also used by the
    // LCD reset/backlight I/O expander. Do not create a second I2C_NUM_0 bus.
    esp_err_t initialize_imu();

    esp_err_t read_imu(ImuSample &out);

} // namespace tervanix::sensor
