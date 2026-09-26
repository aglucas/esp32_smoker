#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cook_plan.h"
#include "data_log.h"
#include "alerts.h"
#include "config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cook_plan";

#define PLANS_PATH     DATA_LOG_BASE_PATH "/plans.json"
#define PLANS_TMP_PATH DATA_LOG_BASE_PATH "/plans.tmp"
#define RUN_PATH       DATA_LOG_BASE_PATH "/plan_run.json"
#define RUN_TMP_PATH   DATA_LOG_BASE_PATH "/plan_run.tmp"
#define PLANS_FILE_MAX (16 * 1024)
#define LATE_NOTE_S    120   // a step alert more than this late says so

// Used until the user saves their own plans.
static const char DEFAULT_PLANS[] =
    "{\"plans\":[{\"name\":\"3-2-1 ribs\",\"steps\":["
    "{\"name\":\"The Smoke\",\"min\":120,\"action\":\"unwrapped ribs\",\"temp\":100},"
    "{\"name\":\"The wrap\",\"min\":120,\"action\":\"wrapped in foil ribs\",\"temp\":120},"
    "{\"name\":\"The Glaze\",\"min\":45,\"action\":\"unwrapped bark up\",\"temp\":165},"
    "{\"name\":\"The Brush\",\"min\":10,\"action\":\"unwrapped bark up\",\"temp\":165}]}]}";

typedef struct {
    char name[PLAN_NAME_MAX + 1];
    int minutes;
    char action[PLAN_ACTION_MAX + 1];
    int temp;        // 0 = none
} plan_step_t;

// The running plan: a snapshot of the steps taken at Start, so editing the
// saved plans mid-cook doesn't disturb it. Saved to RUN_PATH on changes.
typedef struct {
    bool running;
    char name[PLAN_NAME_MAX + 1];
    plan_step_t steps[PLAN_MAX_STEPS];
    int n_steps;
    uint32_t start_unix;      // 0 = clock wasn't synced at Start (see start_uptime_s)
    int64_t start_uptime_s;   // -1 = started before this boot
    int last_alerted;         // last step whose start alert went out; -1 = none
} plan_run_t;

static plan_run_t s_run;
static SemaphoreHandle_t s_mutex;   // guards s_run and the plan files

// ---------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------
static char *read_file(const char *path, size_t max)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > max) {
        return NULL;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    char *buf = malloc(st.st_size + 1);
    size_t n = buf ? fread(buf, 1, st.st_size, f) : 0;
    fclose(f);
    if (!buf) {
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

// Write to a temp file, then rename over the target, so a power cut
// mid-write leaves the old file intact.
static esp_err_t write_file_atomic(const char *path, const char *tmp, const char *data)
{
    FILE *f = fopen(tmp, "w");
    if (!f) {
        return ESP_FAIL;
    }
    size_t len = strlen(data);
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------
// Parsing / validation
// ---------------------------------------------------------------------
static bool parse_step(const cJSON *js, plan_step_t *st, const char **why)
{
    const cJSON *name = cJSON_GetObjectItem(js, "name");
    const cJSON *min = cJSON_GetObjectItem(js, "min");
    const cJSON *action = cJSON_GetObjectItem(js, "action");
    const cJSON *temp = cJSON_GetObjectItem(js, "temp");
    if (!cJSON_IsString(name) || !name->valuestring[0]) {
        *why = "every step needs a name";
        return false;
    }
    if (strlen(name->valuestring) > PLAN_NAME_MAX) {
        *why = "step names can be at most 40 characters";
        return false;
    }
    if (!cJSON_IsNumber(min) || min->valuedouble < 1 || min->valuedouble > 1440) {
        *why = "step time must be 1 to 1440 minutes";
        return false;
    }
    if (action && !cJSON_IsString(action)) {
        *why = "invalid action";
        return false;
    }
    if (action && strlen(action->valuestring) > PLAN_ACTION_MAX) {
        *why = "actions can be at most 80 characters";
        return false;
    }
    if (temp && !cJSON_IsNull(temp) && (!cJSON_IsNumber(temp) || temp->valuedouble < 0 || temp->valuedouble > 999)) {
        *why = "target temp must be 0 to 999";
        return false;
    }
    strlcpy(st->name, name->valuestring, sizeof(st->name));
    st->minutes = (int)min->valuedouble;
    strlcpy(st->action, action ? action->valuestring : "", sizeof(st->action));
    st->temp = cJSON_IsNumber(temp) ? (int)temp->valuedouble : 0;
    return true;
}

static bool parse_steps(const cJSON *arr, plan_step_t *steps, int *n, const char **why)
{
    int count = cJSON_GetArraySize(arr);
    if (!cJSON_IsArray(arr) || count < 1) {
        *why = "a plan needs at least one step";
        return false;
    }
    if (count > PLAN_MAX_STEPS) {
        *why = "a plan can have at most 12 steps";
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (!parse_step(cJSON_GetArrayItem(arr, i), &steps[i], why)) {
            return false;
        }
    }
    *n = count;
    return true;
}

static cJSON *steps_to_json(const plan_step_t *steps, int n)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *st = cJSON_CreateObject();
        cJSON_AddStringToObject(st, "name", steps[i].name);
        cJSON_AddNumberToObject(st, "min", steps[i].minutes);
        cJSON_AddStringToObject(st, "action", steps[i].action);
        cJSON_AddNumberToObject(st, "temp", steps[i].temp);
        cJSON_AddItemToArray(arr, st);
    }
    return arr;
}

// ---------------------------------------------------------------------
// Run state
// ---------------------------------------------------------------------
// Caller holds s_mutex.
static void save_run_locked(void)
{
    if (!s_run.running) {
        remove(RUN_PATH);
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", s_run.name);
    cJSON_AddItemToObject(root, "steps", steps_to_json(s_run.steps, s_run.n_steps));
    cJSON_AddNumberToObject(root, "start_unix", s_run.start_unix);
    cJSON_AddNumberToObject(root, "last", s_run.last_alerted);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        if (write_file_atomic(RUN_PATH, RUN_TMP_PATH, json) != ESP_OK) {
            ESP_LOGW(TAG, "Could not save running plan");
        }
        free(json);
    }
}

static void load_run(void)
{
    char *json = read_file(RUN_PATH, PLANS_FILE_MAX);
    if (!json) {
        return;
    }
    cJSON *root = cJSON_Parse(json);
    free(json);
    const char *why = NULL;
    const cJSON *name = cJSON_GetObjectItem(root, "name");
    const cJSON *start = cJSON_GetObjectItem(root, "start_unix");
    const cJSON *last = cJSON_GetObjectItem(root, "last");
    plan_run_t r = { .running = true, .start_uptime_s = -1 };
    if (root && cJSON_IsString(name) && cJSON_IsNumber(start) && cJSON_IsNumber(last) &&
        parse_steps(cJSON_GetObjectItem(root, "steps"), r.steps, &r.n_steps, &why)) {
        strlcpy(r.name, name->valuestring, sizeof(r.name));
        r.start_unix = (uint32_t)start->valuedouble;
        r.last_alerted = (int)last->valuedouble;
    } else {
        r.running = false;
    }
    cJSON_Delete(root);

    if (r.running && r.start_unix) {
        s_run = r;
        ESP_LOGI(TAG, "Resuming plan '%s' once the clock syncs", r.name);
    } else {
        // Started while offline (no wall clock): can't tell how long the
        // power was out, so the timing can't be recovered.
        ESP_LOGW(TAG, "Dropping saved plan run: no start time");
        remove(RUN_PATH);
    }
}

static uint32_t unix_now_or_0(void)
{
    time_t now = time(NULL);
    return now > UNIX_TIME_VALID ? (uint32_t)now : 0;
}

static int64_t uptime_s(void)
{
    return esp_timer_get_time() / 1000000;
}

// Seconds since Start, or -1 if unknown (resumed after a reboot and the clock
// hasn't synced yet). Caller holds s_mutex.
static int64_t elapsed_locked(void)
{
    uint32_t now = unix_now_or_0();
    if (s_run.start_unix && now) {
        return (int64_t)now - s_run.start_unix;
    }
    if (s_run.start_uptime_s >= 0) {
        return uptime_s() - s_run.start_uptime_s;
    }
    return -1;
}

// Step index at `elapsed` (n_steps = all done); *step_start_s = when it began.
static int step_at(int64_t elapsed, int64_t *step_start_s)
{
    int64_t cum = 0;
    for (int i = 0; i < s_run.n_steps; i++) {
        int64_t len = (int64_t)s_run.steps[i].minutes * 60;
        if (elapsed < cum + len) {
            *step_start_s = cum;
            return i;
        }
        cum += len;
    }
    *step_start_s = cum;
    return s_run.n_steps;
}

static int64_t total_s(void)
{
    int64_t t = 0;
    for (int i = 0; i < s_run.n_steps; i++) {
        t += (int64_t)s_run.steps[i].minutes * 60;
    }
    return t;
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------
esp_err_t cook_plan_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    if (data_log_available()) {
        load_run();
    }
    return ESP_OK;
}

char *cook_plan_get_plans_json(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    char *json = data_log_available() ? read_file(PLANS_PATH, PLANS_FILE_MAX) : NULL;
    xSemaphoreGive(s_mutex);
    return json ? json : strdup(DEFAULT_PLANS);
}

esp_err_t cook_plan_save_plans(const char *json, const char **why)
{
    if (!data_log_available()) {
        *why = "flash storage not available";
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_Parse(json);
    const cJSON *plans = cJSON_GetObjectItem(root, "plans");
    if (!cJSON_IsArray(plans)) {
        cJSON_Delete(root);
        *why = "invalid plan data";
        return ESP_ERR_INVALID_ARG;
    }
    int n_plans = cJSON_GetArraySize(plans);
    if (n_plans > PLAN_MAX_PLANS) {
        cJSON_Delete(root);
        *why = "at most 10 plans";
        return ESP_ERR_INVALID_ARG;
    }

    // Rebuild from validated values so only clean data gets saved.
    cJSON *out = cJSON_CreateObject();
    cJSON *out_plans = cJSON_AddArrayToObject(out, "plans");
    plan_step_t *steps = malloc(sizeof(plan_step_t) * PLAN_MAX_STEPS);
    esp_err_t err = steps ? ESP_OK : ESP_ERR_NO_MEM;
    for (int p = 0; p < n_plans && err == ESP_OK; p++) {
        const cJSON *plan = cJSON_GetArrayItem(plans, p);
        const cJSON *name = cJSON_GetObjectItem(plan, "name");
        if (!cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring) > PLAN_NAME_MAX) {
            *why = "every plan needs a name (at most 40 characters)";
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        for (int q = 0; q < p; q++) {
            const cJSON *other = cJSON_GetObjectItem(cJSON_GetArrayItem(plans, q), "name");
            if (cJSON_IsString(other) && strcmp(other->valuestring, name->valuestring) == 0) {
                *why = "two plans have the same name";
                err = ESP_ERR_INVALID_ARG;
            }
        }
        int n_steps = 0;
        if (err == ESP_OK && !parse_steps(cJSON_GetObjectItem(plan, "steps"), steps, &n_steps, why)) {
            err = ESP_ERR_INVALID_ARG;
        }
        if (err == ESP_OK) {
            cJSON *op = cJSON_CreateObject();
            cJSON_AddStringToObject(op, "name", name->valuestring);
            cJSON_AddItemToObject(op, "steps", steps_to_json(steps, n_steps));
            cJSON_AddItemToArray(out_plans, op);
        }
    }
    free(steps);
    cJSON_Delete(root);

    char *text = err == ESP_OK ? cJSON_PrintUnformatted(out) : NULL;
    cJSON_Delete(out);
    if (err == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        err = text ? write_file_atomic(PLANS_PATH, PLANS_TMP_PATH, text) : ESP_ERR_NO_MEM;
        xSemaphoreGive(s_mutex);
        if (err != ESP_OK) {
            *why = "could not write to flash";
        }
    }
    free(text);
    return err;
}

esp_err_t cook_plan_start(const char *name, const char **why)
{
    char *json = cook_plan_get_plans_json();
    cJSON *root = cJSON_Parse(json);
    free(json);
    const cJSON *plans = cJSON_GetObjectItem(root, "plans");
    const cJSON *found = NULL;
    for (int p = 0; p < cJSON_GetArraySize(plans) && !found; p++) {
        const cJSON *plan = cJSON_GetArrayItem(plans, p);
        const cJSON *pn = cJSON_GetObjectItem(plan, "name");
        if (cJSON_IsString(pn) && strcmp(pn->valuestring, name) == 0) {
            found = plan;
        }
    }
    plan_run_t r = { .running = true, .last_alerted = -1 };
    if (!found) {
        cJSON_Delete(root);
        *why = "no saved plan with that name (save it first)";
        return ESP_ERR_NOT_FOUND;
    }
    bool ok = parse_steps(cJSON_GetObjectItem(found, "steps"), r.steps, &r.n_steps, why);
    cJSON_Delete(root);
    if (!ok) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(r.name, name, sizeof(r.name));
    r.start_uptime_s = uptime_s();
    r.start_unix = unix_now_or_0();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_run = r;
    save_run_locked();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Started plan '%s' (%d steps)", r.name, r.n_steps);
    cook_plan_tick();   // step 1's alert now
    return ESP_OK;
}

void cook_plan_stop(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_run.running) {
        ESP_LOGI(TAG, "Stopped plan '%s'", s_run.name);
    }
    s_run.running = false;
    save_run_locked();
    xSemaphoreGive(s_mutex);
}

void cook_plan_tick(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_run.running) {
        xSemaphoreGive(s_mutex);
        return;
    }
    // Started before the clock synced: pin the start to wall-clock time now
    // that we can, so a later power cut can resume.
    uint32_t now = unix_now_or_0();
    if (!s_run.start_unix && now && s_run.start_uptime_s >= 0) {
        s_run.start_unix = now - (uint32_t)(uptime_s() - s_run.start_uptime_s);
        save_run_locked();
    }
    int64_t el = elapsed_locked();
    if (el < 0) {
        xSemaphoreGive(s_mutex);   // resumed after reboot, waiting for the clock
        return;
    }

    int64_t step_start = 0;
    int idx = step_at(el, &step_start);
    if (idx >= s_run.n_steps) {
        alerts_notify("%s: all %d steps done", s_run.name, s_run.n_steps);
        s_run.running = false;
        save_run_locked();
        xSemaphoreGive(s_mutex);
        return;
    }
    if (idx > s_run.last_alerted) {
        const plan_step_t *st = &s_run.steps[idx];
        char action[PLAN_ACTION_MAX + 8] = "";
        char temp[24] = "";
        char late[48] = "";
        if (st->action[0]) {
            snprintf(action, sizeof(action), " - %s", st->action);
        }
        if (st->temp > 0) {
            snprintf(temp, sizeof(temp), ". Target %dF", st->temp);
        }
        int64_t late_s = el - step_start;
        if (late_s > LATE_NOTE_S) {
            // e.g. the step began while the power was out
            snprintf(late, sizeof(late), " (step began %lld min ago)", (long long)(late_s / 60));
        }
        alerts_notify("Step %d/%d: %s (%d min)%s%s%s", idx + 1, s_run.n_steps, st->name, st->minutes,
                      action, temp, late);
        s_run.last_alerted = idx;
        save_run_locked();
    }
    xSemaphoreGive(s_mutex);
}

void cook_plan_add_status(cJSON *parent)
{
    cJSON *pj = cJSON_AddObjectToObject(parent, "plan");
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cJSON_AddBoolToObject(pj, "running", s_run.running);
    if (s_run.running) {
        cJSON_AddStringToObject(pj, "name", s_run.name);
        cJSON_AddNumberToObject(pj, "steps", s_run.n_steps);
        int64_t el = elapsed_locked();
        if (el < 0) {
            cJSON_AddBoolToObject(pj, "waiting", true);
        } else {
            int64_t step_start = 0;
            int idx = step_at(el, &step_start);
            if (idx < s_run.n_steps) {
                const plan_step_t *st = &s_run.steps[idx];
                cJSON_AddNumberToObject(pj, "step", idx + 1);
                cJSON_AddStringToObject(pj, "stepName", st->name);
                cJSON_AddStringToObject(pj, "action", st->action);
                cJSON_AddNumberToObject(pj, "temp", st->temp);
                cJSON_AddNumberToObject(pj, "stepLeft", step_start + (int64_t)st->minutes * 60 - el);
            }
            cJSON_AddNumberToObject(pj, "totalLeft", total_s() - el);
        }
    }
    xSemaphoreGive(s_mutex);
}
