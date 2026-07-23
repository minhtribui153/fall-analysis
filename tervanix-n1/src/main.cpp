#include "ble_advertiser.hpp"
#include "identity_store.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char tag[] = "main";

tervanix::identity::IdentityStore identity_store;

esp_err_t initialize_nvs()
{
    esp_err_t result = nvs_flash_init();

    if (
        result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }

    return result;
}

void status_task(void*)
{
    while (true) {
        ESP_LOGI(
            tag,
            "BLE=%s registration=%s stored_uuid=%s",
            tervanix::ble::connected()
                ? "CONNECTED"
                : "DISCONNECTED",
            tervanix::ble::registered()
                ? "REGISTERED"
                : "WAITING",
            identity_store.has_uuid()
                ? "YES"
                : "NO"
        );

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

}  // namespace

extern "C" void app_main()
{
    ESP_ERROR_CHECK(initialize_nvs());
    ESP_ERROR_CHECK(identity_store.begin());

    ESP_LOGI(
        tag,
        "Stored UUID present: %s",
        identity_store.has_uuid() ? "yes" : "no"
    );

    ESP_ERROR_CHECK(
        tervanix::ble::start_registration_client(
            identity_store
        )
    );

    xTaskCreate(
        status_task,
        "status",
        3072,
        nullptr,
        4,
        nullptr
    );
}
