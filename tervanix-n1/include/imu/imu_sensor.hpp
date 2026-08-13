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

	/**
	* @brief Initialises the BMI270 IMU sensor.
	* @details
	* Sets up the BMI270 on the Nesso N1's shared internal I2C bus
	* and prepares the sensor for readings.
	* @note
	* The bus itself is owned by nesso_board_io and is also used by the
	* LCD reset/backlight I/O expander. Do not create a second
	* I2C_NUM_0 bus.
	*
	* @return ESP_OK on success, otherwise an ESP-IDF error code.
	*/
	esp_err_t initialize_imu();

	/**
	 * @brief Reads one accelerometer and gyroscope sample from the BMI270.
	 * @param[out] out The structure that receives the six sensor-axis values.
	 * @returns `ESP_OK` when the complete sample is read successfully;
	 *          otherwise, `ESP_FAIL` when the sensor is not initialised or a
	 *          sensor read fails.
	 * @pre `initialize_imu()` must have completed successfully.
	 */
	esp_err_t read_imu(ImuSample &out);

} // namespace tervanix::sensor
