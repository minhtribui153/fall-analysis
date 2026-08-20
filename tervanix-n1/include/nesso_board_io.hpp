#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace tervanix::board
{

    // Shared internal I2C bus for Nesso N1 peripherals.
    // Nesso N1 wiring: SDA=GPIO10, SCL=GPIO8, 400 kHz.
    esp_err_t initialize_internal_i2c();
    i2c_master_bus_handle_t internal_i2c_bus();

    // PI4IOE5V6408-1 at 0x44 controls LCD reset/backlight.
    esp_err_t set_lcd_reset(bool high);
    esp_err_t set_lcd_backlight(bool on);

    // PI4IOE5V6408-0 at 0x43 exposes the two programmable keys.
    // Bit 0 = KEY1, bit 1 = KEY2. The returned values are raw pin levels;
    // health_display.cpp learns the idle level at boot, so it does not depend
    // on whether the physical buttons are active-high or active-low.
    esp_err_t read_user_keys(std::uint8_t &key_bits);

    // BQ27220 fuel gauge at 0x55.
    // Returns the gauge-reported StateOfCharge percentage (0..100).
    esp_err_t read_battery_percentage(std::uint8_t &percentage);

    enum class ChargeStatus : std::uint8_t {
        Unknown = 0,
        Discharging = 1,
        Charging = 2,
        Full = 3,
    };

    // AW32001 charger at 0x49. The charger status field is read directly from
    // SYS_STATUS, so this indicator reflects the charger IC rather than
    // inferring charging from battery voltage.
    esp_err_t read_charge_status(ChargeStatus &status);

} // namespace tervanix::board
