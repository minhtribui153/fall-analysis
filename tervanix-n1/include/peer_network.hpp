#pragma once

#include "esp_err.h"
#include "registry_store.hpp"
#include "telemetry.hpp"

namespace tervanix::peer
{
    /**
     * @brief Starts the peer network server
     * @details This function will initiate the ESP-NOW protocol and registers
     * its own broadcast peer to interface with other nodes. This will handle
     * Peer-to-Peer (P2P) connections between a registered client to another
     * registered client within the network.
     * @returns An error that can possibly happen when starting the P2P server.
     */
    esp_err_t start(const registry::RegistryStore &registry);

    /**
     * @brief Broadcasts a sample packet
     * @details This function will broadcast one telemetry packet to all
     * nearby ESP-NOW nodes on the same Wi-Fi channel.
     * @returns An error that can possibly happen when broadcasting.
     */
    esp_err_t broadcast_sample(const telemetry::PeerSample &sample);

    using ReceiveHandler = void (*)(const telemetry::PeerSample &sample);

    void set_receive_handler(ReceiveHandler handler);
} // namespace tervanix::peer
