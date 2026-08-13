#include "imu/imu_sensor.hpp"

#include "esp_check.h"
#include "esp_log.h"

#include <cstdint>

#include "bmi270.h"
#include "nesso_board_io.hpp"

namespace
{

    constexpr char TAG[] = "imu";
    constexpr std::uint8_t BMI270_I2C_ADDR = 0x68;

    bmi270_handle_t *bmi270_handle = nullptr;
    bool imu_ready = false;

} // namespace

namespace tervanix::sensor
{

    esp_err_t initialize_imu()
    {
#if !defined(TERVANIX_BOARD_NESSO_N1)
        ESP_LOGW(TAG, "IMU initialization is only configured for Nesso N1");
        return ESP_ERR_NOT_SUPPORTED;
#else
        // The Nesso N1 display expander, touch controller, battery devices and
        // BMI270 all share the board's internal I2C bus on
        // SDA=GPIO10/SCL=GPIO8. Reuse the same ESP-IDF bus handle created by
        // the board layer rather than attempting to install a second driver on
        // I2C_NUM_0.
        ESP_RETURN_ON_ERROR(
            tervanix::board::initialize_internal_i2c(),
            TAG,
            "Internal Nesso I2C bus init failed");

        i2c_master_bus_handle_t i2c_bus_handle =
            tervanix::board::internal_i2c_bus();
        if (i2c_bus_handle == nullptr)
        {
            ESP_LOGE(TAG, "Nesso internal I2C bus handle is unavailable");
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Initializing BMI270 on Nesso internal I2C bus");

        const bmi270_driver_config_t driver_config = {
            .addr = BMI270_I2C_ADDR,
            .interface = BMI270_USE_I2C,
            .i2c_bus = i2c_bus_handle,
        };

        int bmi_result = bmi270_create(&driver_config, &bmi270_handle);
        if (bmi_result != 0)
        {
            ESP_LOGE(TAG, "bmi270_create failed: %d", bmi_result);
            return ESP_FAIL;
        }

        const bmi270_config_t measurement_config = {
            .acce_odr = BMI270_ACC_ODR_100_HZ,
            .acce_range = BMI270_ACC_RANGE_2_G,
            .gyro_odr = BMI270_GYR_ODR_100_HZ,
            .gyro_range = BMI270_GYR_RANGE_2000_DPS,
        };

        bmi_result = bmi270_start(bmi270_handle, &measurement_config);
        if (bmi_result != 0)
        {
            ESP_LOGE(TAG, "bmi270_start failed: %d", bmi_result);
            return ESP_FAIL;
        }

        imu_ready = true;
        ESP_LOGI(TAG, "BMI270 ready");
        return ESP_OK;
#endif
    }

    esp_err_t read_imu(ImuSample &out)
    {
        if (!imu_ready)
        {
            return ESP_FAIL;
        }

        int result =
            bmi270_get_acce_data(bmi270_handle, &out.ax, &out.ay, &out.az);
        if (result != 0)
        {
            ESP_LOGW(TAG, "Accel read failed: %d", result);
            return ESP_FAIL;
        }

        result = bmi270_get_gyro_data(bmi270_handle, &out.gx, &out.gy, &out.gz);
        if (result != 0)
        {
            ESP_LOGW(TAG, "Gyro read failed: %d", result);
            return ESP_FAIL;
        }

        return ESP_OK;
    }

} // namespace tervanix::sensor
