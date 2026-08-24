#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "wifi_manager.h"
#include "temp_sensor.h"
#include "sd_logger.h"
#include "web_server.h"

static const char *TAG = "esp32_smoker";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(temp_sensor_init());
    if (sd_logger_init() != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without SD card logging");
    }
    ESP_ERROR_CHECK(wifi_manager_start());
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "esp32_smoker up: sampling every %ds, logging every %ds",
             SAMPLE_INTERVAL_MS / 1000, LOG_INTERVAL_MS / 1000);

    int64_t last_log_us = 0;
    while (1) {
        temp_sensor_update_current();

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us >= (int64_t)LOG_INTERVAL_MS * 1000) {
            last_log_us = now_us;
            temp_point_t p;
            temp_sensor_record_point(&p);
            if (sd_logger_log(p.uptime_s, p.unix_time, p.meat_c, p.grill_c) == ESP_OK) {
                ESP_LOGI(TAG, "Logged point: meat=%.1fC grill=%.1fC", p.meat_c, p.grill_c);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
    }
}
