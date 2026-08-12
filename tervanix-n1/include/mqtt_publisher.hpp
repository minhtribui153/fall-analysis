#pragma once

#include <cstdint>

#include "esp_err.h"
#include "imu/imu_sensor.hpp"

/**
 * @file mqtt_publisher.hpp
 * @brief Provides the legacy direct MQTT telemetry publisher.
*/
namespace tervanix::mqtt
{
	struct TelemetrySample {
		sensor::ImuSample imu;
		std::uint32_t sequence;
		std::int64_t source_uptime_ms;
	};

	struct Stats {
		std::uint64_t published_batches;
		std::uint64_t published_samples;
		std::uint64_t publish_failures;
		std::int64_t last_publish_ms;
	};

	/**
	 * @brief Starts MQTT against the Wi-Fi default gateway.
	 * @details
	 * The function creates the ESP-MQTT client and starts its asynchronous
	 * connection process. Network connectivity must already be available.
	 * @param broker_ip The MQTT broker's host IP Address
	 * @param device_id This node's unique device UUID
	 * @param bearer_token The node's bearer token
	 * @returns `ESP_OK` when the client starts successfully; otherwise, an
	 *          ESP-IDF error code describing the initialisation failure.
	 */
	esp_err_t connect(
		const char *broker_ip,
		const char *device_id,
		const char *bearer_token);

	bool is_connected();
	bool auth_rejected();
	bool auth_accepted();
	Stats stats();

	esp_err_t publish_batch(
		const TelemetrySample *samples,
		int count,
		const char *device_id);

} // namespace tervanix::mqtt
