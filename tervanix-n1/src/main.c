#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "REM";

void app_main(void)
{
    ESP_LOGI(TAG, "Hello world from ESP-IDF on ESP32!");

    while (true) {
        ESP_LOGI(TAG, "Running");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}