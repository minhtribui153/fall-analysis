#include "peer_network.hpp"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include <array>
#include <cstring>

namespace
{
    constexpr char TAG[] = "peer";
    constexpr std::uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    tervanix::peer::ReceiveHandler handler = nullptr;

    const tervanix::registry::RegistryStore *allowed = nullptr;

    struct SeenKey {
        tervanix::identity::NodeUuid uuid{};
        std::uint32_t sequence = 0;
    };

    std::array<SeenKey, 32> seen{};
    std::size_t seen_pos = 0;

    bool duplicate(const tervanix::identity::NodeUuid &uuid, std::uint32_t seq)
    {
        for (const auto &x : seen)
            if (x.sequence == seq && x.uuid == uuid && seq != 0)
                return true;
        seen[seen_pos++ % seen.size()] = {uuid, seq};
        return false;
    }

    void
    on_receive(const esp_now_recv_info_t *, const std::uint8_t *data, int len)
    {
        if (len != sizeof(tervanix::telemetry::PeerSample))
            return;
        tervanix::telemetry::PeerSample packet{};
        std::memcpy(&packet, data, sizeof(packet));
        tervanix::identity::NodeUuid source{};
        std::memcpy(source.data(), packet.source_uuid, source.size());
        if (packet.magic != tervanix::telemetry::PEER_MAGIC || !allowed ||
            !allowed->contains(source) || duplicate(source, packet.sequence))
        {
            ESP_LOGW(TAG, "Rejected unregistered or duplicate peer packet");
            return;
        }
        if (handler)
            handler(packet);
        if (packet.ttl > 0)
        {
            --packet.ttl;
            esp_now_send(
                BROADCAST,
                reinterpret_cast<const std::uint8_t *>(&packet),
                sizeof(packet));
        }
    }
} // namespace

namespace tervanix::peer
{
    esp_err_t start(const registry::RegistryStore &registry)
    {
        allowed = &registry;
        auto err = esp_now_init();
        if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST)
            return err;
        ESP_ERROR_CHECK(esp_now_register_recv_cb(on_receive));
        esp_now_peer_info_t peer{};
        std::memcpy(peer.peer_addr, BROADCAST, 6);
        peer.ifidx = WIFI_IF_STA;
        peer.channel = 0;
        peer.encrypt = false;
        err = esp_now_add_peer(&peer);
        if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST)
            return err;
        ESP_LOGI(TAG, "ESP-NOW relay enabled with registry allow-list");
        return ESP_OK;
    }

    esp_err_t broadcast_sample(const telemetry::PeerSample &sample)
    {
        return esp_now_send(
            BROADCAST,
            reinterpret_cast<const std::uint8_t *>(&sample),
            sizeof(sample));
    }

    void set_receive_handler(ReceiveHandler h) { handler = h; }
} // namespace tervanix::peer
