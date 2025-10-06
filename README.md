# KinkonyAGFW  
**IR + RF Hub Firmware for Kincony AG Hub with Haptique RS90**

![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)
![License](https://img.shields.io/badge/license-MIT-lightgrey.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-orange.svg)

---

## 🔎 Quick Links
- 📥 **Firmware**: [`bin/Haptique_Kincony_AG_Firmware_V1.bin`](./bin/Haptique_Kincony_AG_Firmware_V1.bin)  
- 🧩 **Source (Arduino)**: [`KinkonyAGFW_Arduino/Haptique_AGFW.ino`](./KinkonyAGFW_Arduino/Haptique_AGFW.ino)  
- 📚 **Docs**: [Setup Guide](./docs/SetupGuide.md) · [API Reference](./docs/API_Reference.md)  
- 🧪 **Tools**: see [`/tools`](./tools) (Python test scripts)

---

## 🗂 Project Skeleton

```

/
├─ bin/
│  └─ Haptique_Kincony_AG_Firmware_V1.bin
├─ KinkonyAGFW_Arduino/
│  └─ Haptique_AGFW.ino
├─ docs/
│  ├─ SetupGuide.md
│  └─ API_Reference.md
├─ tools/
│  ├─ wifi_config.py       # POST /api/wifi/save
│  ├─ status_check.py      # GET  /api/status
│  ├─ ir_send.py           # POST /api/ir/send
│  ├─ ir_capture.py        # GET  /api/ir/last
│  └─ rf_send.py           # POST /api/rf/send
├─ LICENSE
└─ README.md

````

---

## 📦 Firmware Download & Flash

The latest stable firmware is in [`/bin`](./bin):  
👉 [`Haptique_Kincony_AG_Firmware_V1.bin`](./bin/Haptique_Kincony_AG_Firmware_V1.bin)

### Requirements
- Kincony AG Hub (ESP32-based)
- **USB-to-Mini-USB** cable (for power + flashing; no external UART needed)
- [esptool.py](https://github.com/espressif/esptool) or ESP-IDF installed

### Flash with esptool.py
```bash
# Replace /dev/ttyUSB0 with your serial port
# macOS: /dev/cu.usbserial* or /dev/cu.usbmodem*
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x1000 bin/Haptique_Kincony_AG_Firmware_V1.bin
````

### Windows GUI (Kincony Flash Tool)

1. Download: [https://www.kincony.com/esp-module-flash-download-tools.html](https://www.kincony.com/esp-module-flash-download-tools.html)
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

Provision Wi-Fi via the API (2.4 GHz only). Default hostname: **haptique-extender** (mDNS).

---

## 💻 Source (Arduino)

* **Folder:** [`/KinkonyAGFW_Arduino`](./KinkonyAGFW_Arduino)
* **Main file:** [`Haptique_AGFW.ino`](./KinkonyAGFW_Arduino/Haptique_AGFW.ino)

### Build with Arduino IDE or PlatformIO

1. Install **ESP32 Board Support**.
2. Open `KinkonyAGFW_Arduino/Haptique_AGFW.ino`.
3. Board: **ESP32 Dev Module** (or your ESP32 variant); set correct **Port**.
4. Click **Upload**.

> 🔎 **Arduino rule:** the sketch folder must match the `.ino` filename.
> If prompted, allow Arduino to place it in `Haptique_AGFW/`.

---

## 🧪 Developer Test Tools (Python)

Simple scripts live in [`/tools`](./tools) to exercise the device APIs.

### Prerequisites

```bash
python3 -m pip install --upgrade pip
pip install requests
```

> Replace `--host` with either your device IP (e.g., `192.168.1.100`) or the hostname `haptique-extender.local` (when mDNS works).

### 1) Provision Wi-Fi

```bash
python tools/wifi_config.py --host http://haptique-extender.local \
  --ssid "Your2GSSID" --pass "YourPassword"
# POST /api/wifi/save
```

### 2) Check Status / IP

```bash
python tools/status_check.py --host http://haptique-extender.local
# GET /api/status
```

### 3) Send IR (raw durations)

```bash
python tools/ir_send.py --host http://<device-ip> \
  --raw 9000,4500,560,560,560,560 --freq-khz 38 --duty 33 --repeat 1
# POST /api/ir/send
```

### 4) Read Last Captured IR

```bash
python tools/ir_capture.py --host http://<device-ip>
# GET /api/ir/last
```

### 5) Send RF 433 MHz code

```bash
python tools/rf_send.py --host http://<device-ip> \
  --code 123456 --bits 24 --protocol 1 --repeat 8
# POST /api/rf/send
```

> These scripts only require `requests` and map 1:1 to the firmware endpoints implemented in the sketch.

---
## 🗓️ Changelog

See the full [CHANGELOG.md](./CHANGELOG.md) for release history.


## ✨ Features

* AP + STA Wi-Fi provisioning with mDNS
* HTTP APIs for IR send, capture, and status
* RF 433 MHz send support
* OTA update system (manifest or direct URL)
* WebSocket live events for IR & RF receive
* Built for **Kincony AG Hub** & **Haptique RS90** integration

---

## 📚 Documentation

We’ve moved detailed instructions to `/docs`:

* [Setup Guide](./docs/SetupGuide.md)
* [API Reference](./docs/API_Reference.md)

---

## 🤝 Contributing

Pull requests and improvements are welcome.
Fork this repo, make changes, and open a PR.

---

## 📜 License

This project is licensed under the **MIT License** — see the [LICENSE](./LICENSE) file.

---

© 2025 Cantata Communication Solutions · *Haptique RS90 × Kincony AG Hub*
