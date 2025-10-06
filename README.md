# KinkonyAGFW  
**IR + RF Hub Firmware for Kincony AG Hub with Haptique RS90**

![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)
![License](https://img.shields.io/badge/license-MIT-lightgrey.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-orange.svg)

---

## 📦 Firmware Download & Flash

The latest stable firmware is in [`/bin`](./bin):  
👉 [`Haptique_Kincony_AG_Firmware_V1.bin`](./bin/Haptique_Kincony_AG_Firmware_V1.bin)

**Requirements**
- Kincony AG Hub (ESP32-based)
- **USB-to-Mini-USB** cable (power + flashing; no external UART needed)
- [esptool.py](https://github.com/espressif/esptool) or ESP-IDF installed

**Flash with esptool.py**
```bash
# Replace /dev/ttyUSB0 with your serial port
# macOS example: /dev/cu.usbserial* or /dev/cu.usbmodem*
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x1000 bin/Haptique_Kincony_AG_Firmware_V1.bin
````

**Windows GUI (Kincony Flash Tool)**

1. Download from Kincony: [https://www.kincony.com/esp-module-flash-download-tools.html](https://www.kincony.com/esp-module-flash-download-tools.html)
2. Open the tool and select the firmware file: `bin/Haptique_Kincony_AG_Firmware_V1.bin`
3. Choose your **COM** port → **Start**.

> For cross-platform and automation, prefer **esptool.py**.

---

## 🔄 First Boot / Provisioning

After flashing, the hub reboots and starts an AP:

```
SSID: HAP_IRHUB
Password: 12345678
```

Connect to the AP and provision Wi-Fi using the API below (**2.4 GHz only**).

---

## 💻 Source (Arduino)

* **Folder:** [`/KinkonyAGFW_Arduino`](./KinkonyAGFW_Arduino)
* **Main file:** [`Haptique_AGFW.ino`](./KinkonyAGFW_Arduino/Haptique_AGFW.ino)

**Build with Arduino IDE or PlatformIO**

1. Install **ESP32 Board Support**.
2. Open `KinkonyAGFW_Arduino/Haptique_AGFW.ino`.
3. Board: **ESP32 Dev Module** (or your specific ESP32), select correct **Port**.
4. Click **Upload**.

> 🔎 **Arduino IDE rule:** the sketch folder name must match the `.ino` filename.
> If you open the file directly, Arduino may prompt to place it in `Haptique_AGFW/`. That’s expected.

---

## 🧭 Table of Contents

* [Overview](#overview)
* [Quick Start APIs](#quick-start-apis)
* [IR & RF Behavior](#ir--rf-behavior)
* [OTA Update APIs](#ota-update-apis)
* [WebSocket Messages](#websocket-messages)
* [Troubleshooting](#troubleshooting)
* [Repo Layout](#repo-layout)
* [Docs](#docs)
* [Contributing](#contributing)
* [License](#license)

---

## Overview

**KinkonyAGFW** enables:

* AP+STA Wi-Fi provisioning with mDNS
* HTTP control of **IR send** and **status**
* **IR capture** (A/B + combined)
* **RF 433 MHz send** (RCSwitch)
* **WebSocket** streaming for IR/RF receive
* **OTA updates** via manifest or direct URL (optional auth)

**Default Hostname:** `haptique-extender` → `http://haptique-extender/`

---

## Quick Start APIs

**Base (hostname):** `http://haptique-extender`
**Base (IP):** `http://<device-ip>`

### Save Wi-Fi (2.4 GHz only)

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"ssid":"Home2G","pass":"MyWifiPass"}' \
  http://haptique-extender/api/wifi/save
```

### Status

```bash
curl http://haptique-extender/api/status
```

Returns Wi-Fi status, IPs, hostname, firmware version, and an `ota` object.

### Change Hostname / Instance

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"hostname":"haptique-extender","instance":"Haptique Extender"}' \
  http://haptique-extender/api/hostname
```

> Device reboots after saving.

### IR Send (raw timings)

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"freq_khz":38,"duty":33,"repeat":1,"raw":[9000,4500,560,560,560,560]}' \
  http://haptique-extender/api/ir/send
```

### IR Last Capture (A/B/combined)

```bash
curl http://haptique-extender/api/ir/last
```

> Returns 404 until the first capture is recorded.

### RF 433 MHz Send

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"code":123456,"bits":24,"protocol":1,"repeat":8}' \
  http://haptique-extender/api/rf/send
```

---

## IR & RF Behavior

* **IR TX**: GPIO **2**, carrier via LEDC
* **IR RX**: GPIO **23**, RMT in active-low mode (TSOP)
* **IR capture** uses A/B detection and produces:

  * `a[]`, `b[]`, `combined[]`
  * CSV strings `frameA`, `frameB` for quick copy/paste
* **RF TX**: GPIO **22** (RCSwitch)
* **RF RX**: GPIO **13** (RCSwitch)

**Important:**

* **IR receive** is available via **HTTP** (`/api/ir/last`) **and** **WebSocket** (`ir_rx` messages).
* **RF receive** is **WebSocket-only** (`rf_rx` messages). There is **no** `/api/rf/last`.

---

## OTA Update APIs

Supports **manifest-based** or **direct URL** OTA with optional **Bearer/Basic** auth. TLS verification can be relaxed via config.

* **Status**

  ```bash
  curl http://haptique-extender/api/ota/status
  ```
* **Get Config**

  ```bash
  curl http://haptique-extender/api/ota/config
  ```
* **Set Config**

  ```bash
  curl -X POST -H "Content-Type: application/json" \
    -d '{"manifest_url":"https://example.com/irgw/manifest.json","auto_check":true,"auto_install":false,"interval_min":360,"allow_insecure_tls":true,"auth":{"type":"bearer","bearer":"<token>"}}' \
    http://haptique-extender/api/ota/config
  ```
* **Fetch Manifest & Compare**

  ```bash
  curl http://haptique-extender/api/ota/manifest
  ```
* **Check & Install (if newer)**

  ```bash
  curl -X POST http://haptique-extender/api/ota/check
  ```
* **Install from Direct URL**

  ```bash
  curl -X POST -H "Content-Type: application/json" \
    -d '{"url":"https://example.com/firmware.bin"}' \
    http://haptique-extender/api/ota/url
  ```

**WebSocket progress:** device pushes `ota_progress` and `ota_done`.

---

## WebSocket Messages

**URL:** `ws://haptique-extender:81/`

**Incoming (from device):**

* **IR capture**

  ```json
  {"type":"ir_rx","freq_khz":38,"frames":2,"gap_ms":30,"countA":68,"a":[...],"countB":68,"b":[...],"combined_count":123,"combined":[...],"frameA":"...", "frameB":"..."}
  ```
* **RF capture**

  ```json
  {"type":"rf_rx","code":123456,"bits":24,"protocol":1,"pulselen":350}
  ```
* **OTA**

  ```json
  {"type":"ota_progress","bytes":12345,"total":456789}
  {"type":"ota_done","ok":true,"written":456789}
  ```

**Outgoing (to device):**

* IR send (arrays)

  ```json
  {"type":"ir_send","freq_khz":38,"duty":33,"repeat":1,"raw":[...]}
  {"type":"ir_sendA","a":[...]}
  {"type":"ir_sendB","b":[...]}
  {"type":"ir_sendAB","a":[...],"b":[...],"gap_us":30000}
  ```
* IR send (CSV variants)

  ```json
  {"type":"ir_send_csv","raw_csv":"9000,4500,560,560,..."}
  {"type":"ir_sendA_csv","a_csv":"..."} 
  {"type":"ir_sendB_csv","b_csv":"..."} 
  {"type":"ir_sendAB_csv","a_csv":"...","b_csv":"...","gap_us":30000}
  ```
* IR carrier test

  ```json
  {"type":"ir_test","ms":800}
  ```
* RF send

  ```json
  {"type":"rf_send","code":123456,"bits":24,"protocol":1,"pulselen":0,"repeat":8}
  ```

---

## Troubleshooting

* Use **AP `HAP_IRHUB`** only for provisioning; once on STA, call APIs on the LAN IP or `http://haptique-extender`.
* If hostname doesn’t resolve:

  * Check your router **DHCP** client list for the device IP, or
  * `curl http://<ip>/api/status`
* For POST requests, always set `Content-Type: application/json`.
* Only **2.4 GHz** SSIDs are supported. Provisioning checks and enforces this.
* Security: the setup AP uses a simple password. Place the device on a trusted network; consider firewalling access.
* OTA: If strict TLS validation is required, disable `allow_insecure_tls` and provide a proper CA (future enhancement).

---

## Repo Layout

```
/
├─ bin/
│  └─ Haptique_Kincony_AG_Firmware_V1.bin
├─ KinkonyAGFW_Arduino/
│  └─ Haptique_AGFW.ino
├─ docs/
│  ├─ SetupGuide.md
│  └─ API_Reference.md
├─ LICENSE
└─ README.md
```

---

## Docs

* **Setup Guide:** [`/docs/SetupGuide.md`](./docs/SetupGuide.md)
* **API Reference:** [`/docs/API_Reference.md`](./docs/API_Reference.md)

---

## Contributing

PRs welcome! Open an issue, fork, and submit a pull request.

---

## License

This project is licensed under the **MIT License** — see [`LICENSE`](./LICENSE).

---

© 2025 Cantata Communication Solutions · *Haptique RS90 × Kincony AG Hub*

```
::contentReference[oaicite:0]{index=0}
```
