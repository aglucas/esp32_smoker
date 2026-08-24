#include <string.h>
#include <stdlib.h>

#include "web_server.h"
#include "wifi_manager.h"
#include "temp_sensor.h"
#include "config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    size_t len = index_html_end - index_html_start;
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    wifi_status_t ws;
    wifi_manager_get_status(&ws);
    float meat, grill;
    temp_sensor_get_current(&meat, &grill);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ap_mode", ws.ap_mode);
    cJSON_AddBoolToObject(root, "sta_connected", ws.sta_connected);
    cJSON_AddStringToObject(root, "ssid", ws.ssid);
    cJSON_AddStringToObject(root, "ip", ws.ip);
    cJSON_AddNumberToObject(root, "meat_c", meat);
    cJSON_AddNumberToObject(root, "grill_c", grill);
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t history_get_handler(httpd_req_t *req)
{
    temp_point_t *points = malloc(sizeof(temp_point_t) * HISTORY_POINTS);
    if (!points) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    size_t count = temp_sensor_get_history(points, HISTORY_POINTS);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "points");
    for (size_t i = 0; i < count; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "uptime_s", points[i].uptime_s);
        cJSON_AddNumberToObject(p, "unix_time", points[i].unix_time);
        cJSON_AddNumberToObject(p, "meat_c", points[i].meat_c);
        cJSON_AddNumberToObject(p, "grill_c", points[i].grill_c);
        cJSON_AddItemToArray(arr, p);
    }
    free(points);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    char *json = NULL;
    esp_err_t err = wifi_manager_scan_json(&json);
    if (err != ESP_OK || !json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"scan failed\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char buf[256];
    int total = req->content_len < (int)sizeof(buf) - 1 ? req->content_len : (int)sizeof(buf) - 1;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            httpd_resp_send_500(req);
            return ESP_OK;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"invalid json\"}");
        return ESP_OK;
    }
    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"missing ssid\"}");
        return ESP_OK;
    }

    esp_err_t err = wifi_manager_save_and_connect(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, err == ESP_OK ? "{\"result\":\"connecting\"}" : "{\"result\":\"error\"}");
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler };
    httpd_uri_t history_uri = { .uri = "/api/history", .method = HTTP_GET, .handler = history_get_handler };
    httpd_uri_t scan_uri = { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_get_handler };
    httpd_uri_t wifi_uri = { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &history_uri);
    httpd_register_uri_handler(server, &scan_uri);
    httpd_register_uri_handler(server, &wifi_uri);

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}
