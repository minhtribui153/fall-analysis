#include "wifi_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/event_groups.h"

namespace
{

    constexpr char TAG[] = "wifi";

    // Set via 'pio run -t menuconfig' -> Tervanix Wi-Fi, or directly in
    // sdkconfig.nesso_n1 -- see tervanix-n1/src/Kconfig.projbuild. Kept
    // out of source so nobody has to edit C++ to point the firmware at
    // a different network.
    constexpr char WIFI_SSID[] = CONFIG_TERVANIX_WIFI_SSID;
    constexpr char WIFI_PASSWORD[] = CONFIG_TERVANIX_WIFI_PASSWORD;

    constexpr int WIFI_CONNECTED_BIT = BIT0;
    constexpr int WIFI_FAIL_BIT = BIT1;
    constexpr int MAX_RETRY = 5;

    esp_netif_ip_info_t current_ip_info{};
    bool have_ip_info = false;

    EventGroupHandle_t wifi_event_group = nullptr;
    bool connected = false;

    void ip_event_handler(
        void *,
        esp_event_base_t event_base,
        int32_t event_id,
        void *event_data)
    {
        if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP ||
            event_data == nullptr)
        {
            return;
        }

        const auto *event = static_cast<const ip_event_got_ip_t *>(event_data);

        current_ip_info = event->ip_info;
        have_ip_info = true;

        ESP_LOGI(
            TAG,
            "sta ip: " IPSTR ", mask: " IPSTR ", gw: " IPSTR,
            IP2STR(&current_ip_info.ip),
            IP2STR(&current_ip_info.netmask),
            IP2STR(&current_ip_info.gw));

        if (wifi_event_group != nullptr)
        {
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }

    void
    on_wifi_event(void *, esp_event_base_t event_base, int32_t event_id, void *)
    {
        if (event_base != WIFI_EVENT)
        {
            return;
        }

        if (event_id == WIFI_EVENT_STA_START)
        {
            esp_wifi_connect();
            return;
        }

        if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            have_ip_info = false;

            if (wifi_event_group != nullptr)
            {
                xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
            }

            esp_wifi_connect();
        }
    }

} // namespace

namespace tervanix::wifi
{

    esp_err_t connect()
    {
        wifi_event_group = xEventGroupCreate();

        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_config));

        ESP_ERROR_CHECK(esp_event_handler_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr));

        ESP_ERROR_CHECK(esp_event_handler_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, nullptr));

        wifi_config_t wifi_config{};
        std::strncpy(
            reinterpret_cast<char *>(wifi_config.sta.ssid),
            WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);
        std::strncpy(
            reinterpret_cast<char *>(wifi_config.sta.password),
            WIFI_PASSWORD,
            sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        if (WIFI_SSID[0] == '\0')
        {
            ESP_LOGW(
                TAG,
                "Wi-Fi SSID is empty; radio remains active for ESP-NOW relay "
                "mode");
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "Connecting to %s...", WIFI_SSID);

        const EventBits_t bits = xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT)
        {
            ESP_LOGI(TAG, "Connected");
            return ESP_OK;
        }

        ESP_LOGE(TAG, "Failed to connect after %d retries", MAX_RETRY);
        return ESP_FAIL;
    }

    bool is_connected() { return connected; }

    esp_err_t get_gateway_ip(char *out, std::size_t out_size)
    {
        if (out == nullptr || out_size == 0)
        {
            return ESP_ERR_INVALID_ARG;
        }

        if (!have_ip_info)
        {
            out[0] = '\0';
            return ESP_ERR_INVALID_STATE;
        }

        const int written =
            std::snprintf(out, out_size, IPSTR, IP2STR(&current_ip_info.gw));

        if (written < 0 || static_cast<std::size_t>(written) >= out_size)
        {
            out[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }

        return ESP_OK;
    }

} // namespace tervanix::wifi
