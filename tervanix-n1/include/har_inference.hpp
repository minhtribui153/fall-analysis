#pragma once

#include <cstdint>

#include "esp_err.h"
#include "imu/imu_sensor.hpp"
#include "model_manager.hpp"

namespace tervanix::har
{

    struct Prediction {
        std::uint64_t prediction_sequence = 0;
        std::int64_t source_uptime_ms = 0;
        char state[24]{};
        float confidence = 0.0f;
        char model_sha256[65]{};
    };

#if defined(TERVANIX_BOARD_NESSO_N1)
    esp_err_t initialize(const model::Image &image);
    bool add_sample(
        const sensor::ImuSample &sample,
        std::int64_t source_uptime_ms,
        Prediction &out);
    bool ready();
#else
    inline esp_err_t initialize(const model::Image &)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    inline bool
    add_sample(const sensor::ImuSample &, std::int64_t, Prediction &)
    {
        return false;
    }
    inline bool ready() { return false; }
#endif

} // namespace tervanix::har
