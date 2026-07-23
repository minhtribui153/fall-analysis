#include "wifi_manager.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_group.h"

namespace {

    constexpr char TAG[] = "wifi";

    // TODO: replace with real credentials, or better, pull these from NVS /
    // a provisioning step instead of hardcoding -- fine as a placeholder
    // for getting the pipeline working end-to-end first.
    constexpr char WIFI_SSID[] = "REPLACE_ME";
    constexpr char WIFI_PASSWORD[] = "REPLACE_ME";

    constexpr int WIFI_CONNECTED_BIT = BIT0;
    constexpr int WIFI_FAIL_BIT = BIT1;
    constexpr int MAX_RETRY = 5;

    EventGroupHandle_t wifi_event_group = nullptr;
    int retry_count = 0;
    bool connected = false;

    void on_wifi_event(
        void* argument,
        esp_event_base_t event_base,
        std::int32_t event_id,
        void* event_data)
    {
        (void)argument;
        (void)event_data;

        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            connected = false;
            if (retry_count < MAX_RETRY) {
                esp_wifi_connect();
                retry_count++;
                ESP_LOGW(TAG, "Retrying connection (%d/%d)", retry_count, MAX_RETRY);
            } else {
                xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            }
        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            retry_count = 0;
            connected = true;
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }

}  // namespace

namespace tervanix::wifi {

    esp_err_t connect()
    {
        wifi_event_group = xEventGroupCreate();

        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_config));

        esp_event_handler_instance_t wifi_handler;
        esp_event_handler_instance_t ip_handler;

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, &wifi_handler));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, &ip_handler));

        wifi_config_t wifi_config {};
        std::strncpy(
            reinterpret_cast<char*>(wifi_config.sta.ssid),
            WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);
        std::strncpy(
            reinterpret_cast<char*>(wifi_config.sta.password),
            WIFI_PASSWORD,
            sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Connecting to %s...", WIFI_SSID);

        const EventBits_t bits = xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected");
            return ESP_OK;
        }

        ESP_LOGE(TAG, "Failed to connect after %d retries", MAX_RETRY);
        return ESP_FAIL;
    }

    bool is_connected()
    {
        return connected;
    }

}  // namespace tervanix::wifi
