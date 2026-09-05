# Sibyl — Setup Guide

---

## Overview

An ESP32 reads a heartbeat sensor and sends the values to TouchDesigner via OSC. TouchDesigner uses the values to compute a 3D shape, renders it into a bitmap, and sends it back to the ESP32 over TCP, which hands it off to the thermal printer for printing.

---

## External Connections (Back of the Device)

| Wire | Purpose |
|---|---|
| 5V power | LED inside the device |
| 7.5V power | Thermal printer |
| USB-C → computer | Powers the small screen |
| HDMI → computer | Video signal for the small screen |
| ESP32 → computer (USB) | Flashing / power / serial port |

Before powering on, confirm all five of these are connected, then turn on the computer and run TouchDesigner.

---

## Arduino Firmware

**Required libraries:**
- WiFi / WiFiUdp (built-in)
- OSCMessage
- Wire (built-in)
- MAX30105 + heartRate.h (SparkFun MAX3010x)
- Adafruit GFX
- Adafruit SSD1306
- Adafruit DRV2605

**Board:** ESP32 Dev Module

**Things to change before flashing:**
```cpp
const char* ssid = "...";       // On-site WiFi
const char* password = "...";   // On-site WiFi password
const char* tdIP = "...";       // IP of the computer running TouchDesigner
```
The WiFi and IP depend on the on-site environment at the time — there's no fixed value, fill in whatever network is being used.

---

## How It Runs

**State machine (controls the OLED screen):**

IDLE (no finger) → READING (15 seconds) → READY (waiting for button press, returns to IDLE after 7 seconds if not pressed) → PRINTING (waiting for data) → COLLECT (5 seconds) → IDLE

**OSC from ESP32 → TouchDesigner:**

| Address | Description |
|---|---|
| `/bpm` | Median heart rate |
| `/finger` | Whether a finger is on the sensor |
| `/ir` | Raw IR value |
| `/button` | Button pressed |
| `/printdone` | Printing finished |
| `/beat` | Each time a heartbeat is detected |
| `/reset` | READY timed out without a button press |

**Printing (TCP, port 8888):**

TD connects to the ESP32's IP:8888 → sends a 4-byte height → sends a bitmap of height × 48 bytes → the ESP32 converts it into ESC/POS commands and feeds them to the printer. The image width is fixed at 384px (48 bytes/row).

---

## TouchDesigner

Set **window3** in the UI to output to the small screen on the device.

---

## Updating the News Entries

`build.py`, `qr/`, `index.html`, and `manifest.json` all live on GitHub (the `marsno3/d` repo).
Don't edit news directly in TD or on the ESP32 — instead follow this process:

1. Add/edit an entry in the entries list in `build.py` (date, title, context, source, url)
2. Run `build.py`, which regenerates:
   - `index.html` (for GitHub Pages)
   - the `qr/` folder (one PNG per news item)
   - `manifest.json`
3. `git add index.html && git commit -m "update news" && git push`
   → Only after this step does the GitHub Pages site update, so the page the QR code links to matches
4. Copy the entire new `qr/` folder into the TD project folder, overwriting the old one
5. Paste the new `manifest.json` content into the `manifest` Text DAT in TD
6. Print a new one and scan the QR code with your phone to confirm it links to the correct news item

The order matters, especially step 3 — if you only do steps 4 and 5 without pushing, the site content the QR code links to won't match the printed news.
