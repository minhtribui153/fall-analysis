#include "nesso_board_io.hpp"

#include <cstdint>

#include "esp_log.h"

#if defined(TERVANIX_BOARD_NESSO_N1)

namespace
{

    constexpr char TAG[] = "nesso-io";
    constexpr gpio_num_t I2C_SDA = GPIO_NUM_10;
    constexpr gpio_num_t I2C_SCL = GPIO_NUM_8;
    constexpr std::uint32_t I2C_HZ = 400000;

    constexpr std::uint8_t UI_EXPANDER_ADDR = 0x44;
    constexpr std::uint8_t REG_DIRECTION = 0x03;
    constexpr std::uint8_t REG_OUTPUT = 0x05;
    constexpr std::uint8_t REG_HIGH_Z = 0x07;
    constexpr std::uint8_t LCD_RESET_PIN = 1;     // E1.P1
    constexpr std::uint8_t LCD_BACKLIGHT_PIN = 6; // E1.P6

    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_dev_handle_t ui_expander = nullptr;

    esp_err_t read_reg(std::uint8_t reg, std::uint8_t &value)
    {
        return i2c_master_transmit_receive(
            ui_expander, &reg, 1, &value, 1, 100);
    }

    esp_err_t write_reg(std::uint8_t reg, std::uint8_t value)
    {
        const std::uint8_t payload[2]{reg, value};
        return i2c_master_transmit(ui_expander, payload, sizeof(payload), 100);
    }

    esp_err_t update_reg_bit(std::uint8_t reg, std::uint8_t bit, bool set)
    {
        std::uint8_t value = 0;
        esp_err_t result = read_reg(reg, value);
        if (result != ESP_OK)
        {
            return result;
        }

        const std::uint8_t mask = static_cast<std::uint8_t>(1U << bit);
        value = set ? static_cast<std::uint8_t>(value | mask)
                    : static_cast<std::uint8_t>(value & ~mask);

        return write_reg(reg, value);
    }

    esp_err_t configure_expander_output(std::uint8_t pin, bool high)
    {
        esp_err_t result = update_reg_bit(REG_DIRECTION, pin, true);
        if (result != ESP_OK)
        {
            return result;
        }

        // Output must not remain high-impedance.
        result = update_reg_bit(REG_HIGH_Z, pin, false);
        if (result != ESP_OK)
        {
            return result;
        }

        return update_reg_bit(REG_OUTPUT, pin, high);
    }

} // namespace

namespace tervanix::board
{

    esp_err_t initialize_internal_i2c()
    {
        if (bus != nullptr)
        {
            return ESP_OK;
        }

        i2c_master_bus_config_t bus_config{};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = I2C_SDA;
        bus_config.scl_io_num = I2C_SCL;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;

        esp_err_t result = i2c_new_master_bus(&bus_config, &bus);
        if (result != ESP_OK)
        {
            ESP_LOGE(
                TAG, "Internal I2C init failed: %s", esp_err_to_name(result));
            return result;
        }

        i2c_device_config_t device_config{};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = UI_EXPANDER_ADDR;
        device_config.scl_speed_hz = I2C_HZ;

        result = i2c_master_bus_add_device(bus, &device_config, &ui_expander);
        if (result != ESP_OK)
        {
            ESP_LOGE(
                TAG, "UI expander init failed: %s", esp_err_to_name(result));
            return result;
        }

        ESP_LOGI(TAG, "Nesso N1 internal I2C ready: SDA=10 SCL=8");
        return ESP_OK;
    }

    i2c_master_bus_handle_t internal_i2c_bus() { return bus; }

    esp_err_t set_lcd_reset(bool high)
    {
        esp_err_t result = initialize_internal_i2c();
        if (result != ESP_OK)
        {
            return result;
        }
        return configure_expander_output(LCD_RESET_PIN, high);
    }

    esp_err_t set_lcd_backlight(bool on)
    {
        esp_err_t result = initialize_internal_i2c();
        if (result != ESP_OK)
        {
            return result;
        }
        return configure_expander_output(LCD_BACKLIGHT_PIN, on);
    }

} // namespace tervanix::board

#else

namespace tervanix::board
{

    esp_err_t initialize_internal_i2c() { return ESP_ERR_NOT_SUPPORTED; }

    i2c_master_bus_handle_t internal_i2c_bus() { return nullptr; }

    esp_err_t set_lcd_reset(bool) { return ESP_ERR_NOT_SUPPORTED; }

    esp_err_t set_lcd_backlight(bool) { return ESP_ERR_NOT_SUPPORTED; }

} // namespace tervanix::board

#endif
