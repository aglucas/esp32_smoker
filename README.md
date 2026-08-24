# esp32_smoker

ESP32 firmware project for a smoker controller, built on [ESP-IDF v5.5.3](https://github.com/espressif/esp-idf/releases/tag/v5.5.3). Targets the original ESP32 (classic).

## Features

- Wi-Fi station mode using credentials saved in NVS; if none are saved or the connection times out, falls back to a SoftAP (`ESP32-Smoker-XXXX`, password `smoker1234` by default, see [main/config.h](main/config.h)) so the device is always reachable.
- Single-page web dashboard (`GET /`) showing live probe temperatures, a chart of recent history, and, while in AP fallback mode, a list of scanned networks with a form to connect.
- JSON API: `GET /api/status`, `GET /api/history`, `GET /api/scan`, `POST /api/wifi` (`{"ssid":"...","password":"..."}`).
- Two analog thermistor inputs read via ADC1 (probe A = meat, probe B = grill), oversampled and converted to °C with the Beta equation.
- A reading is snapshotted every 60s into an in-RAM ring buffer (feeds the web chart) and appended as a CSV row to an SD card over SPI.

## Wiring

| Signal | GPIO | Notes |
|---|---|---|
| Probe A (meat) | GPIO34 | ADC1_CH6, voltage divider: 3V3 → thermistor → GPIO34 → 100k → GND |
| Probe B (grill) | GPIO35 | ADC1_CH7, same divider topology |
| SD MOSI | GPIO23 | VSPI default |
| SD MISO | GPIO19 | VSPI default |
| SD CLK | GPIO18 | VSPI default |
| SD CS | GPIO5 | |

Probe defaults assume a generic 100k NTC thermistor (Beta 3950). Recalibrate `THERMISTOR_*` and `SD_PIN_*` / `PROBE_*_ADC_CHANNEL` in [main/config.h](main/config.h) for your actual probe hardware and wiring.

## Prerequisites

- ESP-IDF v5.5.3 installed and set up (`install.bat` / `export.bat` from the IDF repo, or the ESP-IDF VS Code extension).

## Build

```
idf.py set-target esp32
idf.py build
```

## Flash & Monitor

```
idf.py -p <PORT> flash monitor
```
