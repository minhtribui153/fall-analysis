#include "nesso_board_io.hpp"

#include <algorithm>
#include <cstdint>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if defined(TERVANIX_BOARD_NESSO_N1)

namespace
{

    constexpr char TAG[] = "nesso-io";
    constexpr gpio_num_t I2C_SDA = GPIO_NUM_10;
    constexpr gpio_num_t I2C_SCL = GPIO_NUM_8;
    constexpr std::uint32_t I2C_HZ = 400000;

    constexpr std::uint8_t BUTTON_EXPANDER_ADDR = 0x43;
    constexpr std::uint8_t UI_EXPANDER_ADDR = 0x44;
    constexpr std::uint8_t BQ27220_ADDR = 0x55;
    constexpr std::uint8_t AW32001_ADDR = 0x49;

    // PI4IOE5V6408 register map.
    constexpr std::uint8_t REG_DIRECTION = 0x03;
    constexpr std::uint8_t REG_OUTPUT = 0x05;
    constexpr std::uint8_t REG_HIGH_Z = 0x07;
    constexpr std::uint8_t REG_PULL_ENABLE = 0x0B;
    constexpr std::uint8_t REG_PULL_SELECT = 0x0D;
    constexpr std::uint8_t REG_INPUT_STATUS = 0x0F;

    constexpr std::uint8_t LCD_RESET_PIN = 1;     // E1.P1
    constexpr std::uint8_t LCD_BACKLIGHT_PIN = 6; // E1.P6

    // BQ27220 standard command.
    constexpr std::uint8_t BQ27220_STATE_OF_CHARGE = 0x2C;
    constexpr std::uint8_t AW32001_SYS_STATUS = 0x08;
    constexpr std::uint8_t AW32001_CHARGE_STATUS_SHIFT = 3;
    constexpr std::uint8_t AW32001_CHARGE_STATUS_MASK = 0x03;

    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_dev_handle_t button_expander = nullptr;
    i2c_master_dev_handle_t ui_expander = nullptr;
    i2c_master_dev_handle_t fuel_gauge = nullptr;
    i2c_master_dev_handle_t charger = nullptr;

    SemaphoreHandle_t init_mutex = nullptr;
    bool internal_i2c_initialized = false;

    esp_err_t add_device(std::uint8_t address, i2c_master_dev_handle_t &handle)
    {
        if (handle != nullptr)
        {
            return ESP_OK;
        }

        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = I2C_HZ;

        return i2c_master_bus_add_device(bus, &config, &handle);
    }

    esp_err_t read_reg(
        i2c_master_dev_handle_t device,
        std::uint8_t reg,
        std::uint8_t &value)
    {
        if (device == nullptr)
        {
            return ESP_ERR_INVALID_STATE;
        }

        return i2c_master_transmit_receive(device, &reg, 1, &value, 1, 100);
    }

    esp_err_t write_reg(
        i2c_master_dev_handle_t device,
        std::uint8_t reg,
        std::uint8_t value)
    {
        if (device == nullptr)
        {
            return ESP_ERR_INVALID_STATE;
        }

        const std::uint8_t payload[2]{reg, value};
        return i2c_master_transmit(device, payload, sizeof(payload), 100);
    }

    esp_err_t update_reg_bit(
        i2c_master_dev_handle_t device,
        std::uint8_t reg,
        std::uint8_t bit,
        bool set)
    {
        std::uint8_t value = 0;
        esp_err_t result = read_reg(device, reg, value);
        if (result != ESP_OK)
        {
            return result;
        }

        const std::uint8_t mask = static_cast<std::uint8_t>(1U << bit);
        value = set ? static_cast<std::uint8_t>(value | mask)
                    : static_cast<std::uint8_t>(value & ~mask);

        return write_reg(device, reg, value);
    }

    esp_err_t configure_expander_output(std::uint8_t pin, bool high)
    {
        esp_err_t result =
            update_reg_bit(ui_expander, REG_DIRECTION, pin, true);
        if (result != ESP_OK)
        {
            return result;
        }

        result = update_reg_bit(ui_expander, REG_HIGH_Z, pin, false);
        if (result != ESP_OK)
        {
            return result;
        }

        return update_reg_bit(ui_expander, REG_OUTPUT, pin, high);
    }

    esp_err_t configure_button_inputs()
    {
        if (button_expander == nullptr)
        {
            return ESP_ERR_NOT_FOUND;
        }

        constexpr std::uint8_t KEY_MASK = 0x03U; // P0=KEY1, P1=KEY2

        // PI4IOE5V6408 direction semantics:
        //   0 = input, 1 = output.
        // Explicitly force KEY1/KEY2 to inputs in case another subsystem has
        // previously changed the expander configuration.
        std::uint8_t direction = 0;
        esp_err_t result = read_reg(button_expander, REG_DIRECTION, direction);
        if (result != ESP_OK)
        {
            return result;
        }

        direction = static_cast<std::uint8_t>(direction & ~KEY_MASK);
        result = write_reg(button_expander, REG_DIRECTION, direction);
        if (result != ESP_OK)
        {
            return result;
        }

        // Make sure the output drivers are high impedance for both keys.
        std::uint8_t high_z = 0;
        result = read_reg(button_expander, REG_HIGH_Z, high_z);
        if (result != ESP_OK)
        {
            return result;
        }

        high_z = static_cast<std::uint8_t>(high_z | KEY_MASK);
        result = write_reg(button_expander, REG_HIGH_Z, high_z);
        if (result != ESP_OK)
        {
            return result;
        }

        // The PI4IOE5V6408 defaults to pull-down. The Nesso keys are expected
        // to idle HIGH and be pulled LOW when pressed, so explicitly enable
        // pull-ups on P0/P1. Without this, a button tied to ground can remain
        // LOW both before and during a press and appears "unclickable".
        std::uint8_t pull_enable = 0;
        result = read_reg(button_expander, REG_PULL_ENABLE, pull_enable);
        if (result != ESP_OK)
        {
            return result;
        }

        pull_enable = static_cast<std::uint8_t>(pull_enable | KEY_MASK);
        result = write_reg(button_expander, REG_PULL_ENABLE, pull_enable);
        if (result != ESP_OK)
        {
            return result;
        }

        std::uint8_t pull_select = 0;
        result = read_reg(button_expander, REG_PULL_SELECT, pull_select);
        if (result != ESP_OK)
        {
            return result;
        }

        // 1 = pull-up, 0 = pull-down.
        pull_select = static_cast<std::uint8_t>(pull_select | KEY_MASK);
        result = write_reg(button_expander, REG_PULL_SELECT, pull_select);
        if (result != ESP_OK)
        {
            return result;
        }

        std::uint8_t idle = 0;
        result = read_reg(button_expander, REG_INPUT_STATUS, idle);
        if (result == ESP_OK)
        {
            ESP_LOGI(
                TAG,
                "KEY1/KEY2 configured input+pull-up; raw idle=0x%02X",
                idle & KEY_MASK);
        }

        return result;
    }

    esp_err_t read_u16_le(
        i2c_master_dev_handle_t device,
        std::uint8_t command,
        std::uint16_t &value)
    {
        if (device == nullptr)
        {
            return ESP_ERR_INVALID_STATE;
        }

        std::uint8_t bytes[2]{};
        const esp_err_t result = i2c_master_transmit_receive(
            device, &command, 1, bytes, sizeof(bytes), 100);
        if (result != ESP_OK)
        {
            return result;
        }

        value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[0]) |
            (static_cast<std::uint16_t>(bytes[1]) << 8));
        return ESP_OK;
    }

} // namespace

namespace tervanix::board
{

    esp_err_t initialize_internal_i2c()
    {
        if (internal_i2c_initialized)
        {
            return ESP_OK;
        }

        if (init_mutex == nullptr)
        {
            init_mutex = xSemaphoreCreateMutex();
            if (init_mutex == nullptr)
            {
                return ESP_ERR_NO_MEM;
            }
        }

        xSemaphoreTake(init_mutex, portMAX_DELAY);

        if (internal_i2c_initialized)
        {
            xSemaphoreGive(init_mutex);
            return ESP_OK;
        }

        if (bus == nullptr)
        {
            i2c_master_bus_config_t bus_config{};
            bus_config.i2c_port = I2C_NUM_0;
            bus_config.sda_io_num = I2C_SDA;
            bus_config.scl_io_num = I2C_SCL;
            bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
            bus_config.glitch_ignore_cnt = 7;
            bus_config.flags.enable_internal_pullup = true;

            const esp_err_t result = i2c_new_master_bus(&bus_config, &bus);

            if (result != ESP_OK)
            {
                ESP_LOGE(
                    TAG,
                    "Internal I2C init failed: %s",
                    esp_err_to_name(result));
                xSemaphoreGive(init_mutex);
                return result;
            }
        }

        esp_err_t result = add_device(UI_EXPANDER_ADDR, ui_expander);

        if (result != ESP_OK)
        {
            ESP_LOGE(
                TAG, "UI expander init failed: %s", esp_err_to_name(result));
            xSemaphoreGive(init_mutex);
            return result;
        }

        result = add_device(BUTTON_EXPANDER_ADDR, button_expander);

        if (result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Button expander unavailable: %s",
                esp_err_to_name(result));
            button_expander = nullptr;
        } else
        {
            const esp_err_t button_result = configure_button_inputs();

            if (button_result != ESP_OK)
            {
                ESP_LOGW(
                    TAG,
                    "KEY1/KEY2 configuration failed: %s",
                    esp_err_to_name(button_result));
            }
        }

        result = add_device(BQ27220_ADDR, fuel_gauge);
        if (result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "BQ27220 fuel gauge unavailable: %s",
                esp_err_to_name(result));
            fuel_gauge = nullptr;
        }

        result = add_device(AW32001_ADDR, charger);
        if (result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "AW32001 charger unavailable: %s",
                esp_err_to_name(result));
            charger = nullptr;
        }

        internal_i2c_initialized = true;

        ESP_LOGI(TAG, "Nesso N1 internal I2C ready: SDA=10 SCL=8");

        xSemaphoreGive(init_mutex);
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

    esp_err_t read_user_keys(std::uint8_t &key_bits)
    {
        esp_err_t result = initialize_internal_i2c();
        if (result != ESP_OK)
        {
            return result;
        }
        if (button_expander == nullptr)
        {
            return ESP_ERR_NOT_FOUND;
        }

        std::uint8_t status = 0;
        result = read_reg(button_expander, REG_INPUT_STATUS, status);
        if (result != ESP_OK)
        {
            return result;
        }

        key_bits = static_cast<std::uint8_t>(status & 0x03U);
        return ESP_OK;
    }

    esp_err_t read_battery_percentage(std::uint8_t &percentage)
    {
        esp_err_t result = initialize_internal_i2c();
        if (result != ESP_OK)
        {
            return result;
        }
        if (fuel_gauge == nullptr)
        {
            return ESP_ERR_NOT_FOUND;
        }

        std::uint16_t soc = 0;
        result = read_u16_le(fuel_gauge, BQ27220_STATE_OF_CHARGE, soc);
        if (result != ESP_OK)
        {
            return result;
        }

        soc = std::min<std::uint16_t>(soc, 100);
        percentage = static_cast<std::uint8_t>(soc);
        return ESP_OK;
    }

    esp_err_t read_charge_status(ChargeStatus &status)
    {
        esp_err_t result = initialize_internal_i2c();
        if (result != ESP_OK)
        {
            status = ChargeStatus::Unknown;
            return result;
        }
        if (charger == nullptr)
        {
            status = ChargeStatus::Unknown;
            return ESP_ERR_NOT_FOUND;
        }

        std::uint8_t raw = 0;
        result = read_reg(charger, AW32001_SYS_STATUS, raw);
        if (result != ESP_OK)
        {
            status = ChargeStatus::Unknown;
            return result;
        }

        const std::uint8_t charge_bits = static_cast<std::uint8_t>(
            (raw >> AW32001_CHARGE_STATUS_SHIFT) & AW32001_CHARGE_STATUS_MASK);

        switch (charge_bits)
        {
        case 0:
            status = ChargeStatus::Discharging;
            break;
        case 1:
        case 2:
            status = ChargeStatus::Charging;
            break;
        case 3:
            status = ChargeStatus::Full;
            break;
        default:
            status = ChargeStatus::Unknown;
            break;
        }

        return ESP_OK;
    }

} // namespace tervanix::board

#else

namespace tervanix::board
{

    esp_err_t initialize_internal_i2c() { return ESP_ERR_NOT_SUPPORTED; }

    i2c_master_bus_handle_t internal_i2c_bus() { return nullptr; }

    esp_err_t set_lcd_reset(bool) { return ESP_ERR_NOT_SUPPORTED; }

    esp_err_t set_lcd_backlight(bool) { return ESP_ERR_NOT_SUPPORTED; }

    esp_err_t read_user_keys(std::uint8_t &) { return ESP_ERR_NOT_SUPPORTED; }

    esp_err_t read_battery_percentage(std::uint8_t &)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t read_charge_status(ChargeStatus &status)
    {
        status = ChargeStatus::Unknown;
        return ESP_ERR_NOT_SUPPORTED;
    }

} // namespace tervanix::board

#endif
