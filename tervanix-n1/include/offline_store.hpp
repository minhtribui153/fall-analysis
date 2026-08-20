#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "har_inference.hpp"
#include "mqtt_publisher.hpp"

namespace tervanix::offline
{

    // Fixed firmware reservation.  Deliberately not server-configurable.
    inline constexpr std::size_t RESERVED_BYTES = 512 * 1024;
    inline constexpr int MAX_BATCH_SAMPLES = 10;

    struct Batch {
        std::uint8_t sample_count = 0;
        mqtt::TelemetrySample samples[MAX_BATCH_SAMPLES]{};
        bool has_prediction = false;
        har::Prediction prediction{};
    };

    struct Stats {
        std::uint32_t queued_batches = 0;
        std::uint32_t queued_samples = 0;
        std::uint32_t unknown_batches = 0;
        std::uint32_t purged_unknown_batches = 0;
        std::uint32_t purged_oldest_batches = 0;
        std::uint32_t capacity_batches = 0;
    };

    esp_err_t initialize(bool purge_unknown_first);
    esp_err_t enqueue(const Batch &batch);

    // Peek/ack is intentionally serialized internally.  ack_peeked() should
    // only be called after the batch has successfully been handed to MQTT.
    esp_err_t peek(Batch &batch);
    esp_err_t ack_peeked();

    bool empty();
    Stats stats();

} // namespace tervanix::offline
