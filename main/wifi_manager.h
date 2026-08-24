#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool ap_mode;         // true if the fallback SoftAP is currently active
    bool sta_connected;   // true if the station interface has an IP
    char ssid[33];        // SSID we're connected to (STA) or broadcasting (AP)
    char ip[16];
} wifi_status_t;

// Brings up Wi-Fi: tries any saved station credentials first; if none are
// saved or the connection attempt times out, falls back to a SoftAP so the
// device is always reachable for configuration.
esp_err_t wifi_manager_start(void);

void wifi_manager_get_status(wifi_status_t *out);

// Scans for nearby networks and returns a malloc'd JSON string
// ({"networks":[{"ssid":...,"rssi":...,"secure":...}, ...]}). Caller frees.
esp_err_t wifi_manager_scan_json(char **out_json);

// Persists new station credentials to NVS and attempts to connect.
esp_err_t wifi_manager_save_and_connect(const char *ssid, const char *password);
