#pragma once
#include <cstdint>

namespace tervanix::telemetry
{
    struct ImuSample {
        float ax, ay, az, gx, gy, gz;
    };

    struct PeerSample {
        std::uint32_t magic;
        std::uint32_t sequence;
        std::uint8_t ttl;
        std::uint8_t reserved[3];
        char device_id[18];
        std::uint8_t source_uuid[16];
        std::int64_t timestamp_ms;
        ImuSample sample;
    };
    constexpr std::uint32_t PEER_MAGIC = 0x54565831; // TVX1
} // namespace tervanix::telemetry
