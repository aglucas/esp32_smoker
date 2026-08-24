#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Mounts the SD card over SPI. Returns an error if no card is present, but
// this is non-fatal - the rest of the app keeps running without logging.
esp_err_t sd_logger_init(void);

bool sd_logger_is_available(void);

// Appends one CSV row (uptime_s,unix_time,meat_c,grill_c) to the log file.
esp_err_t sd_logger_log(uint32_t uptime_s, int64_t unix_time, float meat_c, float grill_c);
