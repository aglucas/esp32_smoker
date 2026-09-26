#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "web_server.h"
#include "wifi_manager.h"
#include "temp_sensor.h"
#include "alerts.h"
#include "data_log.h"
#include "cook_plan.h"
#include "config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "web_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

// Reads a (small) request body into buf, NUL-terminated.
static bool read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    int total = req->content_len < (int)buf_len - 1 ? req->content_len : (int)buf_len - 1;
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            return false;
        }
        received += r;
    }
    buf[received] = '\0';
    return true;
}

// Reads a request body of up to `max` bytes into a malloc'd, NUL-terminated
// buffer. Returns NULL (after sending an error response) on failure.
static char *read_body_alloc(httpd_req_t *req, size_t max)
{
    if (req->content_len > max) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"too much data\"}");
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf || !read_body(req, buf, req->content_len + 1)) {
        free(buf);
        httpd_resp_send_500(req);
        return NULL;
    }
    return buf;
}

static esp_err_t send_ok_or_why(httpd_req_t *req, esp_err_t err, const char *why)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(resp, "why", why ? why : "error");
    }
    return send_json(req, resp);
}

static esp_err_t plans_get_handler(httpd_req_t *req)
{
    char *json = cook_plan_get_plans_json();
    if (!json) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

// Body is the full plan list: {"plans":[...]}.
static esp_err_t plans_post_handler(httpd_req_t *req)
{
    char *body = read_body_alloc(req, 16 * 1024);
    if (!body) {
        return ESP_OK;
    }
    const char *why = NULL;
    esp_err_t err = cook_plan_save_plans(body, &why);
    free(body);
    return send_ok_or_why(req, err, why);
}

// Body: {"name":"3-2-1 ribs"}
static esp_err_t plan_start_post_handler(httpd_req_t *req)
{
    char buf[128];
    if (!read_body(req, buf, sizeof(buf))) {
        return httpd_resp_send_500(req);
    }
    cJSON *root = cJSON_Parse(buf);
    const cJSON *name = cJSON_GetObjectItem(root, "name");
    const char *why = "missing plan name";
    esp_err_t err = cJSON_IsString(name) ? cook_plan_start(name->valuestring, &why) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    return send_ok_or_why(req, err, why);
}

static esp_err_t plan_stop_post_handler(httpd_req_t *req)
{
    cook_plan_stop();
    return send_ok_or_why(req, ESP_OK, NULL);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    size_t len = index_html_end - index_html_start;
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t now_get_handler(httpd_req_t *req)
{
    wifi_status_t ws;
    wifi_manager_get_status(&ws);
    probe_reading_t r[NUM_PROBES];
    temp_sensor_get_current(r);
    alert_settings_t s;
    alerts_get_settings(&s);
    char last_alert[256];
    int32_t last_alert_ago;
    alerts_get_last(last_alert, sizeof(last_alert), &last_alert_ago);
    target_hit_t hits[NUM_PROBES];
    alerts_get_target_hits(hits);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(root, "alertsOn", s.alerts_on);
    cJSON_AddNumberToObject(root, "pitLow", s.pit_low);
    cJSON_AddNumberToObject(root, "pitHigh", s.pit_high);
    cJSON_AddNumberToObject(root, "pitLowDelay", s.pit_low_delay_s);
    cJSON_AddBoolToObject(root, "apMode", ws.ap_mode);
    cJSON_AddBoolToObject(root, "staConnected", ws.sta_connected);
    cJSON_AddStringToObject(root, "ssid", ws.ssid);
    cJSON_AddStringToObject(root, "ip", ws.ip);
    cJSON_AddStringToObject(root, "lastAlert", last_alert);
    cJSON_AddNumberToObject(root, "lastAlertAgo", last_alert_ago);

    ntfy_config_t ntfy;
    alerts_get_ntfy(&ntfy);
    cook_plan_add_status(root);

    cJSON *nj = cJSON_AddObjectToObject(root, "ntfy");
    cJSON_AddBoolToObject(nj, "enabled", ntfy.enabled);
    cJSON_AddStringToObject(nj, "server", ntfy.server);
    cJSON_AddStringToObject(nj, "topic", ntfy.topic);

    data_log_info_t log_info;
    if (data_log_get_info(&log_info) == ESP_OK) {
        cJSON_AddNumberToObject(root, "logBytes", log_info.log_bytes);
        cJSON_AddNumberToObject(root, "logFsTotal", log_info.fs_total);
    } else {
        cJSON_AddNullToObject(root, "logBytes");
    }

    cJSON *arr = cJSON_AddArrayToObject(root, "probes");
    for (int i = 0; i < NUM_PROBES; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", temp_sensor_probe_name(i));
        if (isnan(r[i].temp_f)) {
            cJSON_AddNullToObject(p, "temp");
        } else {
            cJSON_AddNumberToObject(p, "temp", roundf(r[i].temp_f * 10) / 10);
        }
        cJSON_AddNumberToObject(p, "target", s.target[i]);
        cJSON_AddNumberToObject(p, "ohms", roundf(r[i].ohms));
        cJSON_AddNumberToObject(p, "mv", roundf(r[i].mv));
        cJSON_AddNumberToObject(p, "minTemp", roundf(temp_sensor_min_readable_f(i)));
        cJSON_AddBoolToObject(p, "hit", hits[i].hit);
        if (hits[i].hit) {
            cJSON_AddNumberToObject(p, "hitTemp", roundf(hits[i].temp_f));
            cJSON_AddNumberToObject(p, "hitAgo", hits[i].ago_s);
        }
        cJSON_AddItemToArray(arr, p);
    }
    return send_json(req, root);
}

// Streams history in chunks so we never build the whole ~45 KB response in RAM.
static esp_err_t history_get_handler(httpd_req_t *req)
{
    hist_entry_t *rows = malloc(sizeof(hist_entry_t) * HISTORY_LEN);
    if (!rows) {
        return httpd_resp_send_500(req);
    }
    size_t count = temp_sensor_get_history(rows);
    // Each row starts with its age in seconds, so the page can place points on
    // a real time axis (gaps from power cuts included).
    time_t unix_now = time(NULL);
    uint32_t uptime_now = (uint32_t)(esp_timer_get_time() / 1000000);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char buf[1400];
    int len = snprintf(buf, sizeof(buf), "{\"interval\":%d,\"rows\":[", SAMPLE_SEC);
    esp_err_t err = ESP_OK;
    bool first = true;
    for (size_t k = 0; k < count && err == ESP_OK; k++) {
        int64_t age = rows[k].t >= UNIX_TIME_VALID ? (int64_t)unix_now - rows[k].t
                                                   : (int64_t)uptime_now - rows[k].t;
        if (age < 0 || age > (int64_t)HISTORY_HOURS * 3600) {
            continue;
        }
        len += snprintf(buf + len, sizeof(buf) - len, "%s[%" PRId64, first ? "" : ",", age);
        first = false;
        for (int p = 0; p < NUM_PROBES; p++) {
            int16_t v = rows[k].v[p];
            if (v == NO_DATA) {
                len += snprintf(buf + len, sizeof(buf) - len, ",null");
            } else {
                len += snprintf(buf + len, sizeof(buf) - len, ",%.1f", v / 10.0f);
            }
        }
        len += snprintf(buf + len, sizeof(buf) - len, "]");
        if (len > 1200) {
            err = httpd_resp_send_chunk(req, buf, len);
            len = 0;
        }
    }
    free(rows);
    if (err != ESP_OK) {
        return err;
    }
    len += snprintf(buf + len, sizeof(buf) - len, "]}");
    err = httpd_resp_send_chunk(req, buf, len);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);   // end of chunked response
    }
    return err;
}

// Body is application/x-www-form-urlencoded: alertsOn=1&pitLow=200&pitHigh=300&t1=203...
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        return httpd_resp_send_500(req);
    }

    alert_settings_t s;
    alerts_get_settings(&s);
    char val[16];
    if (httpd_query_key_value(body, "alertsOn", val, sizeof(val)) == ESP_OK) s.alerts_on = strcmp(val, "1") == 0;
    if (httpd_query_key_value(body, "pitLow", val, sizeof(val)) == ESP_OK)   s.pit_low = strtof(val, NULL);
    if (httpd_query_key_value(body, "pitHigh", val, sizeof(val)) == ESP_OK)  s.pit_high = strtof(val, NULL);
    if (httpd_query_key_value(body, "pitLowDelay", val, sizeof(val)) == ESP_OK) {
        float d = strtof(val, NULL);
        s.pit_low_delay_s = d < 0 ? 0 : d > PIT_LOW_DELAY_MAX_S ? PIT_LOW_DELAY_MAX_S : d;
    }
    if (httpd_query_key_value(body, "pitTarget", val, sizeof(val)) == ESP_OK) s.target[0] = strtof(val, NULL);
    for (int i = 1; i < NUM_PROBES; i++) {
        char key[8];
        snprintf(key, sizeof(key), "t%d", i);
        if (httpd_query_key_value(body, key, val, sizeof(val)) == ESP_OK) s.target[i] = strtof(val, NULL);
    }

    esp_err_t err = alerts_set_settings(&s);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t test_post_handler(httpd_req_t *req)
{
    probe_reading_t r[NUM_PROBES];
    temp_sensor_get_current(r);
    const char *why = NULL;
    bool ok = alerts_send_test(r[0].temp_f, &why);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(root, "why", why);
    }
    return send_json(req, root);
}

static esp_err_t send_log_chunk(const char *buf, size_t len, void *ctx)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, buf, len);
}

static esp_err_t log_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"smoker_log.csv\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = data_log_stream(send_log_chunk, req);
    if (err == ESP_ERR_INVALID_STATE) {
        // Nothing sent yet: logging isn't available.
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "flash log not available");
    }
    if (err != ESP_OK) {
        return err;   // client went away mid-download
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// "Start new recording": wipes the flash log, restarts the graph, and
// re-arms alerts / target-hit indicators. Settings are kept.
static esp_err_t reset_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = data_log_clear();
    if (err == ESP_ERR_INVALID_STATE) {
        // Nothing else is reset either, so the two stay in step.
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"a log download is in progress, try again when it finishes\"}");
    }
    temp_sensor_reset_history();
    alerts_reset_session();
    ESP_LOGI(TAG, "Started new recording");

    if (err == ESP_OK || err == ESP_ERR_NOT_SUPPORTED) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"graph and alerts reset, but the flash log could not be cleared\"}");
}

// Body is JSON: {"enabled":true,"server":"https://ntfy.sh","topic":"..."}
static esp_err_t ntfy_post_handler(httpd_req_t *req)
{
    char buf[256];
    httpd_resp_set_type(req, "application/json");
    if (!read_body(req, buf, sizeof(buf))) {
        return httpd_resp_send_500(req);
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"why\":\"invalid json\"}");
    }

    ntfy_config_t c;
    alerts_get_ntfy(&c);
    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    cJSON *server = cJSON_GetObjectItem(root, "server");
    cJSON *topic = cJSON_GetObjectItem(root, "topic");
    if (cJSON_IsBool(enabled)) c.enabled = cJSON_IsTrue(enabled);
    if (cJSON_IsString(server)) strlcpy(c.server, server->valuestring, sizeof(c.server));
    if (cJSON_IsString(topic))  strlcpy(c.topic, topic->valuestring, sizeof(c.topic));
    cJSON_Delete(root);

    const char *why = NULL;
    esp_err_t err = alerts_set_ntfy(&c, &why);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(resp, "why", why ? why : "error");
    }
    return send_json(req, resp);
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
    if (!read_body(req, buf, sizeof(buf))) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

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
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;   // phones hold idle sockets open
    config.stack_size = 6144;         // history handler formats floats into a 1.4 KB buffer

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/api/now",      .method = HTTP_GET,  .handler = now_get_handler },
        { .uri = "/api/history",  .method = HTTP_GET,  .handler = history_get_handler },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler },
        { .uri = "/api/test",     .method = HTTP_POST, .handler = test_post_handler },
        { .uri = "/api/log.csv",  .method = HTTP_GET,  .handler = log_get_handler },
        { .uri = "/api/reset",    .method = HTTP_POST, .handler = reset_post_handler },
        { .uri = "/api/ntfy",     .method = HTTP_POST, .handler = ntfy_post_handler },
        { .uri = "/api/plans",    .method = HTTP_GET,  .handler = plans_get_handler },
        { .uri = "/api/plans",    .method = HTTP_POST, .handler = plans_post_handler },
        { .uri = "/api/plan/start", .method = HTTP_POST, .handler = plan_start_post_handler },
        { .uri = "/api/plan/stop",  .method = HTTP_POST, .handler = plan_stop_post_handler },
        { .uri = "/api/scan",     .method = HTTP_GET,  .handler = scan_get_handler },
        { .uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}
