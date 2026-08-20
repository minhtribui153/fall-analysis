#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "identity_store.hpp"

namespace tervanix::model
{

    inline constexpr int MAX_CLASSES = 16;
    inline constexpr int MAX_CLASS_NAME = 24;

    struct Image {
        std::uint8_t *data = nullptr;
        std::size_t size = 0;

        char sha256[65]{};
        char model_id[48]{};

        // TFLite tensor/model metadata.
        int sample_rate_hz = 25;
        int window_samples = 50;
        int slide_samples = 25;
        int num_axes = 8;
        float min_confidence = 0.60f;

        // Output semantics come from the HMAC-signed model manifest.
        // Model byte size and output count are both runtime values.
        int class_count = 0;
        char classes[MAX_CLASSES][MAX_CLASS_NAME]{};

        // Runtime HAR acquisition configuration delivered with the signed model
        // manifest.  The current default collects 1.5 s of real IMU history at
        // 40 ms/sample (25 Hz), then resamples that history into the model's
        // fixed 50-sample input tensor.
        int inference_sample_period_ms = 40;
        int inference_window_ms = 1500;
        int prediction_interval_ms = 1000;

        // Store-and-forward policy.  Storage size itself is intentionally NOT
        // server-configurable; it is a fixed firmware reservation.
        bool offline_purge_unknown_first = true;
        int offline_replay_batch_size = 10;
    };

    esp_err_t ensure_current(
        const char *server_ip,
        std::uint16_t server_port,
        const char *device_id,
        const identity::SessionUuid &session_id,
        const char *bearer_token,
        Image &out);

    void release(Image &image);

} // namespace tervanix::model
