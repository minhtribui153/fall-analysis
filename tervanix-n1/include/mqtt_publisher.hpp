#pragma once

#include "esp_err.h"
#include "telemetry.hpp"

namespace tervanix::mqtt
{
    /**
     * @brief Connects to an MQTT broker
     * @param broker_host The MQTT broker's host IP Address
     * @param broker_port The MQTT broker's port
     */
    esp_err_t connect(const char *broker_host, std::uint16_t broker_port);

    bool connected();

    /**
     * @file mqtt_publisher.hpp
     * @brief Provides the legacy direct MQTT telemetry publisher.
     */
    namespace tervanix::mqtt
    {

        /**
         * @brief Starts the MQTT client using the configured broker URI.
         *
         * The function creates the ESP-MQTT client and starts its asynchronous
         * connection process. Network connectivity must already be available.
         *
         * @return `ESP_OK` when the client starts successfully; otherwise, an
         *         ESP-IDF error code describing the initialisation failure.
         */
        esp_err_t connect();

        /**
         * @brief Publishes batches of IMU readings to MQTT broker
         * @details This function will take in the stored samples and the requested number
         * of samples per batch and converts them into a JSON array, rather than
         * one MQTT message per sample.
         *
         * Once they are stored into a batch, these samples will be published
         * depending on the specified maximum count of samples per batch.
         * @param samples The data samples to publish to the MQTT broker
         * @param max_count The maximum number of data samples allowed per batch
         * @param device_id The ID of the device to be sent to.
         */
        void publish_batch(
            const telemetry::ImuSample samples[],
            int max_count,
            const char *device_id);

        void publish_peer_sample(const telemetry::PeerSample &sample);
    } // namespace tervanix::mqtt
