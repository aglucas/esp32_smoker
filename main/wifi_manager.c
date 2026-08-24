#include <string.h>
#include <stdlib.h>

#include "wifi_manager.h"
#include "config.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "cJSON.h"

static const char *TAG = "wifi_mgr";

#define NVS_NAMESPACE "wifi_cfg"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_ap_active = false;
static wifi_status_t s_status = {0};
static SemaphoreHandle_t s_status_mutex;

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
            if (!s_ap_active) {
                ESP_LOGW(TAG, "Giving up on station connection, starting fallback SoftAP");
                start_ap();
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        s_retry_num = 0;

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.sta_connected = true;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&event->ip_info.ip));
        xSemaphoreGive(s_status_mutex);

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

    char ssid[33] = {0};
    char pass[65] = {0};
    bool have_creds = false;
    load_creds(ssid, sizeof(ssid), pass, sizeof(pass), &have_creds);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (have_creds) {
        wifi_config_t sta_config = {0};
        strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
        xSemaphoreGive(s_status_mutex);

        ESP_LOGI(TAG, "Connecting to saved network '%s'...", ssid);
        esp_wifi_connect();

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to '%s'", ssid);
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Could not connect to '%s' within timeout, falling back to AP", ssid);
    } else {
        ESP_LOGI(TAG, "No saved network, starting fallback AP");
    }

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
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
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

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
    xSemaphoreGive(s_status_mutex);

    esp_wifi_connect();
    return ESP_OK;
}
