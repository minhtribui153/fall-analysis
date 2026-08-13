#pragma once
#include "esp_err.h"
#include "identity_store.hpp"
#include <cstddef>
#include <cstdint>

namespace tervanix::model
{
    struct Image {
        std::uint8_t *data = nullptr;
        std::size_t size = 0;
        char sha256[65]{};
        char model_id[48]{};
        int sample_rate_hz = 25;
        int window_samples = 50;
        int slide_samples = 25;
        int num_axes = 8;
        float min_confidence = 0.60f;
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
