#pragma once

#include <cstdint>

namespace tervanix::telemetry
{

    inline constexpr uint8_t protocol_version = 1;
    inline constexpr uint8_t packet_type_imu = 1;

    struct ImuSample {
        float ax;
        float ay;
        float az;
        float gx;
        float gy;
        float gz;
    };

    /**
     * @brief Telemetry packet class for BLE ATT payload
     * @details An utility class to allow representation of packets to be
     * sent into the network.
     * @note With the preferred MTU set to `64 bytes`, on ATT write can carry
     * up to `61 bytes (MUT - 3)`. Keep this packet at or below that limit so
     * a sample fits in one WWR (Write Without Response) operation and never
     * needs custom fragmentation/reassembly.
     */
    struct __attribute__((packed)) TelemetryPacket {
        uint8_t version;
        uint8_t type;
        uint32_t sequence;
        uint64_t uptime_ms;
        uint8_t node_uuid[16];
        uint8_t hardware_id[6];
        ImuSample sample;
    };

    static_assert(sizeof(ImuSample) == 24);
    static_assert(sizeof(TelemetryPacket) == 60);

} // namespace tervanix::telemetry
