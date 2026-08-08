#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file telemetry_protocol.hpp
 * @brief Defines the fixed-size BLE packet used to transport IMU telemetry.
 */

namespace tervanix::protocol {

/** @brief Current telemetry protocol version. */
inline constexpr std::uint8_t telemetry_version = 1;

/** @brief Maximum samples in one BLE packet. */
inline constexpr std::size_t telemetry_samples_per_packet = 2;

/** @brief Time between samples at 100 Hz. */
inline constexpr std::uint32_t telemetry_sample_period_ms = 10;

/**
 * @brief Stores one IMU sample for BLE transmission.
 */
struct TelemetrySample {
    /** @brief X-axis acceleration in g. */
    float ax;

    /** @brief Y-axis acceleration in g. */
    float ay;

    /** @brief Z-axis acceleration in g. */
    float az;

    /** @brief X-axis rotation speed in degrees/s. */
    float gx;

    /** @brief Y-axis rotation speed in degrees/s. */
    float gy;

    /** @brief Z-axis rotation speed in degrees/s. */
    float gz;
};

/**
 * @brief Stores a batch of IMU samples sent over BLE.
 */
struct TelemetryPacket {
    /** @brief Telemetry protocol version. */
    std::uint8_t version;

    /** @brief Number of samples stored in the packet. */
    std::uint8_t sample_count;

    /** @brief Hardware ID of the sending device. */
    std::uint8_t hardware_id[6];

    /** @brief Sequence number of the first sample. */
    std::uint32_t first_sample_sequence;

    /** @brief IMU samples stored in the packet. */
    TelemetrySample samples[telemetry_samples_per_packet];
};

static_assert(sizeof(float) == 4, "Telemetry protocol requires 32-bit float");
static_assert(sizeof(TelemetrySample) == 24);
static_assert(offsetof(TelemetryPacket, samples) == 12);
static_assert(sizeof(TelemetryPacket) == 60);

}  // namespace tervanix::protocol
