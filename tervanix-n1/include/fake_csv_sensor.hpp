#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "imu/imu_sensor.hpp"

namespace tervanix::fake_sensor
{

#if defined(TERVANIX_BOARD_ESP32_DEV)

    struct PlaybackSample {
        sensor::ImuSample imu{};

        // Time since the beginning of the current recording cycle.
        std::uint64_t relative_ms = 0;

        // Original recorded delay from the previous CSV row.
        std::uint32_t delay_from_previous_ms = 0;

        // True for the first sample after initial start or EOF rewind.
        bool cycle_start = false;

        // Original CSV label for debugging/ground-truth only.
        // This is NOT used as the HAR prediction.
        char label[24]{};
    };

    esp_err_t initialize();

    esp_err_t read_next(PlaybackSample &sample);

    std::size_t row_count();

#else

    struct PlaybackSample {
        sensor::ImuSample imu{};
        std::uint64_t relative_ms = 0;
        std::uint32_t delay_from_previous_ms = 0;
        bool cycle_start = false;
        char label[24]{};
    };

    inline esp_err_t initialize() { return ESP_ERR_NOT_SUPPORTED; }

    inline esp_err_t read_next(PlaybackSample &)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    inline std::size_t row_count() { return 0; }

#endif

} // namespace tervanix::fake_sensor
