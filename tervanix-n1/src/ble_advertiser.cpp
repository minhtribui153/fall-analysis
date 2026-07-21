#include "ble_advertiser.hpp"
#include "gatt_server.hpp"

#include <cstdint>
#include <cstring>

#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

namespace {

    constexpr char TAG[] = "ble";

    std::uint8_t address_type = 0;

    int on_gap_event(
        ble_gap_event* event,
        void* argument);

    void start_ble_advertising()
    {
        ble_hs_adv_fields fields {};

        fields.flags =
            BLE_HS_ADV_F_DISC_GEN |
            BLE_HS_ADV_F_BREDR_UNSUP;

        const char device_name[] = "Tervanix-Node";

        fields.name =
            reinterpret_cast<const std::uint8_t*>(
                device_name
            );

        fields.name_len =
            static_cast<std::uint8_t>(
                std::strlen(device_name)
            );

        fields.name_is_complete = 1;

        const int fields_result =
            ble_gap_adv_set_fields(&fields);

        if (fields_result != 0) {
            ESP_LOGE(
                TAG,
                "Advertising fields failed: %d",
                fields_result
            );
            return;
        }

        ble_gap_adv_params parameters {};

        // Connectable advertising.
        parameters.conn_mode =
            BLE_GAP_CONN_MODE_UND;

        // Generally discoverable.
        parameters.disc_mode =
            BLE_GAP_DISC_MODE_GEN;

        const int result =
            ble_gap_adv_start(
                address_type,
                nullptr,
                BLE_HS_FOREVER,
                &parameters,
                on_gap_event,
                nullptr
            );

        if (result != 0) {
            ESP_LOGE(
                TAG,
                "Advertising start failed: %d",
                result
            );
            return;
        }

        ESP_LOGI(
            TAG,
            "Advertising continuously as %s",
            device_name
        );
    }

    int on_gap_event(ble_gap_event* event, void* argument)
    {
        (void)argument;

        switch (event->type) {
            case BLE_GAP_EVENT_CONNECT:
                if (event->connect.status == 0) {
                    ESP_LOGI(TAG, "BLE client connected");
                } else {
                    ESP_LOGW(
                        TAG,
                        "Connection failed: %d",
                        event->connect.status
                    );

                    start_ble_advertising();
                }
                break;

            case BLE_GAP_EVENT_DISCONNECT:
                ESP_LOGI(TAG, "BLE client disconnected");
                start_ble_advertising();
                break;

            case BLE_GAP_EVENT_ADV_COMPLETE:
                ESP_LOGI(TAG, "Advertising stopped");
                start_ble_advertising();
                break;

            default:
                break;
        }

        return 0;
    }

    void on_nimble_sync()
    {
        ESP_LOGI(TAG, "NimBLE host synchronized");

        int result =
            ble_hs_id_infer_auto(
                0,
                &address_type
            );

        if (result != 0) {
            ESP_LOGE(
                TAG,
                "Failed to determine address type: %d",
                result
            );
            return;
        }

        std::uint8_t address[6] {};

        result = ble_hs_id_copy_addr(
            address_type,
            address,
            nullptr
        );

        if (result != 0) {
            ESP_LOGE(
                TAG,
                "Failed to read BLE address: %d",
                result
            );
            return;
        }

        ESP_LOGI(
            TAG,
            "BLE address: "
            "%02X:%02X:%02X:%02X:%02X:%02X",
            address[5],
            address[4],
            address[3],
            address[2],
            address[1],
            address[0]
        );

        start_ble_advertising();
    }

    void on_nimble_reset(int reason)
    {
        ESP_LOGE(
            TAG,
            "NimBLE host reset: %d",
            reason
        );
    }

    void nimble_host_task(void* parameter)
    {
        (void)parameter;

        ESP_LOGI(TAG, "NimBLE host task started");

        nimble_port_run();

        ESP_LOGW(TAG, "NimBLE host stopped");
        nimble_port_freertos_deinit();
    }

}  // namespace

namespace tervanix::ble {

    esp_err_t start_advertising()
    {
        ESP_LOGI(TAG, "Initializing NimBLE");

        const esp_err_t result =
            nimble_port_init();

        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "NimBLE initialization failed: %s",
                esp_err_to_name(result)
            );

            return result;
        }

        const int gatt_result =
            initialize_gatt_server();

        if (gatt_result != 0) {
            ESP_LOGE(
                TAG,
                "GATT initialization failed: %d",
                gatt_result
            );

            return ESP_FAIL;
        }

        ble_hs_cfg.sync_cb = on_nimble_sync;
        ble_hs_cfg.reset_cb = on_nimble_reset;

        nimble_port_freertos_init(
            nimble_host_task
        );

        return ESP_OK;
    }

}  // namespace tervanix::ble