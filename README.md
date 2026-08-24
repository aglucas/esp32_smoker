# esp32_smoker

ESP32 firmware project for a smoker controller, built on [ESP-IDF v5.5.3](https://github.com/espressif/esp-idf/releases/tag/v5.5.3).

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
