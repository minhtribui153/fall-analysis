#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file telemetry_protocol.hpp
 * @brief Defines the fixed-size BLE packet used to transport IMU telemetry.
 */

namespace tervanix::protocol {

/** @brief Current version of the BLE telemetry packet format. */
inline constexpr std::uint8_t telemetry_version = 1;

/** @brief Maximum number of IMU samples stored in one telemetry packet. */
inline constexpr std::size_t telemetry_samples_per_packet = 2;

/** @brief Time between consecutive IMU samples at the 100 Hz sampling rate. */
inline constexpr std::uint32_t telemetry_sample_period_ms = 10;

/**
 * @brief Contains one six-axis IMU sample in the BLE wire format.
 *
 * The pipeline expects acceleration values in `g` and angular velocity values
 * in degrees per second.
 */
struct TelemetrySample {
    /** @brief Acceleration along the X-axis, expected in `g`. */
    float ax;

    /** @brief Acceleration along the Y-axis, expected in `g`. */
    float ay;

    /** @brief Acceleration along the Z-axis, expected in `g`. */
    float az;

    /** @brief Angular velocity around the X-axis, expected in degrees/s. */
    float gx;

    /** @brief Angular velocity around the Y-axis, expected in degrees/s. */
    float gy;

    /** @brief Angular velocity around the Z-axis, expected in degrees/s. */
    float gz;
};

/**
 * @brief Contains a fixed-size batch of IMU samples sent over BLE.
 *
 * The packet has a 12-byte header followed by two 24-byte samples, producing a
 * total size of 60 bytes. This fits inside the 61-byte GATT payload available
 * with the negotiated 64-byte ATT MTU and therefore avoids fragmentation.
 */
struct TelemetryPacket {
    /** @brief Telemetry protocol version used by this packet. */
    std::uint8_t version;

    /** @brief Number of valid entries currently stored in `samples`. */
    std::uint8_t sample_count;

    /** @brief Raw six-byte hardware identifier of the sending node. */
    std::uint8_t hardware_id[6];

    /** @brief Sequence number assigned to the first sample in this packet. */
    std::uint32_t first_sample_sequence;

    /** @brief Fixed-capacity array containing the packet's IMU samples. */
    TelemetrySample samples[telemetry_samples_per_packet];
};

static_assert(sizeof(float) == 4, "Telemetry protocol requires 32-bit float");
static_assert(sizeof(TelemetrySample) == 24);
static_assert(offsetof(TelemetryPacket, samples) == 12);
static_assert(sizeof(TelemetryPacket) == 60);

}  // namespace tervanix::protocol
