#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "data_log.h"
#include "temp_sensor.h"

#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "data_log";

#define PARTITION_LABEL "storage"
#define BASE_PATH       DATA_LOG_BASE_PATH
#define LOG_PATH        BASE_PATH "/log.csv"
#define OLD_LOG_PATH    BASE_PATH "/log_old.csv"

// NVS flag: set once the partition has mounted successfully, so a later mount
// failure is never "fixed" by formatting (which would erase the log).
#define NVS_NAMESPACE   "smoker"
#define NVS_KEY_FMT     "log_fmt"

// Graph restore reads at most this much from the end of the log: ~64 bytes a
// row is generous, so this covers the whole graph window.
#define RESTORE_TAIL_BYTES  ((long)HISTORY_HOURS * 3600 / (LOG_INTERVAL_MS / 1000) * 64)
#define RESTORE_MAX_ROWS    2000

static bool s_available = false;
static int s_readers = 0;            // downloads in progress
static SemaphoreHandle_t s_mutex;    // guards file create/append/rotate/clear and s_readers

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

// Caller holds s_mutex.
static esp_err_t write_header_locked(void)
{
    FILE *f = fopen(LOG_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create %s", LOG_PATH);
        return ESP_FAIL;
    }
    fprintf(f, "uptime_s,unix_time,local_time");
    for (int i = 0; i < NUM_PROBES; i++) {
        fprintf(f, ",%s_f", temp_sensor_probe_name(i));
    }
    fprintf(f, "\n");
    fclose(f);
    return ESP_OK;
}

esp_err_t data_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    setenv("TZ", LOG_TIMEZONE, 1);
    tzset();

    // Only format a partition that has never mounted (first boot after
    // flashing the partition table). If one that held a log fails to mount,
    // leave it alone rather than wipe the data.
    uint8_t was_formatted = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_FMT, &was_formatted);
        nvs_close(h);
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = BASE_PATH,
        .partition_label = PARTITION_LABEL,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK && !was_formatted) {
        ESP_LOGI(TAG, "Formatting log partition (first use)");
        esp_vfs_littlefs_unregister(PARTITION_LABEL);
        err = esp_littlefs_format(PARTITION_LABEL);
        if (err == ESP_OK) {
            err = esp_vfs_littlefs_register(&conf);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed (%s); logging disabled, existing data left untouched",
                 esp_err_to_name(err));
        return err;
    }
    if (!was_formatted && nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_FMT, 1);
        nvs_commit(h);
        nvs_close(h);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (file_size(LOG_PATH) < 0) {
        err = write_header_locked();
    }
    xSemaphoreGive(s_mutex);
    if (err != ESP_OK) {
        return err;
    }

    s_available = true;
    size_t total = 0, used = 0;
    esp_littlefs_info(PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "Logging to flash: %u KB used of %u KB", (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

bool data_log_available(void)
{
    return s_available;
}

esp_err_t data_log_append(uint32_t uptime_s, const float temps_f[NUM_PROBES])
{
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }

    char row[128];
    int len;
    time_t now = time(NULL);
    if (now > UNIX_TIME_VALID) {
        struct tm tm;
        localtime_r(&now, &tm);
        char local[24];
        strftime(local, sizeof(local), "%Y-%m-%d %H:%M:%S", &tm);
        len = snprintf(row, sizeof(row), "%" PRIu32 ",%lld,%s", uptime_s, (long long)now, local);
    } else {
        len = snprintf(row, sizeof(row), "%" PRIu32 ",,", uptime_s);
    }
    for (int i = 0; i < NUM_PROBES && len < (int)sizeof(row); i++) {
        len += isnan(temps_f[i]) ? snprintf(row + len, sizeof(row) - len, ",")
                                 : snprintf(row + len, sizeof(row) - len, ",%.1f", temps_f[i]);
    }

    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    // Rotate, unless a download is reading the files right now (it'll happen
    // on a later row instead).
    if (s_readers == 0 && file_size(LOG_PATH) >= LOG_ROTATE_BYTES) {
        remove(OLD_LOG_PATH);
        if (rename(LOG_PATH, OLD_LOG_PATH) == 0) {
            ESP_LOGI(TAG, "Rotated log");
        }
        write_header_locked();
    }
    FILE *f = fopen(LOG_PATH, "a");
    if (f) {
        fprintf(f, "%s\n", row);
        fclose(f);
    } else {
        ESP_LOGE(TAG, "Failed to open %s for append", LOG_PATH);
        err = ESP_FAIL;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

typedef struct {
    uint32_t uptime;
    uint32_t unix_time;     // 0 = logged before the clock synced
    int16_t v[NUM_PROBES];
} parsed_row_t;

// Parses "uptime_s,unix_time,local_time,t0,t1,..." (header/partial lines fail).
static bool parse_row(char *line, parsed_row_t *r)
{
    if (line[0] < '0' || line[0] > '9') {
        return false;
    }
    char *field[3 + NUM_PROBES];
    int nf = 0;
    for (char *p = line; nf < 3 + NUM_PROBES; nf++) {
        field[nf] = p;
        char *comma = strchr(p, ',');
        if (!comma) {
            nf++;
            break;
        }
        *comma = '\0';
        p = comma + 1;
    }
    if (nf < 3 + NUM_PROBES) {
        return false;
    }
    r->uptime = (uint32_t)strtoul(field[0], NULL, 10);
    r->unix_time = (uint32_t)strtoul(field[1], NULL, 10);
    for (int i = 0; i < NUM_PROBES; i++) {
        char *f = field[3 + i];
        f[strcspn(f, "\r\n")] = '\0';
        r->v[i] = *f ? (int16_t)lroundf(strtof(f, NULL) * 10) : NO_DATA;
    }
    return true;
}

// Appends rows from the last `want` bytes of `path` into a ring of `max`
// entries (so the newest rows win if there are too many).
static void read_tail(const char *path, long want, parsed_row_t *rows, size_t max, size_t *total)
{
    long size = file_size(path);
    if (size <= 0 || want <= 0) {
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[160];
    if (size > want) {
        fseek(f, size - want, SEEK_SET);
        fgets(line, sizeof(line), f);   // drop the partial first line
    }
    parsed_row_t r;
    while (fgets(line, sizeof(line), f)) {
        if (parse_row(line, &r)) {
            rows[*total % max] = r;
            (*total)++;
        }
    }
    fclose(f);
}

// Consecutive rows are from the same boot if uptime kept counting up and,
// when both have a wall-clock time, the two clocks advanced together.
static bool same_boot(const parsed_row_t *prev, const parsed_row_t *r)
{
    if (r->uptime <= prev->uptime) {
        return false;
    }
    if (prev->unix_time > UNIX_TIME_VALID && r->unix_time > UNIX_TIME_VALID) {
        int64_t drift = ((int64_t)r->unix_time - prev->unix_time) - ((int64_t)r->uptime - prev->uptime);
        return drift > -5 && drift < 5;
    }
    return true;
}

esp_err_t data_log_read_recent(uint32_t since_unix, hist_entry_t **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    parsed_row_t *ring = malloc(sizeof(parsed_row_t) * RESTORE_MAX_ROWS);
    if (!ring) {
        return ESP_ERR_NO_MEM;
    }

    // Block rotate/clear while reading (appends are fine).
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_readers++;
    long cur_size = file_size(LOG_PATH);
    xSemaphoreGive(s_mutex);

    size_t total = 0;
    if (cur_size < RESTORE_TAIL_BYTES) {
        read_tail(OLD_LOG_PATH, RESTORE_TAIL_BYTES - (cur_size > 0 ? cur_size : 0), ring, RESTORE_MAX_ROWS, &total);
    }
    read_tail(LOG_PATH, RESTORE_TAIL_BYTES, ring, RESTORE_MAX_ROWS, &total);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_readers--;
    xSemaphoreGive(s_mutex);

    // The ring holds the newest `count` rows, oldest at index `first`.
    size_t count = total < RESTORE_MAX_ROWS ? total : RESTORE_MAX_ROWS;
    size_t first = total < RESTORE_MAX_ROWS ? 0 : total % RESTORE_MAX_ROWS;
    hist_entry_t *res = malloc(sizeof(hist_entry_t) * (count ? count : 1));
    if (!res) {
        free(ring);
        return ESP_ERR_NO_MEM;
    }

    // Walk boot sessions (uptime restarts => new boot). Within a session,
    // unix - uptime is constant, so one synced row dates all of them.
    size_t out_n = 0;
    size_t s_start = 0;
    while (s_start < count) {
        size_t s_end = s_start + 1;
        while (s_end < count && same_boot(&ring[(first + s_end - 1) % RESTORE_MAX_ROWS],
                                          &ring[(first + s_end) % RESTORE_MAX_ROWS])) {
            s_end++;
        }
        int64_t offset = 0;
        bool have_offset = false;
        for (size_t k = s_start; k < s_end && !have_offset; k++) {
            const parsed_row_t *r = &ring[(first + k) % RESTORE_MAX_ROWS];
            if (r->unix_time > UNIX_TIME_VALID) {
                offset = (int64_t)r->unix_time - r->uptime;
                have_offset = true;
            }
        }
        for (size_t k = s_start; k < s_end && have_offset; k++) {
            const parsed_row_t *r = &ring[(first + k) % RESTORE_MAX_ROWS];
            uint32_t t = r->unix_time > UNIX_TIME_VALID ? r->unix_time : (uint32_t)(r->uptime + offset);
            if (t >= since_unix) {
                res[out_n].t = t;
                memcpy(res[out_n].v, r->v, sizeof(r->v));
                out_n++;
            }
        }
        s_start = s_end;
    }
    free(ring);
    *out = res;
    *n = out_n;
    return ESP_OK;
}

esp_err_t data_log_clear(void)
{
    if (!s_available) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_readers == 0) {
        remove(OLD_LOG_PATH);
        err = write_header_locked();
        ESP_LOGI(TAG, "Log cleared");
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t data_log_get_info(data_log_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    long cur = file_size(LOG_PATH), old = file_size(OLD_LOG_PATH);
    out->log_bytes = (cur > 0 ? cur : 0) + (old > 0 ? old : 0);
    return esp_littlefs_info(PARTITION_LABEL, &out->fs_total, &out->fs_used);
}

// Sends one file through cb; if skip_header, drops everything up to and
// including the first newline.
static esp_err_t stream_file(const char *path, bool skip_header, char *buf, size_t buf_len,
                             data_log_chunk_cb_t cb, void *ctx)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_OK;   // missing file = nothing to send
    }
    esp_err_t err = ESP_OK;
    size_t n;
    while (err == ESP_OK && (n = fread(buf, 1, buf_len, f)) > 0) {
        char *start = buf;
        if (skip_header) {
            char *nl = memchr(buf, '\n', n);
            if (!nl) {
                continue;
            }
            skip_header = false;
            start = nl + 1;
            n -= (size_t)(start - buf);
        }
        if (n) {
            err = cb(start, n, ctx);
        }
    }
    fclose(f);
    return err;
}

esp_err_t data_log_stream(data_log_chunk_cb_t cb, void *ctx)
{
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    char *buf = malloc(1024);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    // Readers block rotate/clear (not appends), so no mutex is held while the
    // slow network send runs.
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_readers++;
    bool have_old = file_size(OLD_LOG_PATH) >= 0;
    xSemaphoreGive(s_mutex);

    esp_err_t err = ESP_OK;
    if (have_old) {
        err = stream_file(OLD_LOG_PATH, false, buf, 1024, cb, ctx);
    }
    if (err == ESP_OK) {
        err = stream_file(LOG_PATH, have_old, buf, 1024, cb, ctx);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_readers--;
    xSemaphoreGive(s_mutex);
    free(buf);
    return err;
}
