#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

// Timed cook plans: named lists of steps (name, minutes, action, target temp).
// Plans are saved on the LittleFS partition. Starting a plan sends an alert
// at the start of each step; the running plan survives a power cut (resumes
// once the clock syncs).

#define PLAN_MAX_PLANS    10
#define PLAN_MAX_STEPS    12
#define PLAN_NAME_MAX     40
#define PLAN_ACTION_MAX   80

// Call after data_log_init() (needs the LittleFS mount).
esp_err_t cook_plan_init(void);

// The saved plans as JSON: {"plans":[{"name":..,"steps":[{"name":..,"min":..,
// "action":..,"temp":..}]}]}. Malloc'd; caller frees.
char *cook_plan_get_plans_json(void);

// Validates and saves the full plan list (same JSON shape). On
// ESP_ERR_INVALID_ARG, *why says what's wrong.
esp_err_t cook_plan_save_plans(const char *json, const char **why);

// Starts the saved plan with this name (replacing any running plan) and
// sends step 1's alert.
esp_err_t cook_plan_start(const char *name, const char **why);

void cook_plan_stop(void);

// Sends step alerts when steps begin. Call every READ_MS.
void cook_plan_tick(void);

// Adds the run status as "plan" to a /api/now response.
void cook_plan_add_status(cJSON *parent);
