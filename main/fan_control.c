#include <math.h>
#include <stddef.h>
#include <string.h>

#include "fan_control.h"
#include "config.h"
#include "alerts.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "fan";

#define NVS_NAMESPACE   "smoker"
#define NVS_KEY_FAN     "fan2"   // current layout
#define NVS_KEY_FAN_V1  "fan"    // older layouts: only mode..kd are read from it
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_CHANNEL    LEDC_CHANNEL_0
// 80 MHz / (freq x 2^bits) must be a clock divider of 1..1023: 13 bits covers
// ~10 Hz to ~9.7 kHz. (10 bits could not go below ~80 Hz.)
#define DUTY_BITS       LEDC_TIMER_13_BIT
#define DUTY_MAX        ((1u << 13) - 1)
#define D_FILTER        0.3f                   // smoothing for the derivative's noisy slope

static fan_settings_t s_cfg = {
    .mode = FAN_MODE_AUTO,
    .manual_pct = 50,
    .kp = DEFAULT_FAN_KP,
    .ki = DEFAULT_FAN_KI,
    .kd = DEFAULT_FAN_KD,
    .alert_pct = DEFAULT_FAN_ALERT_PCT,
    .alert_min = DEFAULT_FAN_ALERT_MIN,
};
static fan_status_t s_status = { .note = "", .high_for_s = -1 };

// Set when the pit reaches the high alarm; the fan then stays off until the
// pit has cooled back to the target (not just below the high alarm).
static bool s_over_high;

// Fan alert state
static int64_t s_high_since_us;   // 0 = fan not at/above alert_pct
static bool s_high_alert_sent;

// PID memory
static float s_integral;          // % contribution, kept within 0-100
static float s_prev_pit = NAN;
static float s_slope_f_per_min;   // filtered pit slope
static int64_t s_prev_us;

static float s_applied_pct;       // after the minimum-speed rule
static esp_timer_handle_t s_kick_timer;
static SemaphoreHandle_t s_mutex;

static void set_duty(float pct)
{
    uint32_t duty = (uint32_t)lroundf(pct / 100.0f * DUTY_MAX);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

// End of the start-up kick: drop from 100% to the real output.
static void kick_done(void *arg)
{
    set_duty(s_applied_pct);
}

// Caller holds s_mutex.
static void apply_locked(float pct)
{
    if (pct > 0 && pct < FAN_MIN_PCT) {
        pct = FAN_MIN_PCT;   // the fan stalls below this even after the kick
    }
    bool starting = pct > 0 && s_applied_pct == 0;
    s_applied_pct = pct;
    s_status.pct = pct;
    if (starting && FAN_KICK_MS > 0) {
        set_duty(100);
        esp_timer_stop(s_kick_timer);
        esp_timer_start_once(s_kick_timer, (uint64_t)FAN_KICK_MS * 1000);
    } else if (!esp_timer_is_active(s_kick_timer)) {
        set_duty(pct);
    } else if (pct == 0) {
        esp_timer_stop(s_kick_timer);
        set_duty(0);
    }
}

// Caller holds s_mutex.
static void reset_pid_locked(void)
{
    s_integral = 0;
    s_prev_pit = NAN;
    s_slope_f_per_min = 0;
    s_prev_us = 0;
}

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    // Roomier buffer, pre-filled with defaults: a shorter blob leaves the
    // newer fields at their defaults, a longer one's extras are ignored.
    struct {
        fan_settings_t s;
        uint8_t spare[32];
    } tmp = { .s = s_cfg };
    size_t len = sizeof(tmp);
    const size_t base = offsetof(fan_settings_t, alert_pct);   // mode..kd
    if (nvs_get_blob(h, NVS_KEY_FAN, &tmp, &len) == ESP_OK && len >= base) {
        s_cfg = tmp.s;
    } else {
        // Older firmware stored other fields after kd (a since-removed Min %),
        // so take only mode..kd from it.
        tmp.s = s_cfg;
        len = sizeof(tmp);
        if (nvs_get_blob(h, NVS_KEY_FAN_V1, &tmp, &len) == ESP_OK && len >= base) {
            memcpy(&s_cfg, &tmp.s, base);
        }
    }
    ESP_LOGI(TAG, "Fan settings: mode %d, Kp %.2f Ki %.2f Kd %.2f, alert %.0f%% for %.0f min",
             s_cfg.mode, s_cfg.kp, s_cfg.ki, s_cfg.kd, s_cfg.alert_pct, s_cfg.alert_min);
    nvs_close(h);
}

esp_err_t fan_control_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = DUTY_BITS,
        .freq_hz = FAN_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {
        .gpio_num = FAN_GPIO,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));

    const esp_timer_create_args_t kick_args = { .callback = kick_done, .name = "fan_kick" };
    ESP_ERROR_CHECK(esp_timer_create(&kick_args, &s_kick_timer));

    load_settings();
    ESP_LOGI(TAG, "Fan PWM on GPIO%d at %d Hz", FAN_GPIO, FAN_PWM_FREQ_HZ);
    return ESP_OK;
}

void fan_control_update(float pit_f, float target_f, float high_f)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int64_t now_us = esp_timer_get_time();
    float dt_s = s_prev_us ? (now_us - s_prev_us) / 1e6f : READ_MS / 1000.0f;
    s_prev_us = now_us;

    float out = 0;
    s_status.p = s_status.i = s_status.d = 0;
    s_status.note = "";

    if (!isnan(pit_f)) {
        if (high_f > 0 && pit_f >= high_f) {
            s_over_high = true;
        } else if (s_over_high && target_f > 0 && pit_f <= target_f) {
            s_over_high = false;   // back at target: hand control back to the PID
        }
    }

    if (s_cfg.mode == FAN_MODE_OFF) {
        s_status.note = "off";
        reset_pid_locked();
    } else if (s_cfg.mode == FAN_MODE_MANUAL) {
        out = s_cfg.manual_pct;
        reset_pid_locked();
    } else if (target_f <= 0) {
        s_status.note = "no pit target set";
        reset_pid_locked();
    } else if (isnan(pit_f)) {
        // Don't blow on a fire we can't measure.
        s_status.note = "no pit reading";
        reset_pid_locked();
    } else if (s_over_high) {
        s_status.note = "pit went over the high alarm; off until it cools to the target";
        s_integral = 0;       // restart the I term fresh when control resumes
        s_prev_pit = pit_f;   // keep the slope continuous
    } else {
        float err = target_f - pit_f;   // positive = pit too cold = more air

        // Slope of the pit temp (F/min), smoothed: raw 2 s readings are noisy.
        if (!isnan(s_prev_pit) && dt_s > 0) {
            float slope = (pit_f - s_prev_pit) / dt_s * 60.0f;
            s_slope_f_per_min += D_FILTER * (slope - s_slope_f_per_min);
        }
        s_prev_pit = pit_f;

        float p = s_cfg.kp * err;
        if (s_cfg.ki > 0) {
            // Keeping the integral inside 0-100% stops it winding up while
            // the fan is pinned at 0 or 100.
            s_integral += s_cfg.ki * err * dt_s / 60.0f;
            s_integral = fminf(fmaxf(s_integral, 0), 100);
        } else {
            s_integral = 0;
        }
        float d = -s_cfg.kd * s_slope_f_per_min;   // rising pit = ease off

        out = fminf(fmaxf(p + s_integral + d, 0), 100);
        s_status.p = p;
        s_status.i = s_integral;
        s_status.d = d;
    }
    apply_locked(out);

    // Fan alert: in Auto, fan at/above alert_pct continuously for alert_min.
    if (s_cfg.mode == FAN_MODE_AUTO && s_cfg.alert_pct > 0 && s_cfg.alert_min > 0 &&
        s_applied_pct >= s_cfg.alert_pct) {
        if (!s_high_since_us) {
            s_high_since_us = now_us;
        }
        int64_t high_s = (now_us - s_high_since_us) / 1000000;
        s_status.high_for_s = (int32_t)high_s;
        if (!s_high_alert_sent && high_s >= (int64_t)(s_cfg.alert_min * 60)) {
            s_high_alert_sent = true;
            alerts_notify("Fan at %.0f%%+ for %.0f min (now %.0f%%): check the fire's fuel and vents",
                          s_cfg.alert_pct, s_cfg.alert_min, s_applied_pct);
        }
    } else {
        // Dropping below the threshold re-arms the alert.
        s_high_since_us = 0;
        s_high_alert_sent = false;
        s_status.high_for_s = -1;
    }
    xSemaphoreGive(s_mutex);
}

void fan_control_get_settings(fan_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mutex);
}

esp_err_t fan_control_set_settings(const fan_settings_t *in)
{
    fan_settings_t c = *in;
    if (c.mode > FAN_MODE_MANUAL) {
        c.mode = FAN_MODE_AUTO;
    }
    c.manual_pct = fminf(fmaxf(c.manual_pct, 0), 100);
    c.kp = fminf(fmaxf(c.kp, 0), 100);
    c.ki = fminf(fmaxf(c.ki, 0), 100);
    c.kd = fminf(fmaxf(c.kd, 0), 100);
    c.alert_pct = fminf(fmaxf(c.alert_pct, 0), 100);
    c.alert_min = fminf(fmaxf(c.alert_min, 0), 1440);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = c;
    reset_pid_locked();
    s_over_high = false;   // re-evaluated at the next reading
    s_high_since_us = 0;
    s_high_alert_sent = false;
    s_status.high_for_s = -1;
    if (c.mode == FAN_MODE_MANUAL) {
        apply_locked(c.manual_pct);   // respond now, not at the next update
    } else if (c.mode == FAN_MODE_OFF) {
        apply_locked(0);
    }
    xSemaphoreGive(s_mutex);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_FAN, &c, sizeof(c));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Fan settings: mode %d, manual %.0f%%, Kp %.2f Ki %.2f Kd %.2f, alert %.0f%% for %.0f min",
             c.mode, c.manual_pct, c.kp, c.ki, c.kd, c.alert_pct, c.alert_min);
    return err;
}

void fan_control_get_status(fan_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}
