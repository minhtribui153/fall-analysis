#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace tervanix::board
{

    // Shared internal I2C bus for Nesso N1 peripherals.
    // Nesso N1 wiring: SDA=GPIO10, SCL=GPIO8, 400 kHz.
    esp_err_t initialize_internal_i2c();
    i2c_master_bus_handle_t internal_i2c_bus();

    // PI4IOE5V6408-1 at 0x44 controls Nesso N1 UI/power signals.
    esp_err_t set_lcd_reset(bool high);
    esp_err_t set_lcd_backlight(bool on);

} // namespace tervanix::board
