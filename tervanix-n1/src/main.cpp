#include <cinttypes>

#include "ble_advertiser.hpp"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef TERVANIX_BOARD_NAME
#define TERVANIX_BOARD_NAME "Unknown"
#endif

namespace {

constexpr char TAG[] = "tervanix";

esp_err_t initializeNvs()
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

}  // namespace

extern "C" void app_main()
{
    ESP_ERROR_CHECK(initializeNvs());

    esp_chip_info_t chipInfo {};
    esp_chip_info(&chipInfo);

    ESP_LOGI(TAG, "Tervanix starting");
    ESP_LOGI(TAG, "Board: %s", TERVANIX_BOARD_NAME);
    ESP_LOGI(TAG, "CPU cores: %d", chipInfo.cores);

    ESP_ERROR_CHECK(
        tervanix::ble::start_advertising()
    );

    uint32_t elapsedSeconds = 0;

    while (true) {
        ESP_LOGI(
            TAG,
            "Alive: %" PRIu32 " pings",
            elapsedSeconds++
        );

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}