#pragma once

// ---- Wi-Fi ----
#define WIFI_CONNECT_TIMEOUT_MS   15000
#define WIFI_MAX_RETRY            5
#define AP_SSID_PREFIX            "ESP32-Smoker-"
#define AP_PASSWORD               "smoker1234"   // WPA2-PSK, >=8 chars. Change me.
#define AP_CHANNEL                1
#define AP_MAX_CONN               4

// ---- Temperature probes (ADC1, classic ESP32 pins) ----
#define PROBE_A_ADC_CHANNEL       ADC_CHANNEL_6   // GPIO34 - meat probe
#define PROBE_B_ADC_CHANNEL       ADC_CHANNEL_7   // GPIO35 - grill probe
#define ADC_SAMPLE_COUNT          16              // oversampling per reading

// Thermistor voltage-divider: 3V3 --[Thermistor]-- ADC_PIN --[SERIES_R]-- GND
// Defaults assume a generic 100k NTC probe. Recalibrate SERIES_R/R25/BETA
// against your actual probe's datasheet for accurate readings.
#define THERMISTOR_SERIES_R_OHMS  100000.0f
#define THERMISTOR_R25_OHMS       100000.0f
#define THERMISTOR_BETA           3950.0f
#define THERMISTOR_T25_KELVIN     298.15f

// ---- Sampling / logging ----
#define SAMPLE_INTERVAL_MS        2000     // live-reading update cadence (web display)
#define LOG_INTERVAL_MS           60000    // 1 minute - SD card + chart history cadence
#define HISTORY_POINTS            720      // 12h of 1-min points kept in RAM for the web chart

// ---- SD card (SPI mode, VSPI default pins) ----
#define SD_PIN_MISO               19
#define SD_PIN_MOSI               23
#define SD_PIN_CLK                18
#define SD_PIN_CS                 5
#define SD_MOUNT_POINT            "/sdcard"
#define SD_LOG_FILENAME           SD_MOUNT_POINT "/smoker_log.csv"
