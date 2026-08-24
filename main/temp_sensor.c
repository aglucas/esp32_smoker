#include <math.h>
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

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_a, s_cali_b;
static bool s_cali_a_ok = false, s_cali_b_ok = false;

static float s_current_meat_c = NAN, s_current_grill_c = NAN;
static SemaphoreHandle_t s_current_mutex;

static temp_point_t s_history[HISTORY_POINTS];
static size_t s_history_count = 0;
static size_t s_history_head = 0; // index to write next
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

static float resistance_to_celsius(float r_ohms)
{
    float t_kelvin = 1.0f / (1.0f / THERMISTOR_T25_KELVIN +
                              (1.0f / THERMISTOR_BETA) * logf(r_ohms / THERMISTOR_R25_OHMS));
    return t_kelvin - 273.15f;
}

static float read_probe_celsius(adc_channel_t channel, adc_cali_handle_t cali, bool cali_ok)
{
    int64_t sum_mv = 0;
    int valid = 0;
    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, channel, &raw) != ESP_OK) {
            continue;
        }
        int mv = 0;
        if (cali_ok) {
            if (adc_cali_raw_to_voltage(cali, raw, &mv) != ESP_OK) {
                continue;
            }
        } else {
            mv = (int)(raw * 3300.0f / 4095.0f);
        }
        sum_mv += mv;
        valid++;
    }
    if (valid == 0) {
        return NAN;
    }
    float mv = (float)sum_mv / valid;

    if (mv <= 1.0f || mv >= 3299.0f) {
        return NAN; // open circuit or shorted probe
    }
    float r_ohms = THERMISTOR_SERIES_R_OHMS * (mv / (3300.0f - mv));
    return resistance_to_celsius(r_ohms);
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
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, PROBE_A_ADC_CHANNEL, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, PROBE_B_ADC_CHANNEL, &chan_cfg));

    s_cali_a_ok = adc_calibration_init(ADC_UNIT_1, PROBE_A_ADC_CHANNEL, ADC_ATTEN_DB_12, &s_cali_a);
    s_cali_b_ok = adc_calibration_init(ADC_UNIT_1, PROBE_B_ADC_CHANNEL, ADC_ATTEN_DB_12, &s_cali_b);

    return ESP_OK;
}

void temp_sensor_update_current(void)
{
    float meat = read_probe_celsius(PROBE_A_ADC_CHANNEL, s_cali_a, s_cali_a_ok);
    float grill = read_probe_celsius(PROBE_B_ADC_CHANNEL, s_cali_b, s_cali_b_ok);

    xSemaphoreTake(s_current_mutex, portMAX_DELAY);
    s_current_meat_c = meat;
    s_current_grill_c = grill;
    xSemaphoreGive(s_current_mutex);
}

void temp_sensor_get_current(float *meat_c, float *grill_c)
{
    xSemaphoreTake(s_current_mutex, portMAX_DELAY);
    *meat_c = s_current_meat_c;
    *grill_c = s_current_grill_c;
    xSemaphoreGive(s_current_mutex);
}

void temp_sensor_record_point(temp_point_t *out)
{
    temp_point_t p;
    temp_sensor_get_current(&p.meat_c, &p.grill_c);
    p.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    p.unix_time = (int64_t)time(NULL);

    xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    s_history[s_history_head] = p;
    s_history_head = (s_history_head + 1) % HISTORY_POINTS;
    if (s_history_count < HISTORY_POINTS) {
        s_history_count++;
    }
    xSemaphoreGive(s_history_mutex);

    if (out) {
        *out = p;
    }
}

size_t temp_sensor_get_history(temp_point_t *out, size_t max_points)
{
    xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    size_t count = s_history_count;
    if (count > max_points) {
        count = max_points;
    }
    size_t oldest = (s_history_head + HISTORY_POINTS - s_history_count) % HISTORY_POINTS;
    size_t skip = s_history_count - count; // if truncated, keep the newest `count` points
    size_t start = (oldest + skip) % HISTORY_POINTS;
    for (size_t i = 0; i < count; i++) {
        out[i] = s_history[(start + i) % HISTORY_POINTS];
    }
    xSemaphoreGive(s_history_mutex);
    return count;
}
