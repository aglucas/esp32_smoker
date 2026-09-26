#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "hal/adc_types.h"
#include "config.h"

typedef struct {
    const char *name;
    adc_channel_t channel;  // MUST be ADC1
    float r_ref;            // divider resistor for this channel (ohms)
    // Beta model, used while sh_c == 0 (i.e. before you calibrate)
    float r0;               // probe resistance at t0_c
    float t0_c;
    float beta;
    // Steinhart-Hart coefficients
    double sh_a, sh_b, sh_c;
} probe_config_t;

typedef struct {
    float temp_f;   // NAN if the probe is unplugged/shorted/out of range
    float ohms;     // -1 = open, -2 = shorted
    float mv;
} probe_reading_t;

#define NO_DATA               INT16_MIN
#define HISTORY_LEN           ((uint32_t)HISTORY_HOURS * 3600UL / SAMPLE_SEC)

// One graph point. `t` is unix seconds once the clock has synced; before that
// it's seconds since boot. The two can't be confused: uptime never reaches
// UNIX_TIME_VALID.
typedef struct {
    uint32_t t;
    int16_t v[NUM_PROBES];   // temp F * 10, NO_DATA if no reading
} hist_entry_t;

esp_err_t temp_sensor_init(void);

const char *temp_sensor_probe_name(int probe);

// Lowest temp (F) the probe can read: below it the divider voltage passes
// OPEN_MV and the reading is indistinguishable from an unplugged probe.
float temp_sensor_min_readable_f(int probe);

// Reads all probes and updates the cached "current" values. Call every READ_MS.
void temp_sensor_update_current(void);

void temp_sensor_get_current(probe_reading_t out[NUM_PROBES]);

// Snapshots the current temps into the RAM history ring buffer used by the
// web graph. Call every SAMPLE_SEC.
void temp_sensor_push_history(void);

// Empties the graph history and records the current reading as its first point.
void temp_sensor_reset_history(void);

// Call once when the clock first syncs after boot (unix_now = time(NULL),
// uptime_now = seconds since boot). Converts the since-boot timestamps to unix
// time and puts `restored` (oldest first, all from before this boot, e.g.
// read back from the flash log) in front of them, so the graph survives a
// power cut.
void temp_sensor_history_time_synced(uint32_t unix_now, uint32_t uptime_now,
                                     const hist_entry_t *restored, size_t n_restored);

// Copies the history (oldest first). `out` must hold HISTORY_LEN entries.
// Returns the count.
size_t temp_sensor_get_history(hist_entry_t *out);
