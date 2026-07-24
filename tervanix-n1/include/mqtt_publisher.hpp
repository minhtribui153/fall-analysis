#pragma once

#include "esp_err.h"
#include "imu/imu_sensor.hpp"

namespace tervanix::mqtt {

    // Connects to the MQTT broker. Call after WiFi is connected.
    esp_err_t connect();

    // Publishes a batch of readings as one JSON message (an array), rather
    // than one MQTT message per sample -- publishing individually at
    // 100Hz would mean 100 messages/sec per device, which is a lot of
    // network/broker overhead for not much benefit. Batching every 10
    // samples means 10 messages/sec instead.
    void publish_batch(const sensor::ImuSample* samples, int count, const char* device_id);

}  // namespace tervanix::mqtt
