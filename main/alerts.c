#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "alerts.h"
#include "temp_sensor.h"
#include "wifi_manager.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "alerts";

#define NVS_NAMESPACE   "smoker"
#define NVS_KEY         "settings"
#define NVS_KEY_HITS    "hits"
#define NVS_KEY_NTFY    "ntfy"
#define NVS_KEY_ARM     "arm"
#define ARM_HEARTBEAT_S 300          // refresh the saved "armed" timestamp this often
#define ALERT_MSG_LEN   256
#define ALERT_QUEUE_LEN 8

typedef struct {
    char text[ALERT_MSG_LEN];
    int64_t created_us;     // esp_timer time, for "sent late" and max-age checks
} alert_msg_t;

// Pit-low armed state saved in NVS so a power blip mid-cook doesn't disarm it.
typedef struct {
    uint8_t armed;
    uint32_t unix_time;     // last time it was confirmed armed (0 = clock not synced)
} pit_arm_rec_t;

static alert_settings_t s_settings = {
    .alerts_on = true,
    .pit_low = DEFAULT_PIT_LOW_F,
    .pit_high = DEFAULT_PIT_HIGH_F,
    .target = DEFAULT_TARGETS_F,
    .pit_low_delay_s = DEFAULT_PIT_LOW_DELAY_S,
};

// alarm state
static bool s_pit_armed, s_pit_low_sent, s_pit_high_sent, s_pit_lost_sent;
static int64_t s_pit_low_since_us;   // 0 = pit not currently low
static uint32_t s_arm_saved_unix;    // unix time last written to NVS
static bool s_arm_restore_pending;   // saved "armed" found at boot, waiting for the clock
static uint32_t s_arm_restore_unix;
static bool s_done_sent[NUM_PROBES];
static bool s_near_sent[NUM_PROBES];

// "Target reached" latch per meat probe. Unlike the alert state above it
// ignores alerts_on, survives temp drops (resting, probe pulled) and reboots
// (saved in NVS), and is cleared only when that probe's target changes.
typedef struct {
    bool hit;
    float target_f;     // target it was reached against (validated on load)
    float temp_f;       // reading when it was reached
    int64_t unix_time;  // 0 if the clock wasn't synced yet
} target_hit_rec_t;
static target_hit_rec_t s_hits[NUM_PROBES];
static int64_t s_hit_us[NUM_PROBES];   // esp_timer time of the hit; 0 = not this boot

static ntfy_config_t s_ntfy = {
    .enabled = USE_NTFY,
    .server = NTFY_SERVER,
    .topic = NTFY_TOPIC,
};

static char s_last_alert[ALERT_MSG_LEN];
static int64_t s_last_alert_us;      // 0 = never

static SemaphoreHandle_t s_mutex;    // guards everything above
static QueueHandle_t s_queue;

// ---------------------------------------------------------------------
// Delivery (runs on the sender task: TLS needs more stack than the loop)
// ---------------------------------------------------------------------
static bool send_ntfy(const ntfy_config_t *ntfy, const char *msg)
{
    char url[NTFY_SERVER_MAX + NTFY_TOPIC_MAX + 2];
    snprintf(url, sizeof(url), "%s/%s", ntfy->server, ntfy->topic);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Title", "Smoker");
    esp_http_client_set_header(client, "Priority", "high");
    esp_http_client_set_header(client, "Tags", "fire");
    esp_http_client_set_post_field(client, msg, strlen(msg));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "ntfy -> %s, HTTP %d", esp_err_to_name(err), status);
    esp_http_client_cleanup(client);
    return err == ESP_OK && status == 200;
}


static bool internet_up(void)
{
    wifi_status_t ws;
    wifi_manager_get_status(&ws);
    return ws.sta_connected;
}

// Delivers queued alerts in order. Each one waits for Wi-Fi and is retried
// until ntfy accepts it, so an alert raised while offline arrives late
// (marked with how late) instead of being lost. Queued alerts live in RAM,
// so a power cut still loses them.
static void sender_task(void *arg)
{
    alert_msg_t m;
    while (1) {
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        while (1) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            ntfy_config_t ntfy = s_ntfy;
            xSemaphoreGive(s_mutex);
            if (!ntfy.enabled) {
                break;   // turned off: nothing to deliver to
            }
            int64_t age_s = (esp_timer_get_time() - m.created_us) / 1000000;
            if (age_s > (int64_t)ALERT_MAX_AGE_MIN * 60) {
                ESP_LOGW(TAG, "Dropping alert undelivered for %lld min: %s", (long long)(age_s / 60), m.text);
                break;
            }
            if (!internet_up()) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            char text[ALERT_MSG_LEN + 32];
            if (age_s >= 60) {
                snprintf(text, sizeof(text), "%s (sent %lld min late)", m.text, (long long)(age_s / 60));
            } else {
                strlcpy(text, m.text, sizeof(text));
            }
            if (send_ntfy(&ntfy, text)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(ALERT_RETRY_SEC * 1000));
        }
    }
}

// Caller holds s_mutex.
static void send_alert(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void send_alert(const char *fmt, ...)
{
    alert_msg_t m;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m.text, sizeof(m.text), fmt, ap);
    va_end(ap);

    ESP_LOGW(TAG, "ALERT: %s", m.text);
    strlcpy(s_last_alert, m.text, sizeof(s_last_alert));
    s_last_alert_us = m.created_us = esp_timer_get_time();
    // Queue even when offline; the sender waits for Wi-Fi. If the queue is
    // full (long outage), drop the oldest waiting alert to make room.
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        alert_msg_t dropped;
        if (xQueueReceive(s_queue, &dropped, 0) == pdTRUE) {
            ESP_LOGW(TAG, "alert queue full, dropping oldest: %s", dropped.text);
        }
        xQueueSend(s_queue, &m, 0);
    }
}

// ---------------------------------------------------------------------
// Alarm state machine
// ---------------------------------------------------------------------
// Caller holds s_mutex.
static void save_arm_locked(bool armed, uint32_t unix_time)
{
    pit_arm_rec_t rec = { .armed = armed, .unix_time = unix_time };
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, NVS_KEY_ARM, &rec, sizeof(rec)) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
    s_arm_saved_unix = unix_time;
}

static uint32_t unix_now_or_0(void)
{
    time_t now = time(NULL);
    return now > UNIX_TIME_VALID ? (uint32_t)now : 0;
}

// Caller holds s_mutex.
static void set_pit_armed_locked(void)
{
    if (!s_pit_armed) {
        s_pit_armed = true;
        s_arm_restore_pending = false;
        save_arm_locked(true, unix_now_or_0());
    }
}

// Once the clock is known: restore a saved "armed" from a recent power blip,
// and keep the saved timestamp fresh while armed. Caller holds s_mutex.
static void update_arm_persistence_locked(void)
{
    uint32_t now = unix_now_or_0();
    if (!now) {
        return;
    }
    if (s_arm_restore_pending) {
        s_arm_restore_pending = false;
        if (s_arm_restore_unix && now - s_arm_restore_unix <= PIT_ARM_RESTORE_MIN * 60) {
            s_pit_armed = true;
            ESP_LOGI(TAG, "Pit low alarm still armed from before the reboot");
        } else {
            ESP_LOGI(TAG, "Saved pit-low arm is stale; starting disarmed");
        }
    }
    if (s_pit_armed && now - s_arm_saved_unix >= ARM_HEARTBEAT_S) {
        save_arm_locked(true, now);
    }
}

static void reset_state_locked(void)
{
    if (s_pit_armed || s_arm_restore_pending) {
        save_arm_locked(false, 0);
    }
    s_arm_restore_pending = false;
    s_pit_armed = s_pit_low_sent = s_pit_high_sent = s_pit_lost_sent = false;
    s_pit_low_since_us = 0;
    for (int i = 0; i < NUM_PROBES; i++) {
        s_done_sent[i] = s_near_sent[i] = false;
    }
}

static void save_hits_locked(void);

// Caller holds s_mutex.
static void update_target_hits_locked(const float temps_f[NUM_PROBES], int64_t now_us)
{
    bool changed = false;
    for (int i = 1; i < NUM_PROBES; i++) {
        float t = s_settings.target[i];
        float f = temps_f[i];
        if (s_hits[i].hit || t <= 0 || isnan(f) || f < t) {
            continue;
        }
        time_t now = time(NULL);
        s_hits[i] = (target_hit_rec_t){
            .hit = true,
            .target_f = t,
            .temp_f = f,
            .unix_time = now > UNIX_TIME_VALID ? (int64_t)now : 0,
        };
        s_hit_us[i] = now_us;
        changed = true;
        ESP_LOGI(TAG, "%s reached target %.0fF", temp_sensor_probe_name(i), t);
    }
    if (changed) {
        save_hits_locked();
    }
}

void alerts_check(const float temps_f[NUM_PROBES])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    update_target_hits_locked(temps_f, esp_timer_get_time());
    if (!s_settings.alerts_on) {
        xSemaphoreGive(s_mutex);
        return;
    }
    int64_t now_us = esp_timer_get_time();
    float pit = temps_f[0];
    update_arm_persistence_locked();

    // ---- Pit ----
    if (!isnan(pit)) {
        s_pit_lost_sent = false;
        if (s_settings.pit_low > 0) {
            // Don't alarm during warm-up: arm only after the pit first reaches temp.
            if (!s_pit_armed && pit >= s_settings.pit_low) {
                set_pit_armed_locked();
            }
            if (s_pit_armed) {
                if (pit < s_settings.pit_low) {
                    if (!s_pit_low_since_us) {
                        s_pit_low_since_us = now_us;
                    }
                    if (!s_pit_low_sent &&
                        now_us - s_pit_low_since_us >= (int64_t)(s_settings.pit_low_delay_s * 1000000.0f)) {
                        send_alert("Pit is LOW: %.0fF (alarm %.0fF)", pit, s_settings.pit_low);
                        s_pit_low_sent = true;
                    }
                } else {
                    s_pit_low_since_us = 0;
                    // Back at the low setting: re-arm (another 60 s below
                    // it will alert again).
                    if (s_pit_low_sent) {
                        s_pit_low_sent = false;
                        send_alert("Pit recovered: %.0fF", pit);
                    }
                }
            }
        }
        if (s_settings.pit_high > 0) {
            if (!s_pit_high_sent && pit > s_settings.pit_high) {
                send_alert("Pit is HIGH: %.0fF (alarm %.0fF)", pit, s_settings.pit_high);
                s_pit_high_sent = true;
            } else if (s_pit_high_sent && pit < s_settings.pit_high - PIT_HYST_F) {
                s_pit_high_sent = false;
            }
        }
    } else if (s_pit_armed && !s_pit_lost_sent) {
        // An open divider also means "colder than the probe can read", so a
        // dying fire lands here too.
        send_alert("Pit probe lost: unplugged or pit below %.0fF!", temp_sensor_min_readable_f(0));
        s_pit_lost_sent = true;
    }

    // ---- Meat ----
    for (int i = 1; i < NUM_PROBES; i++) {
        float t = s_settings.target[i];
        float f = temps_f[i];
        if (t <= 0 || isnan(f)) {
            continue;
        }
        const char *name = temp_sensor_probe_name(i);
        if (!s_done_sent[i] && f >= t) {
            send_alert("%s is DONE: %.0fF (target %.0fF)", name, f, t);
            s_done_sent[i] = s_near_sent[i] = true;
        } else if (NEAR_DONE_F > 0 && !s_near_sent[i] && f >= t - NEAR_DONE_F) {
            send_alert("%s almost done: %.0fF / %.0fF", name, f, t);
            s_near_sent[i] = true;
        }
        if (f < t - MEAT_REARM_F) {
            s_done_sent[i] = s_near_sent[i] = false;  // new piece of meat
        }
    }
    xSemaphoreGive(s_mutex);
}

// ---------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------
static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;   // nothing saved yet, keep defaults
    }
    // Start from the defaults so fields missing from an older, shorter blob
    // keep their default. Other sizes (e.g. a different NUM_PROBES) are dropped.
    alert_settings_t tmp = s_settings;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, NVS_KEY, &tmp, &len) == ESP_OK &&
        (len == sizeof(tmp) || len == offsetof(alert_settings_t, pit_low_delay_s))) {
        s_settings = tmp;
        ESP_LOGI(TAG, "Loaded settings from NVS%s", len == sizeof(tmp) ? "" : " (older format)");
    }
    target_hit_rec_t hits[NUM_PROBES];
    len = sizeof(hits);
    if (nvs_get_blob(h, NVS_KEY_HITS, hits, &len) == ESP_OK && len == sizeof(hits)) {
        for (int i = 1; i < NUM_PROBES; i++) {
            // Only keep a hit that still matches the saved target.
            if (hits[i].hit && hits[i].target_f == s_settings.target[i]) {
                s_hits[i] = hits[i];
            }
        }
    }
    pit_arm_rec_t arm;
    len = sizeof(arm);
    if (nvs_get_blob(h, NVS_KEY_ARM, &arm, &len) == ESP_OK && len == sizeof(arm) && arm.armed) {
        // Decided once the clock syncs (see update_arm_persistence_locked).
        s_arm_restore_pending = true;
        s_arm_restore_unix = arm.unix_time;
    }
    ntfy_config_t ntfy;
    len = sizeof(ntfy);
    if (nvs_get_blob(h, NVS_KEY_NTFY, &ntfy, &len) == ESP_OK && len == sizeof(ntfy)) {
        ntfy.server[NTFY_SERVER_MAX] = ntfy.topic[NTFY_TOPIC_MAX] = '\0';
        s_ntfy = ntfy;
        ESP_LOGI(TAG, "ntfy: %s %s/%s", ntfy.enabled ? "on" : "off", ntfy.server, ntfy.topic);
    }
    nvs_close(h);
}

// Caller holds s_mutex. Written only when a probe first hits its target or a
// target changes, so flash wear is negligible.
static void save_hits_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, NVS_KEY_HITS, s_hits, sizeof(s_hits)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static esp_err_t save_settings(const alert_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY, s, sizeof(*s));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------
esp_err_t alerts_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(ALERT_QUEUE_LEN, sizeof(alert_msg_t));
    if (!s_mutex || !s_queue) {
        return ESP_ERR_NO_MEM;
    }
    load_settings();
    if (xTaskCreate(sender_task, "alert_tx", 8192, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void alerts_get_settings(alert_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_mutex);
}

esp_err_t alerts_set_settings(const alert_settings_t *in)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool hits_changed = false;
    for (int i = 1; i < NUM_PROBES; i++) {
        if (in->target[i] != s_settings.target[i] && s_hits[i].hit) {
            s_hits[i] = (target_hit_rec_t){0};
            s_hit_us[i] = 0;
            hits_changed = true;
        }
    }
    if (hits_changed) {
        save_hits_locked();
    }
    s_settings = *in;   // target[0] is the pit target (graph only)
    reset_state_locked();
    alert_settings_t copy = s_settings;
    xSemaphoreGive(s_mutex);
    return save_settings(&copy);
}

void alerts_notify(const char *fmt, ...)
{
    char text[ALERT_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_settings.alerts_on) {
        send_alert("%s", text);
    } else {
        ESP_LOGI(TAG, "Alerts off, not sending: %s", text);
    }
    xSemaphoreGive(s_mutex);
}

void alerts_get_ntfy(ntfy_config_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_ntfy;
    xSemaphoreGive(s_mutex);
}

esp_err_t alerts_set_ntfy(const ntfy_config_t *in, const char **why)
{
    ntfy_config_t c = *in;
    c.server[NTFY_SERVER_MAX] = c.topic[NTFY_TOPIC_MAX] = '\0';

    // Server: http(s) URL, no spaces; drop trailing slashes.
    size_t n = strlen(c.server);
    while (n && c.server[n - 1] == '/') {
        c.server[--n] = '\0';
    }
    bool https = strncmp(c.server, "https://", 8) == 0;
    if ((!https && strncmp(c.server, "http://", 7) != 0) || n <= (https ? 8u : 7u) || strchr(c.server, ' ')) {
        *why = "server must be a URL like https://ntfy.sh";
        return ESP_ERR_INVALID_ARG;
    }
    // Topic: ntfy allows letters, digits, '-' and '_'.
    n = strlen(c.topic);
    if (n == 0) {
        *why = "topic is required";
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = c.topic[i];
        if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_') {
            *why = "topic can only use letters, numbers, - and _";
            return ESP_ERR_INVALID_ARG;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ntfy = c;
    xSemaphoreGive(s_mutex);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_NTFY, &c, sizeof(c));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        *why = "could not save to flash";
    }
    ESP_LOGI(TAG, "ntfy set: %s %s/%s", c.enabled ? "on" : "off", c.server, c.topic);
    return err;
}

void alerts_reset_session(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    reset_state_locked();
    memset(s_hits, 0, sizeof(s_hits));
    memset(s_hit_us, 0, sizeof(s_hit_us));
    save_hits_locked();
    s_last_alert[0] = '\0';
    s_last_alert_us = 0;
    xSemaphoreGive(s_mutex);
}

bool alerts_send_test(float pit_f, const char **why)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (isnan(pit_f)) {
        send_alert("Test alert from your smoker thermometer. Pit: no probe");
    } else {
        send_alert("Test alert from your smoker thermometer. Pit: %.0fF", pit_f);
    }
    xSemaphoreGive(s_mutex);

    if (!internet_up()) {
        *why = "no internet right now; it will be sent when Wi-Fi reconnects";
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ntfy_on = s_ntfy.enabled;
    xSemaphoreGive(s_mutex);
    if (!ntfy_on) {
        *why = "ntfy is turned off (Notifications > Change)";
        return false;
    }
    return true;
}

void alerts_get_target_hits(target_hit_t out[NUM_PROBES])
{
    int64_t now_us = esp_timer_get_time();
    time_t now = time(NULL);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < NUM_PROBES; i++) {
        out[i].hit = s_hits[i].hit;
        out[i].temp_f = s_hits[i].temp_f;
        out[i].ago_s = -1;
        if (!s_hits[i].hit) {
            continue;
        }
        if (s_hit_us[i]) {
            out[i].ago_s = (int32_t)((now_us - s_hit_us[i]) / 1000000);
        } else if (s_hits[i].unix_time && now > UNIX_TIME_VALID) {
            // Hit before a reboot: only the wall clock can say when.
            out[i].ago_s = (int32_t)(now - s_hits[i].unix_time);
        }
    }
    xSemaphoreGive(s_mutex);
}

void alerts_get_last(char *buf, size_t len, int32_t *ago_s)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(buf, s_last_alert, len);
    *ago_s = s_last_alert_us ? (int32_t)((esp_timer_get_time() - s_last_alert_us) / 1000000) : -1;
    xSemaphoreGive(s_mutex);
}
