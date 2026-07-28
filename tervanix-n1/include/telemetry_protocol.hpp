#pragma once

#include <cstddef>
#include <cstdint>

namespace tervanix::protocol {

// The Nesso samples at 100 Hz, so samples are 10 ms apart.
inline constexpr std::uint8_t telemetry_version = 1;
inline constexpr std::size_t telemetry_samples_per_packet = 2;
inline constexpr std::uint32_t telemetry_sample_period_ms = 10;

struct TelemetrySample {
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
};

// 12-byte header + 2 x 24-byte IMU samples = 60 bytes.
// With the existing 64-byte ATT MTU, a normal GATT write can carry at most
// 61 bytes (MTU - 3), so this deliberately fits without fragmentation.
struct TelemetryPacket {
    std::uint8_t version;
    std::uint8_t sample_count;
    std::uint8_t hardware_id[6];
    std::uint32_t first_sample_sequence;
    TelemetrySample samples[telemetry_samples_per_packet];
};

static_assert(sizeof(float) == 4, "Telemetry protocol requires 32-bit float");
static_assert(sizeof(TelemetrySample) == 24);
static_assert(offsetof(TelemetryPacket, samples) == 12);
static_assert(sizeof(TelemetryPacket) == 60);

}  // namespace tervanix::protocol
