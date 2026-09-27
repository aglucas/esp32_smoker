#include <string.h>
#include <stdlib.h>

#include "wifi_manager.h"
#include "config.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "wifi_mgr";

#define NVS_NAMESPACE "wifi_cfg"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_ap_active = false;
static bool s_defer_ap_fallback = false;
static wifi_status_t s_status = {0};
static SemaphoreHandle_t s_status_mutex;

// Background reconnect while the fallback AP is up (see reconnect_task).
static volatile bool s_scanning;          // a page-requested scan is running
static volatile int64_t s_user_connect_us; // last Connect from the setup page
static int s_candidate;                   // alternates between known networks

static void start_ap(void);

static void start_sntp_once(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_config);
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.sta_connected = false;
        xSemaphoreGive(s_status_mutex);

        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry connect to AP (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            if (!s_ap_active && !s_defer_ap_fallback) {
                ESP_LOGW(TAG, "Giving up on station connection, starting fallback SoftAP");
                start_ap();
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        s_retry_num = 0;

        wifi_ap_record_t ap_info;
        bool have_info = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.sta_connected = true;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&event->ip_info.ip));
        if (have_info) {
            strlcpy(s_status.ssid, (const char *)ap_info.ssid, sizeof(s_status.ssid));
        }
        xSemaphoreGive(s_status_mutex);
        ESP_LOGI(TAG, "Connected to '%s' (" IPSTR ")", have_info ? (const char *)ap_info.ssid : "?",
                 IP2STR(&event->ip_info.ip));

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        start_sntp_once();

        if (s_ap_active) {
            ESP_LOGI(TAG, "Station connected, disabling fallback SoftAP");
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            s_ap_active = false;
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.ap_mode = false;
            xSemaphoreGive(s_status_mutex);
        }
    }
}

static void load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len, bool *found)
{
    *found = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t s_len = ssid_len;
    size_t p_len = pass_len;
    esp_err_t e1 = nvs_get_str(h, "ssid", ssid, &s_len);
    esp_err_t e2 = nvs_get_str(h, "pass", pass, &p_len);
    nvs_close(h);

    if (e1 == ESP_OK && strlen(ssid) > 0) {
        *found = true;
        if (e2 != ESP_OK) {
            pass[0] = '\0';
        }
    }
}

static esp_err_t save_creds(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", password ? password : "");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void start_ap(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    wifi_config_t ap_config = {0};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = AP_CHANNEL;
    ap_config.ap.max_connection = AP_MAX_CONN;
    ap_config.ap.authmode = strlen(AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    strncpy((char *)ap_config.ap.password, AP_PASSWORD, sizeof(ap_config.ap.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.ap_mode = true;
    strncpy(s_status.ssid, (char *)ap_config.ap.ssid, sizeof(s_status.ssid) - 1);
    strncpy(s_status.ip, "192.168.4.1", sizeof(s_status.ip) - 1);
    xSemaphoreGive(s_status_mutex);

    s_ap_active = true;
    ESP_LOGI(TAG, "SoftAP started: SSID=%s", ap_config.ap.ssid);
}

static bool try_connect_sta(const char *ssid, const char *password)
{
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password ? password : "", sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
    xSemaphoreGive(s_status_mutex);

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to '%s'", ssid);
        return true;
    }
    ESP_LOGW(TAG, "Could not connect to '%s' within timeout", ssid);
    return false;
}

// Next network to retry: alternates between the secrets.h default and the
// one saved from the page (skipping whichever isn't set). False if none.
static bool pick_candidate(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    char saved_ssid[33] = {0}, saved_pass[65] = {0};
    bool have_saved = false;
    load_creds(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass), &have_saved);
    if (have_saved && strcmp(saved_ssid, DEFAULT_STA_SSID) == 0) {
        have_saved = false;   // same network twice
    }
    bool have_default = DEFAULT_STA_SSID[0] != '\0';
    int n = (have_default ? 1 : 0) + (have_saved ? 1 : 0);
    if (n == 0) {
        return false;
    }
    bool use_default = have_default && (!have_saved || (s_candidate++ % 2 == 0));
    strlcpy(ssid, use_default ? DEFAULT_STA_SSID : saved_ssid, ssid_len);
    strlcpy(pass, use_default ? DEFAULT_STA_PASSWORD : saved_pass, pass_len);
    return true;
}

// While the fallback AP is up (no home Wi-Fi), try the home network again
// every WIFI_RECONNECT_SEC. The AP stays up meanwhile so the page keeps
// working; on success the GOT_IP handler turns the AP off. A failed attempt
// just ends quietly: s_retry_num is maxed so the disconnect handler doesn't
// burst-retry, and the AP is already running.
static void reconnect_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_SEC * 1000));
        wifi_status_t st;
        wifi_manager_get_status(&st);
        if (!s_ap_active || st.sta_connected || s_scanning ||
            esp_timer_get_time() - s_user_connect_us < (int64_t)WIFI_RECONNECT_SEC * 1000000) {
            continue;
        }
        char ssid[33], pass[65];
        if (!pick_candidate(ssid, sizeof(ssid), pass, sizeof(pass))) {
            continue;   // nothing to try until a network is set on the page
        }
        wifi_config_t sta_config = {0};
        strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
        strlcpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password));
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

        ESP_LOGI(TAG, "Retrying Wi-Fi '%s' in the background (setup AP stays up)", ssid);
        s_retry_num = WIFI_MAX_RETRY;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        if (esp_wifi_set_config(WIFI_IF_STA, &sta_config) == ESP_OK) {
            esp_wifi_connect();
        }
    }
}

esp_err_t wifi_manager_start(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(reconnect_task, "wifi_retry", 3072, NULL, 3, NULL);

    // Defer the disconnect handler's own AP fallback while we work through
    // the candidate list below; we start the AP ourselves once (and only
    // once) every candidate has been exhausted.
    s_defer_ap_fallback = true;

    // Skipped when no default network is set (no main/secrets.h).
    if (DEFAULT_STA_SSID[0] && try_connect_sta(DEFAULT_STA_SSID, DEFAULT_STA_PASSWORD)) {
        s_defer_ap_fallback = false;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        return ESP_OK;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    bool have_creds = false;
    load_creds(ssid, sizeof(ssid), pass, sizeof(pass), &have_creds);

    if (have_creds && strcmp(ssid, DEFAULT_STA_SSID) != 0) {
        if (try_connect_sta(ssid, pass)) {
            s_defer_ap_fallback = false;
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            return ESP_OK;
        }
    } else if (!have_creds) {
        ESP_LOGI(TAG, "No saved network configured");
    }

    ESP_LOGW(TAG, "No network reachable, starting fallback AP");
    s_defer_ap_fallback = false;
    if (!s_ap_active) {
        start_ap();
    }
    return ESP_OK;
}

void wifi_manager_get_status(wifi_status_t *out)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mutex);
}

esp_err_t wifi_manager_scan_json(char **out_json)
{
    wifi_scan_config_t scan_config = {0};
    scan_config.show_hidden = false;
    s_scanning = true;
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    s_scanning = false;
    if (err != ESP_OK) {
        return err;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) {
        num = 20;
    }
    wifi_ap_record_t records[20];
    uint16_t got = num;
    esp_wifi_scan_get_ap_records(&got, records);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < got; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", (char *)records[i].ssid);
        cJSON_AddNumberToObject(n, "rssi", records[i].rssi);
        cJSON_AddBoolToObject(n, "secure", records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, n);
    }
    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t wifi_manager_save_and_connect(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_creds(ssid, password);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password ? password : "", sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    s_user_connect_us = esp_timer_get_time();   // background retries wait a round
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    // In AP mode the status keeps showing the AP's own name (the setup page
    // tells people to join it); the real network name is filled in on connect.
    if (!s_ap_active) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
        xSemaphoreGive(s_status_mutex);
    }

    esp_wifi_connect();
    return ESP_OK;
}
