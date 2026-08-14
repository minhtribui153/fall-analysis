#pragma once

#include <cstdint>

#include "esp_err.h"
#include "identity_store.hpp"
#include "imu/imu_sensor.hpp"

namespace tervanix::har
{
    struct Prediction;
}

/**
 * @file mqtt_publisher.hpp
 * @brief Provides the legacy direct MQTT telemetry publisher.
 */
namespace tervanix::mqtt
{
    struct TelemetrySample {
        sensor::ImuSample imu{};
        uint32_t sequence = 0;
        int64_t source_uptime_ms = 0;
        /** @brief Wall-clock timestamp to retain original time of
         * flash-replayed samples captured, even across reboots. */
        int64_t timestamp_ms = 0;
    };

    struct Stats {
        uint64_t published_batches = 0;
        uint64_t published_samples = 0;
        uint64_t published_states = 0;
        uint64_t publish_failures = 0;
        int64_t last_publish_ms = 0;
    };

    /**
     * @brief Starts MQTT against the Wi-Fi default gateway.
     * @details
     * The function creates the ESP-MQTT client and starts its asynchronous
     * connection process. Network connectivity must already be available.
     * @param broker_ip The MQTT broker's host IP Address
     * @param device_id This node's unique device UUID
     * @param worker_id The registered worker ID
     * @param session_id The authenticated session_id
     * @param bearer_token The node's bearer token after generated from the server
     * @returns `ESP_OK` when the client starts successfully; otherwise, an
     *          ESP-IDF error code describing the initialisation failure.
     */
    esp_err_t connect(
        const char *broker_ip,
        const char *device_id,
        const char *worker_id,
        const identity::SessionUuid &session_id,
        const char *bearer_token);

    bool is_connected();
    bool auth_rejected();
    bool auth_accepted();
    Stats stats();

    esp_err_t publish_batch(
        const TelemetrySample *samples,
        int count,
        const har::Prediction *prediction = nullptr);

    esp_err_t publish_state(const har::Prediction &prediction);
} // namespace tervanix::mqtt
