#include "wifi_manager.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace
{
    constexpr char TAG[] = "wifi";
    constexpr char WIFI_SSID[] = CONFIG_TERVANIX_WIFI_SSID;
    constexpr char WIFI_PASSWORD[] = CONFIG_TERVANIX_WIFI_PASSWORD;

    constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
    constexpr EventBits_t WIFI_FAIL_BIT = BIT1;
    constexpr int MAX_RETRY = 8;

    EventGroupHandle_t wifi_event_group = nullptr;
    std::atomic_bool connected{false};
    int retry_count = 0;
    esp_netif_ip_info_t current_ip_info{};

    void on_wifi_event(
        void *,
        esp_event_base_t event_base,
        int32_t event_id,
        void *event_data)
    {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
        {
            esp_wifi_connect();
            return;
        }

        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            connected.store(false);
            std::memset(&current_ip_info, 0, sizeof(current_ip_info));

            if (retry_count < MAX_RETRY)
            {
                ++retry_count;
                ESP_LOGW(
                    TAG,
                    "Disconnected; reconnecting (%d/%d)",
                    retry_count,
                    MAX_RETRY);
                esp_wifi_connect();
            } else if (wifi_event_group != nullptr)
            {
                xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            }
            return;
        }

        if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
        {
            const auto *got_ip =
                static_cast<const ip_event_got_ip_t *>(event_data);
            if (got_ip == nullptr)
            {
                return;
            }

            current_ip_info = got_ip->ip_info;
            retry_count = 0;
            connected.store(true);

            ESP_LOGI(
                TAG,
                "Wi-Fi ready: ip=" IPSTR " gateway=" IPSTR,
                IP2STR(&current_ip_info.ip),
                IP2STR(&current_ip_info.gw));

            if (wifi_event_group != nullptr)
            {
                xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
            }
        }
    }

} // namespace

namespace tervanix::wifi
{

    esp_err_t connect()
    {
        if (WIFI_SSID[0] == '\0')
        {
            ESP_LOGE(TAG, "CONFIG_TERVANIX_WIFI_SSID is empty");
            return ESP_ERR_INVALID_STATE;
        }

        if (wifi_event_group != nullptr)
        {
            return connected.load() ? ESP_OK : ESP_ERR_INVALID_STATE;
        }

        wifi_event_group = xEventGroupCreate();
        if (wifi_event_group == nullptr)
        {
            return ESP_ERR_NO_MEM;
        }

        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");

        const esp_err_t event_loop_result = esp_event_loop_create_default();
        if (event_loop_result != ESP_OK &&
            event_loop_result != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(
                TAG,
                "esp_event_loop_create_default failed: %s",
                esp_err_to_name(event_loop_result));
            return event_loop_result;
        }

        if (esp_netif_create_default_wifi_sta() == nullptr)
        {
            ESP_LOGE(TAG, "Could not create default Wi-Fi STA netif");
            return ESP_FAIL;
        }

        wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(
            esp_wifi_init(&init_config), TAG, "esp_wifi_init failed");

        esp_event_handler_instance_t wifi_handler{};
        esp_event_handler_instance_t ip_handler{};

        ESP_RETURN_ON_ERROR(
            esp_event_handler_instance_register(
                WIFI_EVENT,
                ESP_EVENT_ANY_ID,
                &on_wifi_event,
                nullptr,
                &wifi_handler),
            TAG,
            "Wi-Fi event registration failed");

        ESP_RETURN_ON_ERROR(
            esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                &on_wifi_event,
                nullptr,
                &ip_handler),
            TAG,
            "IP event registration failed");

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

        ESP_RETURN_ON_ERROR(
            esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode failed");
        ESP_RETURN_ON_ERROR(
            esp_wifi_set_config(WIFI_IF_STA, &wifi_config),
            TAG,
            "set_config failed");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

        ESP_LOGI(TAG, "Connecting to %s", WIFI_SSID);

        const EventBits_t bits = xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(30000));

        if ((bits & WIFI_CONNECTED_BIT) != 0)
        {
            return ESP_OK;
        }

        ESP_LOGE(TAG, "Wi-Fi connection timed out/failed");
        return ESP_FAIL;
    }

    bool is_connected() { return connected.load(); }

    bool get_gateway_ip(char *out, size_t out_size)
    {
        if (out == nullptr || out_size == 0 || !connected.load())
        {
            return false;
        }

        const int written =
            std::snprintf(out, out_size, IPSTR, IP2STR(&current_ip_info.gw));

        return written > 0 && static_cast<size_t>(written) < out_size;
    }

} // namespace tervanix::wifi
