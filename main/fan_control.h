#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    FAN_MODE_OFF = 0,
    FAN_MODE_AUTO = 1,     // PID to the pit target
    FAN_MODE_MANUAL = 2,   // fixed manual_pct
} fan_mode_t;

typedef struct {
    uint8_t mode;          // fan_mode_t
    float manual_pct;      // 0-100, used in manual mode
    float kp, ki, kd;      // units in config.h
    float alert_pct;       // auto: alert when the fan is >= this %...
    float alert_min;       // ...for this many minutes (either 0 = off)
    // Add any new fields at the end: load_settings accepts older (shorter)
    // and newer (longer) saved blobs.
} fan_settings_t;

typedef struct {
    float pct;             // output being applied, 0-100
    float p, i, d;         // PID terms (%), auto mode only
    int32_t high_for_s;    // how long the fan has been at/above alert_pct, -1 = not
    const char *note;      // why the fan is off in auto mode, or ""
} fan_status_t;

// Sets up the PWM output (fan off) and loads settings. Call early in boot.
esp_err_t fan_control_init(void);

// Runs the controller with the latest pit reading (F, NAN = none), the pit
// target and the pit high alarm (0 = off). Call every READ_MS.
void fan_control_update(float pit_f, float target_f, float high_f);

void fan_control_get_settings(fan_settings_t *out);

// Validates (clamps) and saves to NVS; resets the PID's memory.
esp_err_t fan_control_set_settings(const fan_settings_t *in);

void fan_control_get_status(fan_status_t *out);
