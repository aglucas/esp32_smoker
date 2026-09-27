# esp32_smoker

ESP32 firmware project for a smoker controller, built on [ESP-IDF v5.5.3](https://github.com/espressif/esp-idf/releases/tag/v5.5.3). Targets the [Seeed Studio XIAO ESP32C3](https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/).

## Features

- 3 NTC thermistor probes (probe 0 = pit, 1..2 = meat), read in °F. Beta model by default, or per-probe Steinhart-Hart coefficients once calibrated. Unplugged/shorted probes show as `--`.
- Phone-friendly web dashboard at `http://smoker.local` (mDNS) or the device IP: live temps, pit low/high alarm, pit target and per-probe meat targets, and a graph (pit target drawn as a green line, low/high as red lines) of the last 1/3/6/12 hours with touch/hover readout. Fully self-contained (no CDN), so it also works on the offline setup AP.
- Alerts: pit too low (only after it stays low for the "Low alarm delay", 60 s by default and set on the pit card, so opening the lid doesn't trigger it, and only armed once the pit first reaches temp), pit too high, pit probe lost, meat "almost done", meat done. Delivered by [ntfy.sh](https://ntfy.sh) push notifications (set up from the dashboard). Settings are saved in NVS.
- Wi-Fi: tries `DEFAULT_STA_SSID`, then credentials saved in NVS; if neither connects it falls back to a SoftAP (`ESP32-Smoker-XXXX`, password `smoker1234` by default) and the dashboard shows a network picker. Alerts need internet, so none are sent in AP mode.
- Graph history: one sample every 30 s for 12 h, kept in RAM. After a power cut, once the clock syncs over the internet (a few seconds after Wi-Fi connects), the graph reloads the last 12 h from the flash log at 1-minute resolution. The outage shows as a break in the lines. On the offline setup AP the clock never syncs, so the graph starts empty (the flash log still has everything).
- Flash log: one CSV row per minute (`uptime_s,unix_time,local_time,<probe>_f...`) saved to a LittleFS partition in the ESP32's internal flash, so no SD card is needed. Download it from the dashboard's **Data log** panel. **Start new recording** (next to it) deletes the log, clears the graph, and re-arms alerts and the meat target-hit checks; settings are kept. `unix_time` and `local_time` (Mountain Time, set by `LOG_TIMEZONE`) are blank until SNTP syncs. When the active file passes 1 MB it is rotated, and the oldest data is dropped, so the log keeps roughly the last 2-4 weeks of cooking.
- Cook steps: named, timed plans (for example "3-2-1 ribs") edited in the **Cook steps** panel. Each step has a name, minutes, an action and a target temp. Plans are saved on the LittleFS partition (`plans.json`); up to 10 plans with up to 12 steps each. **Start** sends step 1's alert right away, then each next step's alert when the previous step's time is up (for example "Step 2/4: The wrap (120 min) - wrapped in foil ribs. Target 120F"), and a final "all steps done" alert. The target temp is only shown in the alert. The running plan is saved too, so after a power cut it resumes once the clock syncs, and a step that began during the outage is sent late with a note.
- Fan control: a 12 V PC fan on GPIO6 (D4) through a 2N2222, driven by slow (50 Hz) PWM, which lets a 2/3-wire PC fan run steadily at lower speeds than fast PWM. **Auto** runs a PID that holds the pit target (starting with Kp = 4 %/°F, Ki = Kd = 0); **Manual** holds a fixed %; **Off**. In Auto the fan is off when no pit target is set or the pit probe has no reading. If the pit reaches the high alarm, the fan turns off and stays off until the pit has cooled back down to the target. The PID output (0-100 %) drives the fan directly; with slow PWM the fan runs steadily down to about 5 %, and any non-zero output below that is raised to 5 % (`FAN_MIN_PCT`). A 1 s full-speed kick runs when the fan starts from stopped. **Fan alert** (Auto mode): if the fan stays at or above a set % for a set time (90 % for 15 min by default, both set in the Fan panel, 0 = off), one alert is sent to say the fire may be low on fuel or the vents may be wrong; it re-arms once the fan drops below that %. The fan % is plotted on its own graph (0-100 %, half the height) under the temperature graph, sharing its time axis, and logged in the CSV (`fan_pct`).
- JSON API: `GET /api/now`, `GET /api/history`, `POST /api/settings` (form-encoded `alertsOn`, `pitLow`, `pitHigh`, `pitTarget`, `pitLowDelay`, `t1`, `t2`), `POST /api/test`, `GET /api/log.csv`, `POST /api/reset`, `POST /api/hit/clear` (`{"probe":1}`, clears a meat target-hit check), `POST /api/fan` (`{"mode":"auto","manual":50,"kp":4,"ki":0,"kd":0,"alertPct":90,"alertMin":15}`), `POST /api/ntfy` (`{"enabled":true,"server":"https://ntfy.sh","topic":"..."}`), `GET/POST /api/plans`, `POST /api/plan/start` (`{"name":"..."}`), `POST /api/plan/stop`, `GET /api/scan`, `POST /api/wifi` (`{"ssid":"...","password":"..."}`).

## Wiring

Each probe is a voltage divider: `3V3 → rRef → pin → thermistor → GND`. Probes must be on ADC1. The XIAO exposes only three ADC1 pins (A0-A2); A3 is on ADC2, which is unreliable on the C3 and blocked by ESP-IDF.

| Signal | XIAO pin | GPIO | Notes |
|---|---|---|---|
| Pit probe | A0 | GPIO2 | ADC1_CH2, rRef 100k (see strapping note) |
| Meat 1 | A1 | GPIO3 | ADC1_CH3, rRef 100k |
| Meat 2 | A2 | GPIO4 | ADC1_CH4, rRef 100k |
| Fan | D4 | GPIO6 | PWM to the 2N2222 base (see fan note) |

- **Probes and resistor values:** all three probes are Maverick ET-72/73 style (about 200 kΩ at room temperature) on 100k divider resistors, using HeaterMeter's Steinhart-Hart coefficients for that probe (`MAVERICK_SH` in [main/config.h](main/config.h)). They were verified against a 180 °F oven and boiling water (about 202 °F at 5,400 ft). The C3's ADC is only accurate up to about 2.5 V (the classic ESP32 reaches about 3.1 V), so below about 49 °F a probe can't be told apart from an unplugged one, and the dashboard shows "below 49° or unplugged".
- **Strapping pin:** GPIO2 (A0) is sampled at reset to choose the boot mode. The pit probe on A0 holds it at 3.3 V when unplugged and about 2.2 V at room temperature. If the board ever fails to boot with the pit probe plugged in, unplug it during power-up.
- **Fan:** GPIO6 → ~220 Ω → 2N2222 base; emitter to GND; collector to the fan's black (−) wire; fan red (+) to 12 V. The 12 V supply's ground must be connected to the XIAO's GND. Add a diode (1N4148 or 1N5819) across the fan, cathode to +12 V, to absorb switching spikes, and a 10 kΩ resistor from base to GND so the fan stays off while the board boots. Around 220 Ω gives the base roughly 10 mA, enough to fully switch a typical 0.1-0.3 A PC fan (1 kΩ may leave the transistor only part-on and hot).
- **Antenna:** the XIAO ESP32C3 needs its external antenna attached for usable Wi-Fi range.

Probe names, pins, resistor values and thermistor constants are in `PROBE_TABLE` in [main/config.h](main/config.h). For accuracy, measure the 3V3 rail and set `VSUPPLY_MV`, and set `OPEN_MV` about 50 mV below what an unplugged probe reads. The serial log prints mV and ohms for every probe every 2 s, which you can use for calibration.

## Alerts setup

- **ntfy:** install the ntfy app. On the dashboard, tap **Change** next to *Notifications* to turn ntfy on or off and set the server and topic. **Random** generates a hard-to-guess topic, and the panel shows a link to subscribe to it. The settings are saved in NVS; `NTFY_SERVER`, `NTFY_TOPIC` and `USE_NTFY` in [main/config.h](main/config.h) are only first-boot defaults.

Use **Send test alert** on the dashboard to check delivery.

- **Offline delivery:** alerts raised while Wi-Fi or the internet is down are queued and sent once it's back, marked "(sent N min late)". Failed sends are retried every 30 s. Alerts undelivered after 2 hours are dropped, and a power cut loses the queue.
- **Pit low after a reboot:** the pit low alarm stays armed after a reboot if the board was running within the last 2 hours (`PIT_ARM_RESTORE_MIN`), so a power blip mid-cook doesn't disarm it. A cold start the next day begins disarmed, as usual.

## Prerequisites

- ESP-IDF v5.5.3 installed and set up (`install.bat` / `export.bat` from the IDF repo, or the ESP-IDF VS Code extension).

## Build

```
idf.py set-target esp32c3
idf.py build
```

To have the smoker join your Wi-Fi without using the setup page, copy `main/secrets.h.example` to `main/secrets.h` and fill in your network name and password. `secrets.h` is git-ignored, so the password never gets committed. Without it, the firmware still builds and you pick a network from the dashboard instead.

## Flash & Monitor

```
idf.py -p <PORT> flash monitor
```

`flash` writes the bootloader, the partition table ([partitions.csv](partitions.csv)) and the app together. NVS, which holds Wi-Fi credentials and alert settings, keeps the same place in flash, so those survive a firmware update. The log partition is formatted automatically on first boot.

The XIAO's USB-C port is the C3's built-in USB serial port, and the log console is set to use it. If flashing can't connect, hold **BOOT**, tap **RESET**, then release **BOOT** to enter download mode.
