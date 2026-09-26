#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "wifi_manager.h"
#include "temp_sensor.h"
#include "data_log.h"
#include "web_server.h"
#include "alerts.h"
#include "cook_plan.h"

static const char *TAG = "esp32_smoker";

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed; use the IP address instead");
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("ESP32 Smoker");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "Open http://%s.local", MDNS_HOSTNAME);
}

// Serial output doubles as a calibration log (mV + resistance per probe).
static void log_readings(const probe_reading_t r[NUM_PROBES])
{
    char line[256];
    int len = 0;
    for (int i = 0; i < NUM_PROBES && len < (int)sizeof(line); i++) {
        char temp[12];
        if (r[i].ohms == -1) {
            snprintf(temp, sizeof(temp), "<%.0f/open", temp_sensor_min_readable_f(i));
        } else if (r[i].ohms == -2) {
            snprintf(temp, sizeof(temp), "short");
        } else if (isnan(r[i].temp_f)) {
            snprintf(temp, sizeof(temp), "--");
        } else {
            snprintf(temp, sizeof(temp), "%.1f", r[i].temp_f);
        }
        len += snprintf(line + len, sizeof(line) - len, "%s%s: %s F (%.0f mV, %.0f ohm)",
                        i ? " | " : "", temp_sensor_probe_name(i), temp, r[i].mv, r[i].ohms);
    }
    ESP_LOGI(TAG, "%s", line);
}

// After a power cut the graph (RAM) starts empty. Once SNTP sets the clock we
// know how long the power was out, so reload the graph window from the flash
// log. Without internet (setup AP) the clock never syncs and this never runs.
static void restore_graph_from_log(int64_t now_us)
{
    uint32_t unix_now = (uint32_t)time(NULL);
    hist_entry_t *rows = NULL;
    size_t n = 0;
    if (data_log_read_recent(unix_now - HISTORY_HOURS * 3600, &rows, &n) != ESP_OK) {
        n = 0;
    }
    temp_sensor_history_time_synced(unix_now, (uint32_t)(now_us / 1000000), rows, n);
    free(rows);
}

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
    if (data_log_init() != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without flash logging");
    }
    ESP_ERROR_CHECK(alerts_init());
    ESP_ERROR_CHECK(cook_plan_init());
    ESP_ERROR_CHECK(wifi_manager_start());
    start_mdns();
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "esp32_smoker up: reading every %ds, graph every %ds, flash log every %ds",
             READ_MS / 1000, SAMPLE_SEC, LOG_INTERVAL_MS / 1000);

    temp_sensor_update_current();
    temp_sensor_push_history();
    int64_t last_hist_us = esp_timer_get_time();
    int64_t last_log_us = 0;
    bool clock_synced = false;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        temp_sensor_update_current();

        probe_reading_t r[NUM_PROBES];
        float temps_f[NUM_PROBES];
        temp_sensor_get_current(r);
        for (int i = 0; i < NUM_PROBES; i++) {
            temps_f[i] = r[i].temp_f;
        }
        alerts_check(temps_f);
        cook_plan_tick();
        log_readings(r);

        int64_t now_us = esp_timer_get_time();
        if (!clock_synced && time(NULL) > UNIX_TIME_VALID) {
            clock_synced = true;
            restore_graph_from_log(now_us);
        }
        if (now_us - last_hist_us >= (int64_t)SAMPLE_SEC * 1000000) {
            last_hist_us = now_us;
            temp_sensor_push_history();
        }
        if (last_log_us == 0 || now_us - last_log_us >= (int64_t)LOG_INTERVAL_MS * 1000) {
            last_log_us = now_us;
            data_log_append((uint32_t)(now_us / 1000000), temps_f);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(READ_MS));
    }
}
