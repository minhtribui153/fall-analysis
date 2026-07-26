#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace tervanix::identity {

using HardwareId = std::array<std::uint8_t, 6>;

// Single canonical source for this device's MAC-derived identity.
// Both the BLE registration client (registration_protocol's
// hardware_id field, see ble_advertiser.cpp) and the device_id string
// used for MQTT/Kafka/Postgres (see main.cpp::getDeviceId) MUST read
// from here. Previously these were two separate calls
// (esp_efuse_mac_get_default vs esp_read_mac(ESP_MAC_WIFI_STA)) --
// they happen to resolve to the same MAC under this project's default
// 4-universal-MAC-address config, but there's no reason to derive the
// same logical identity two different ways in two different files.
esp_err_t get_hardware_id(HardwareId& out);

// Formats as "AA:BB:CC:DD:EE:FF" -- the same string form stored in
// node_registrations.device_id and devices.identification.device_id.
// out_size should be at least 18 (17 chars + null terminator).
void format_device_id(const HardwareId& id, char* out, std::size_t out_size);

}  // namespace tervanix::identity
