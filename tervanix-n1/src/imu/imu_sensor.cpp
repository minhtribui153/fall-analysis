#include "imu/imu_sensor.hpp"

#include "esp_log.h"

#include "bmi270.h"
#include "driver/i2c_master.h"

namespace
{

    constexpr char TAG[] = "imu";

    // TODO: confirm against the actual board wiring -- these are placeholders.
    // The Arduino NESSO N1 boards handle this internally via their own
    // library, but this custom board's I2C pins need to be set explicitly.
    constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_21;
    constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_22;
    constexpr uint32_t I2C_CLOCK_HZ = 400000; // 400kHz, standard fast-mode I2C

    // TODO: confirm the BMI270's actual I2C address on this board -- it's
    // one of two values depending on how the SDO pin is wired (0x68 if
    // SDO is pulled low, 0x69 if pulled high). Check the schematic or
    // scan the bus if unsure.
    constexpr uint8_t BMI270_I2C_ADDR = 0x68;

    i2c_master_bus_handle_t i2c_bus_handle = nullptr;
    bmi270_handle_t *bmi270_handle = nullptr;
    bool imu_ready = false;

} // namespace

namespace tervanix::sensor
{

    esp_err_t initialize_imu()
    {
        ESP_LOGI(TAG, "Initializing I2C bus");

        i2c_master_bus_config_t bus_config{};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = I2C_SDA_PIN;
        bus_config.scl_io_num = I2C_SCL_PIN;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;

        esp_err_t result = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(result));
            return result;
        }

        ESP_LOGI(TAG, "Initializing BMI270");

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

        // 100Hz matches the sample rate used by the detection pipeline.
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

        // IMPORTANT, not yet verified: the detection pipeline (thresholds
        // in detection.py, calibrated against real captured data from the
        // Arduino NESSO N1 boards) assumes accel in g and gyro in deg/s.
        // Once real readings come off this board, sanity-check at rest --
        // accel magnitude should read ~1.0 (Earth's gravity, 1g), and gyro
        // should read ~0 when the board isn't moving. If the numbers look
        // off by a large, suspicious factor (e.g. ~9.8x too high, or in
        // the 0.01-0.1 range instead of near 1.0), the driver's likely
        // returning SI units (m/s^2, rad/s) instead -- convert here rather
        // than downstream, so everything after this function can keep
        // assuming g / deg/s like the rest of the pipeline does.
        return ESP_OK;
    }

} // namespace tervanix::sensor
