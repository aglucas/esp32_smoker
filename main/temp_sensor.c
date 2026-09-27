#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "temp_sensor.h"
#include "config.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "temp_sensor";

static const probe_config_t s_probes[NUM_PROBES] = PROBE_TABLE;

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali[NUM_PROBES];
static bool s_cali_ok[NUM_PROBES];

static probe_reading_t s_current[NUM_PROBES];
static SemaphoreHandle_t s_current_mutex;

static hist_entry_t s_history[HISTORY_LEN];
static uint32_t s_history_count = 0;
static uint32_t s_history_head = 0; // index to write next
static SemaphoreHandle_t s_history_mutex;

static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (calibrated) {
        ESP_LOGI(TAG, "ADC calibration ok (unit %d chan %d)", unit, channel);
    } else {
        ESP_LOGW(TAG, "ADC calibration not supported, using raw voltage estimate (unit %d chan %d)", unit, channel);
    }
    return calibrated;
}

static float read_mv(int probe)
{
    int64_t sum_mv = 0;
    int valid = 0;
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, s_probes[probe].channel, &raw) != ESP_OK) {
            continue;
        }
        int mv = 0;
        if (s_cali_ok[probe]) {
            if (adc_cali_raw_to_voltage(s_cali[probe], raw, &mv) != ESP_OK) {
                continue;
            }
        } else {
            mv = (int)(raw * VSUPPLY_MV / 4095.0f);
        }
        sum_mv += mv;
        valid++;
    }
    return valid ? (float)sum_mv / valid : NAN;
}

// Divider: 3V3 -- rRef -- node(ADC) -- probe -- GND
static float mv_to_ohms(const probe_config_t *p, float mv)
{
    if (isnan(mv) || mv >= OPEN_MV) return -1;  // open / unplugged
    if (mv <= SHORT_MV) return -2;               // shorted
    return p->r_ref * mv / (VSUPPLY_MV - mv);
}

static float ohms_to_f(const probe_config_t *p, float r)
{
    double inv_t;
    if (p->sh_c != 0) {
        double ln_r = log(r);
        inv_t = p->sh_a + p->sh_b * ln_r + p->sh_c * ln_r * ln_r * ln_r;
    } else {
        inv_t = 1.0 / (p->t0_c + 273.15) + log(r / p->r0) / p->beta;
    }
    double c = 1.0 / inv_t - 273.15;
    return c * 9.0 / 5.0 + 32.0;
}

esp_err_t temp_sensor_init(void)
{
    s_current_mutex = xSemaphoreCreateMutex();
    s_history_mutex = xSemaphoreCreateMutex();

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,   // ~0-3.1 V input range
    };
    for (int i = 0; i < NUM_PROBES; i++) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_probes[i].channel, &chan_cfg));
        s_cali_ok[i] = adc_calibration_init(ADC_UNIT_1, s_probes[i].channel, ADC_ATTEN_DB_12, &s_cali[i]);
        s_current[i] = (probe_reading_t){ .temp_f = NAN, .ohms = -1, .mv = 0 };
    }
    return ESP_OK;
}

const char *temp_sensor_probe_name(int probe)
{
    return s_probes[probe].name;
}

float temp_sensor_min_readable_f(int probe)
{
    const probe_config_t *p = &s_probes[probe];
    return ohms_to_f(p, p->r_ref * OPEN_MV / (VSUPPLY_MV - OPEN_MV));
}

void temp_sensor_update_current(void)
{
    probe_reading_t r[NUM_PROBES];
    for (int i = 0; i < NUM_PROBES; i++) {
        r[i].mv = read_mv(i);
        r[i].ohms = mv_to_ohms(&s_probes[i], r[i].mv);
        r[i].temp_f = NAN;
        if (r[i].ohms > 0) {
            float f = ohms_to_f(&s_probes[i], r[i].ohms);
            if (f >= -40 && f <= 750) {
                r[i].temp_f = f;
            }
        }
    }

    xSemaphoreTake(s_current_mutex, portMAX_DELAY);
    memcpy(s_current, r, sizeof(s_current));
    xSemaphoreGive(s_current_mutex);
}

void temp_sensor_get_current(probe_reading_t out[NUM_PROBES])
{
    xSemaphoreTake(s_current_mutex, portMAX_DELAY);
    memcpy(out, s_current, sizeof(s_current));
    xSemaphoreGive(s_current_mutex);
}

static volatile uint8_t s_fan_pct = FAN_NO_DATA;

void temp_sensor_set_fan_pct(float pct)
{
    s_fan_pct = (uint8_t)lroundf(pct < 0 ? 0 : pct > 100 ? 100 : pct);
}

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

// Caller holds s_history_mutex.
static void history_append_locked(const hist_entry_t *e)
{
    s_history[s_history_head] = *e;
    s_history_head = (s_history_head + 1) % HISTORY_LEN;
    if (s_history_count < HISTORY_LEN) {
        s_history_count++;
    }
}

void temp_sensor_push_history(void)
{
    probe_reading_t r[NUM_PROBES];
    temp_sensor_get_current(r);

    hist_entry_t e;
    time_t now = time(NULL);
    e.t = now > UNIX_TIME_VALID ? (uint32_t)now : uptime_s();
    for (int i = 0; i < NUM_PROBES; i++) {
        e.v[i] = isnan(r[i].temp_f) ? NO_DATA : (int16_t)lroundf(r[i].temp_f * 10);
    }
    e.fan = s_fan_pct;

    xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    history_append_locked(&e);
    xSemaphoreGive(s_history_mutex);
}

void temp_sensor_reset_history(void)
{
    xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    s_history_count = 0;
    s_history_head = 0;
    xSemaphoreGive(s_history_mutex);
    temp_sensor_push_history();
}

void temp_sensor_history_time_synced(uint32_t unix_now, uint32_t uptime_now,
                                     const hist_entry_t *restored, size_t n_restored)
{
    uint32_t boot_unix = unix_now - uptime_now;

    xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    // Pull out this boot's samples, converting since-boot stamps to unix.
    uint32_t count = s_history_count;
    hist_entry_t *cur = malloc(sizeof(hist_entry_t) * (count ? count : 1));
    if (!cur) {
        xSemaphoreGive(s_history_mutex);
        ESP_LOGW(TAG, "No memory to restore graph history");
        return;
    }
    uint32_t oldest = (s_history_head + HISTORY_LEN - count) % HISTORY_LEN;
    for (uint32_t k = 0; k < count; k++) {
        cur[k] = s_history[(oldest + k) % HISTORY_LEN];
        if (cur[k].t < UNIX_TIME_VALID) {
            cur[k].t += boot_unix;
        }
    }

    // Rebuild: restored rows (only those from before this boot and within
    // the graph window), then this boot's samples. The ring drops the oldest
    // if it overflows.
    s_history_count = 0;
    s_history_head = 0;
    uint32_t window_start = unix_now - HISTORY_HOURS * 3600;
    size_t used = 0;
    for (size_t i = 0; i < n_restored; i++) {
        if (restored[i].t >= window_start && restored[i].t < boot_unix) {
            history_append_locked(&restored[i]);
            used++;
        }
    }
    for (uint32_t k = 0; k < count; k++) {
        history_append_locked(&cur[k]);
    }
    xSemaphoreGive(s_history_mutex);
    free(cur);
    ESP_LOGI(TAG, "Clock synced: graph restored %u points from the flash log", (unsigned)used);
}

size_t temp_sensor_get_history(hist_entry_t *out)
{
    xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    uint32_t count = s_history_count;
    uint32_t oldest = (s_history_head + HISTORY_LEN - count) % HISTORY_LEN;
    for (uint32_t k = 0; k < count; k++) {
        out[k] = s_history[(oldest + k) % HISTORY_LEN];
    }
    xSemaphoreGive(s_history_mutex);
    return count;
}
