#include "ble_advertiser.hpp"
#include "board_config.hpp"
#include "device_identity.hpp"
#include "esp_wifi.h"
#include "identity_store.hpp"
#include "mqtt_publisher.hpp"
#include "peer_network.hpp"
#include "registration_client.hpp"
#include "registry_store.hpp"
#include "wifi_manager.hpp"

#if TERVANIX_HAS_IMU
#include "imu/imu_sensor.hpp"
#endif

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <cstring>
#include <sys/time.h>

namespace
{
    constexpr char TAG[] = "tervanix";
    constexpr int SAMPLE_RATE_HZ = 100;
    constexpr int BATCH_SIZE = 10;

    tervanix::identity::IdentityStore identity_store;
    tervanix::registry::RegistryStore registry_store;

    /**
    * @brief Initialises NVS storage.
    *
    * @return ESP_OK on success, otherwise an ESP-IDF error code.
    */
    esp_err_t initialize_nvs()
    {
        auto r = nvs_flash_init();
        if (r == ESP_ERR_NVS_NO_FREE_PAGES ||
            r == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_ERROR_CHECK(nvs_flash_erase());
            r = nvs_flash_init();
        }
        return r;
    }

#if TERVANIX_HAS_IMU
    /**
    * @brief Gets the formatted hardware ID of the device.
    *
    * @param[out] out Stores the formatted hardware ID.
    * @param[in] size Size of the output buffer.
    */
    void get_device_id(char *out, size_t n)
    {
        tervanix::identity::HardwareId id{};
        ESP_ERROR_CHECK(tervanix::identity::get_hardware_id(id));
        tervanix::identity::format_device_id(id, out, n);
    }
#endif

    /**
    * @brief Waits for BLE device registration to complete.
    */
    void wait_registration()
    {
        for (int ms = 0; !tervanix::ble::registered() && ms < 20000; ms += 200)
            vTaskDelay(pdMS_TO_TICKS(200));
        if (!tervanix::ble::registered() && !identity_store.has_uuid())
            ESP_LOGW(
                TAG, "No registration UUID yet; relay remains operational");
    }

    void sync_time()
    {
        esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&c);
        esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    }

    void on_peer(const tervanix::telemetry::PeerSample &p)
    {
        if (tervanix::mqtt::connected())
            tervanix::mqtt::publish_peer_sample(p);
    }
#if TERVANIX_HAS_IMU
    void sample_task(void *)
    {
        char id[18]{};
        get_device_id(id, sizeof(id));
        tervanix::telemetry::ImuSample batch[BATCH_SIZE]{};
        int idx = 0;
        std::uint32_t seq = 1;
        while (true)
        {
            tervanix::telemetry::ImuSample s{};
            if (tervanix::sensor::read_imu(s) == ESP_OK)
            {
                batch[idx++] = s;
                timeval tv{};
                gettimeofday(&tv, nullptr);
                tervanix::telemetry::PeerSample p{};
                p.magic = tervanix::telemetry::PEER_MAGIC;
                p.sequence = seq++;
                p.ttl = 4;
                std::memcpy(
                    p.source_uuid,
                    identity_store.uuid().data(),
                    identity_store.uuid().size());
                std::strncpy(p.device_id, id, sizeof(p.device_id) - 1);
                p.timestamp_ms =
                    (std::int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                p.sample = s;
                tervanix::peer::broadcast_sample(p);
                if (idx == BATCH_SIZE)
                {
                    tervanix::mqtt::publish_batch(batch, BATCH_SIZE, id);
                    idx = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000 / SAMPLE_RATE_HZ));
        }
    }
#endif
    void status_task(void *)
    {
        while (true)
        {
            ESP_LOGI(
                TAG,
                "BLE=%s registration=%s WiFi=%s MQTT=%s role=%s",
                tervanix::ble::connected() ? "up" : "down",
                tervanix::ble::registered() ? "ok" : "waiting",
                tervanix::wifi::is_connected() ? "up" : "down",
                tervanix::mqtt::connected() ? "up" : "down",
                TERVANIX_IS_RELAY ? "relay" : "sensor");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
} // namespace

extern "C" void app_main()
{
    ESP_ERROR_CHECK(initialize_nvs());
    ESP_ERROR_CHECK(identity_store.begin());
    ESP_ERROR_CHECK(registry_store.begin());
    ESP_LOGI(TAG, "Starting %s", TERVANIX_BOARD_NAME);
    ESP_ERROR_CHECK(tervanix::ble::start_registration_client(identity_store));

    xTaskCreate(status_task, "status", 3072, nullptr, 4, nullptr);
    wait_registration();

    auto wr = tervanix::wifi::connect();
    if (wr != ESP_OK)
        ESP_LOGW(TAG, "No infrastructure Wi-Fi; peer relay remains active");

    bool trusted = identity_store.has_uuid() &&
                   identity_store.has_server_uuid() &&
                   registry_store.contains(identity_store.uuid());

    if (tervanix::wifi::is_connected())
    {
        char gateway_ip[16]{};
        tervanix::wifi::get_gateway_ip(gateway_ip, sizeof(gateway_ip));

        trusted =
            tervanix::registration::verify_over_wifi(
                identity_store, registry_store, gateway_ip, 8765U) == ESP_OK;
        if (!trusted)
        {
            ESP_LOGE(
                TAG,
                "Identity or registrar mismatch; node rejects Wi-Fi and peer "
                "networking");
            esp_wifi_disconnect();
            return;
        }
    }

    if (!trusted)
    {
        ESP_LOGE(
            TAG,
            "No previously verified registry snapshot; peer networking "
            "disabled");
        return;
    }

    ESP_ERROR_CHECK(tervanix::peer::start(registry_store));
    tervanix::peer::set_receive_handler(on_peer);

    if (tervanix::wifi::is_connected())
    {
        char gateway_ip[16]{};
        tervanix::wifi::get_gateway_ip(gateway_ip, sizeof(gateway_ip));
        sync_time();
        auto mr = tervanix::mqtt::connect(gateway_ip, 1883U);
        if (mr != ESP_OK)
            ESP_LOGW(TAG, "MQTT unavailable; peer relay remains active");
    }
#if TERVANIX_HAS_IMU
    ESP_ERROR_CHECK(tervanix::sensor::initialize_imu());
    xTaskCreate(sample_task, "sensor", 4096, nullptr, 5, nullptr);
#else
    ESP_LOGI(
        TAG, "Networking-only ESP32 relay build: BMI270 and sampling excluded");
#endif
}
