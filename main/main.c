#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "esp32_smoker";

void app_main(void)
{
    ESP_LOGI(TAG, "esp32_smoker starting up");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
