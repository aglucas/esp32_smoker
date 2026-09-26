#pragma once

// =====================================================================
//                            USER CONFIG
// =====================================================================

// ---- Wi-Fi ----
#define WIFI_CONNECT_TIMEOUT_MS   15000
#define WIFI_MAX_RETRY            5

// Default network to try before saved credentials / fallback AP. Set it in
// main/secrets.h (copy secrets.h.example); that file is git-ignored so the
// password stays out of the repo. Without it, the default network is skipped.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef DEFAULT_STA_SSID
#define DEFAULT_STA_SSID          ""
#define DEFAULT_STA_PASSWORD      ""
#endif

#define AP_SSID_PREFIX            "ESP32-Smoker-"
#define AP_PASSWORD               "smoker1234"   // WPA2-PSK, >=8 chars. Change me.
#define AP_CHANNEL                1
#define AP_MAX_CONN               4

#define MDNS_HOSTNAME             "smoker"       // -> http://smoker.local

// ---- Alert delivery (ntfy push notifications) ----
// Alerts need internet, so nothing is sent while only the setup AP is up.
#define USE_NTFY                  1        // default only; on/off is set on the web page

// ntfy: install the ntfy app on your phone, subscribe to the topic.
// Anyone who knows the topic name can read it, so make it unguessable.
// These are first-boot defaults: the server, topic and on/off switch can be
// changed on the web page ("Change" next to Notifications) and are saved in NVS.
#define NTFY_SERVER               "https://ntfy.sh"
#define NTFY_TOPIC                "smoker-change-me-7q2x9k"

// ---- Temperature probes (Seeed XIAO ESP32C3) ----
// Only ADC1 is usable: on the C3, ADC2 is unreliable and blocked by ESP-IDF.
// The XIAO breaks out three ADC1 pins: A0 (GPIO2), A1 (GPIO3), A2 (GPIO4).
// On the C3, ADC1 channel N is GPION.
//
// Divider per channel: 3V3 --[rRef]-- ADC pin --[thermistor]-- GND
//
// Measure the 3V3 pin with a multimeter and enter it here (in mV).
#define VSUPPLY_MV                3300.0f
// Readings at/above this are treated as "no probe plugged in". The C3 ADC is
// only accurate up to ~2.5 V and saturates a little above that, so unplug a
// probe, watch the serial log to see what your board reads, then set this
// ~50 mV below that value.
#define OPEN_MV                   2600.0f
#define SHORT_MV                  20.0f
#define ADC_SAMPLE_COUNT          64       // oversampling per reading

// Probe 0 is always the pit; the rest are meat probes.
// Beta model is used while shC == 0; set shA/shB/shC to use Steinhart-Hart.
//
// All three probes are Maverick ET-72/73 style (~200k at room temp) on 100k
// divider resistors, using HeaterMeter's Steinhart-Hart coefficients for that
// probe. On the meat probes these matched room temp, a 180 F oven and boiling
// water (~202 F at 5,400 ft) within ~1.5 F.
//   Pit:  225 F ~440 mV (~5.5 mV/F), 350 F ~110 mV (~0.8 mV/F).
//   Meat: 165 F ~1.0 V, 203 F ~600 mV.
// Below ~49 F the voltage passes OPEN_MV (the C3's ~2.5 V ceiling), so a
// colder probe can't be told apart from an unplugged one; the web page and
// log show "below 49 or unplugged".
//
// GPIO2 (A0) is a boot strapping pin. An unplugged pit probe holds it at
// 3.3 V; a room-temp probe at ~2.2 V and a hot one much lower. If the board
// ever fails to boot with the pit probe plugged in, unplug it at power-up.
#define MAVERICK_SH               2.4723753e-4, 2.3402251e-4, 1.3879768e-7
#define NUM_PROBES                3
#define PROBE_TABLE { \
    /* name      adc channel    rRef       r0         t0C    beta     shA, shB, shC */ \
    { "Pit",     ADC_CHANNEL_2, 100000.0f, 200000.0f, 25.0f, 3950.0f, MAVERICK_SH }, /* A0 / GPIO2 */ \
    { "Meat 1",  ADC_CHANNEL_3, 100000.0f, 200000.0f, 25.0f, 3950.0f, MAVERICK_SH }, /* A1 / GPIO3 */ \
    { "Meat 2",  ADC_CHANNEL_4, 100000.0f, 200000.0f, 25.0f, 3950.0f, MAVERICK_SH }, /* A2 / GPIO4 */ \
}

// ---- Alert defaults (changeable from the web page, saved in NVS) ----
#define DEFAULT_PIT_LOW_F         200.0f   // 0 = off
#define DEFAULT_PIT_HIGH_F        300.0f   // 0 = off
// Index 0 is the pit target (drawn on the graph only, no alert); 0 = off.
#define DEFAULT_TARGETS_F         { 225, 203, 165 }

#define DEFAULT_PIT_LOW_DELAY_S   60       // pit must stay low this long before alerting (set on the web page)
#define PIT_LOW_DELAY_MAX_S       3600
#define PIT_HYST_F                10.0f    // pit high re-arms once this far below the limit
// After a reboot, pit low stays armed only if the board was last running
// (armed) within this long - a power blip mid-cook, not tomorrow's cold start.
#define PIT_ARM_RESTORE_MIN       120
#define ALERT_RETRY_SEC           30       // retry a failed ntfy send this often
#define ALERT_MAX_AGE_MIN         120      // give up on alerts undelivered this long
#define NEAR_DONE_F               5.0f     // "almost done" heads-up (0 = off)
#define MEAT_REARM_F              15.0f    // re-arm meat alert if temp drops this far

// ---- Timing / history ----
#define READ_MS                   2000     // read probes & check alerts
#define SAMPLE_SEC                30       // web graph resolution
#define HISTORY_HOURS             12       // web graph memory (RAM)
#define LOG_INTERVAL_MS           60000    // flash CSV log cadence

// Not a setting: time(NULL) above this means SNTP has set the clock. Before
// that (and always on the offline setup AP) only time-since-boot is known.
#define UNIX_TIME_VALID           1700000000

// ---- Temperature log (LittleFS "storage" partition in internal flash) ----
// Rows go to log.csv; when it passes LOG_ROTATE_BYTES it becomes log_old.csv
// (replacing the previous one) and a new log.csv starts, so the log never
// uses more than ~2x this. ~40 bytes/row at 1 row/min is ~60 KB per day, so
// 1 MB per file keeps roughly the last 2-4 weeks of cooking.
#define LOG_ROTATE_BYTES          (1024 * 1024)
// POSIX TZ string for the local_time column (America/Denver).
#define LOG_TIMEZONE              "MST7MDT,M3.2.0,M11.1.0"
