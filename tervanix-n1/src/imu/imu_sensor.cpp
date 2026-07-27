#include "imu_sensor.hpp"

#include <cstdint>

#include "bmi270.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"

namespace {

constexpr char TAG[] = "imu";

// Confirm these pins against the Nesso N1 schematic before flashing.
constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_21;
constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_22;
constexpr std::uint32_t I2C_CLOCK_HZ = 400000;
constexpr std::uint8_t BMI270_I2C_ADDR = BMI270_I2C_ADDRESS_L;

i2c_master_bus_handle_t i2c_bus_handle = nullptr;
bmi270_handle_t* bmi270_handle = nullptr;
bool imu_ready = false;

void cleanup_imu()
{
    imu_ready = false;

    if (bmi270_handle != nullptr) {
        (void)bmi270_stop(bmi270_handle);
        (void)bmi270_delete(bmi270_handle);
        bmi270_handle = nullptr;
    }

    if (i2c_bus_handle != nullptr) {
        (void)i2c_del_master_bus(i2c_bus_handle);
        i2c_bus_handle = nullptr;
    }
}

}  // namespace

namespace tervanix::sensor {

esp_err_t initialize_imu()
{
    if (imu_ready) {
        return ESP_OK;
    }

    // Recover cleanly if an earlier initialization attempt failed halfway.
    cleanup_imu();

    ESP_LOGI(TAG, "Initializing I2C bus on SDA=%d, SCL=%d at %lu Hz",
             static_cast<int>(I2C_SDA_PIN),
             static_cast<int>(I2C_SCL_PIN),
             static_cast<unsigned long>(I2C_CLOCK_HZ));

    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = I2C_SDA_PIN;
    bus_config.scl_io_num = I2C_SCL_PIN;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t result = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus initialization failed: %s", esp_err_to_name(result));
        cleanup_imu();
        return result;
    }

    const bmi270_driver_config_t driver_config = {
        .addr = BMI270_I2C_ADDR,
        .interface = BMI270_USE_I2C,
        .i2c_bus = i2c_bus_handle,
    };

    result = bmi270_create(&driver_config, &bmi270_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "BMI270 creation failed at address 0x%02X: %s",
                 BMI270_I2C_ADDR, esp_err_to_name(result));
        cleanup_imu();
        return result;
    }

    std::uint8_t chip_id = 0;
    result = bmi270_get_chip_id(bmi270_handle, &chip_id);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read BMI270 chip ID: %s", esp_err_to_name(result));
        cleanup_imu();
        return result;
    }

    if (chip_id != BMI270_CHIP_ID) {
        ESP_LOGE(TAG, "Unexpected BMI270 chip ID: 0x%02X (expected 0x%02X)",
                 chip_id, BMI270_CHIP_ID);
        cleanup_imu();
        return ESP_ERR_NOT_FOUND;
    }

    const bmi270_config_t measurement_config = {
        .acce_odr = BMI270_ACC_ODR_100_HZ,
        .acce_range = BMI270_ACC_RANGE_2_G,
        .gyro_odr = BMI270_GYR_ODR_100_HZ,
        .gyro_range = BMI270_GYR_RANGE_2000_DPS,
    };

    result = bmi270_start(bmi270_handle, &measurement_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "BMI270 start failed: %s", esp_err_to_name(result));
        cleanup_imu();
        return result;
    }

    imu_ready = true;
    ESP_LOGI(TAG, "BMI270 ready (chip ID 0x%02X, accel ±2 g, gyro ±2000 dps, 100 Hz)",
             chip_id);
    return ESP_OK;
}

esp_err_t read_imu(ImuSample& out)
{
    if (!imu_ready || bmi270_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    ImuSample sample{};

    esp_err_t result = bmi270_get_acce_data(
        bmi270_handle, &sample.ax, &sample.ay, &sample.az);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Accelerometer read failed: %s", esp_err_to_name(result));
        return result;
    }

    result = bmi270_get_gyro_data(
        bmi270_handle, &sample.gx, &sample.gy, &sample.gz);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Gyroscope read failed: %s", esp_err_to_name(result));
        return result;
    }

    // The Espressif driver already returns acceleration in g and angular
    // velocity in degrees per second, matching the telemetry pipeline.
    out = sample;
    return ESP_OK;
}

}  // namespace tervanix::sensor