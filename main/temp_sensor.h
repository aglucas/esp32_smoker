#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    int64_t unix_time;   // 0 if time has not been synced yet
    uint32_t uptime_s;
    float meat_c;         // probe A - NAN if the probe is disconnected/out of range
    float grill_c;        // probe B - NAN if the probe is disconnected/out of range
} temp_point_t;

esp_err_t temp_sensor_init(void);

// Reads both probes and updates the cached "current" values used for the
// live web readout. Call periodically (SAMPLE_INTERVAL_MS).
void temp_sensor_update_current(void);

void temp_sensor_get_current(float *meat_c, float *grill_c);

// Snapshots the current reading into the RAM history ring buffer, for the
// web chart and SD logging. Call once per LOG_INTERVAL_MS.
void temp_sensor_record_point(temp_point_t *out);

// Copies up to max_points history entries, oldest first. Returns the count.
size_t temp_sensor_get_history(temp_point_t *out, size_t max_points);
