#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

typedef struct {
    bool alerts_on;
    float pit_low;              // 0 = off
    float pit_high;             // 0 = off
    float target[NUM_PROBES];   // [0] = pit target (graph only), [1..] = meat; 0 = off
    // Added later: keep new fields at the end so settings saved by older
    // firmware (a shorter blob) still load (see load_settings).
    float pit_low_delay_s;      // pit must stay below pit_low this long; 0 = alert at once
} alert_settings_t;

#define NTFY_SERVER_MAX 96
#define NTFY_TOPIC_MAX  64

typedef struct {
    bool enabled;
    char server[NTFY_SERVER_MAX + 1];   // e.g. "https://ntfy.sh", no trailing slash
    char topic[NTFY_TOPIC_MAX + 1];
} ntfy_config_t;

// Loads settings from NVS and starts the background sender task.
esp_err_t alerts_init(void);

// Evaluates the alarm state machine against the latest temps (F, NAN = no
// probe). Call every READ_MS.
void alerts_check(const float temps_f[NUM_PROBES]);

void alerts_get_settings(alert_settings_t *out);

// Saves to NVS and resets the alarm state so new thresholds re-arm cleanly.
esp_err_t alerts_set_settings(const alert_settings_t *in);

// Sends a free-form notification (e.g. a cook step starting) through the same
// queue as the alarms. Skipped when "Alerts enabled" is off.
void alerts_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void alerts_get_ntfy(ntfy_config_t *out);

// Validates and saves the ntfy settings to NVS. On ESP_ERR_INVALID_ARG,
// *why says what's wrong. Takes effect for the next alert.
esp_err_t alerts_set_ntfy(const ntfy_config_t *in, const char **why);

// "Start new recording": re-arms every alert, clears the target-hit
// indicators and the last-alert text. Settings are kept.
void alerts_reset_session(void);

// Clears one meat probe's "target reached" indicator and re-arms its
// almost-done / done alerts (e.g. a new piece of meat). If the probe is
// still at/above target it will be marked reached again at the next reading.
esp_err_t alerts_clear_target_hit(int probe);

// Queues a test notification. Returns false (with *why set) if it can't be
// delivered, e.g. no internet in setup-AP mode.
bool alerts_send_test(float pit_f, const char **why);

typedef struct {
    bool hit;        // probe has reached its target since the target was set
    float temp_f;    // reading when it was reached
    int32_t ago_s;   // seconds since it was reached; -1 if unknown (e.g. hit
                     // before a reboot while the clock wasn't synced)
} target_hit_t;

// Per-probe "target reached" latch (index 0, the pit, is always false). The
// latch holds through temp drops and reboots until that probe's target changes.
void alerts_get_target_hits(target_hit_t out[NUM_PROBES]);

// Most recent alert text ("" if none) and its age in seconds (-1 if none).
void alerts_get_last(char *buf, size_t len, int32_t *ago_s);
