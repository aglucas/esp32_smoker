#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "config.h"
#include "temp_sensor.h"

// Where the LittleFS "storage" partition is mounted. Other modules keep their
// own files here too (e.g. cook plans).
#define DATA_LOG_BASE_PATH "/log"

// True once the partition is mounted.
bool data_log_available(void);

// Mounts the LittleFS "storage" partition (formatting it on first boot) and
// makes sure a log file with a CSV header exists. Non-fatal on failure: the
// rest of the app keeps running without logging.
esp_err_t data_log_init(void);

// Appends one CSV row: uptime_s,unix_time,local_time,<one column per probe in F>,fan_pct.
// NAN temps (and a negative fan_pct) are written as empty cells; time columns
// are blank until SNTP syncs.
esp_err_t data_log_append(uint32_t uptime_s, const float temps_f[NUM_PROBES], float fan_pct);

// Deletes all logged data and starts a fresh file. Fails with
// ESP_ERR_INVALID_STATE while a download is in progress, and
// ESP_ERR_NOT_SUPPORTED if flash logging isn't available.
esp_err_t data_log_clear(void);

// Reads logged rows at or after since_unix back into graph entries (oldest
// first, t = unix seconds) so the graph can be rebuilt after a power cut.
// Rows logged before the clock synced get their time from another row of the
// same boot that has one; boots that never synced are skipped. *out is
// malloc'd (caller frees); *n may be 0.
esp_err_t data_log_read_recent(uint32_t since_unix, hist_entry_t **out, size_t *n);

typedef struct {
    size_t log_bytes;    // size of the logged data (both files)
    size_t fs_total;     // partition capacity
    size_t fs_used;
} data_log_info_t;

esp_err_t data_log_get_info(data_log_info_t *out);

// Streams the whole log (older file first, one header) through `cb` in
// chunks. Stops and returns cb's error if it fails.
typedef esp_err_t (*data_log_chunk_cb_t)(const char *buf, size_t len, void *ctx);
esp_err_t data_log_stream(data_log_chunk_cb_t cb, void *ctx);
