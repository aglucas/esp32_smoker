#pragma once

#include "esp_err.h"

// Starts the HTTP server and registers the dashboard + JSON API routes.
esp_err_t web_server_start(void);
