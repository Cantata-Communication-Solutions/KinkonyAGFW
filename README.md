# KinkonyAGFW  
**IR + RF Hub Firmware for Kinkony AG Hub with Haptique RS90**

![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)
![License](https://img.shields.io/badge/license-MIT-lightgrey.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-orange.svg)

---

### 📦 Firmware Download & Flash Instructions

The latest stable firmware is available in the [`/bin`](./bin) directory:  
👉 [Haptique_Kincony_AG_Firmware_V1.bin](./bin/Haptique_Kincony_AG_Firmware_V1.bin)

---

#### 🧰 Requirements
- **Kincony AG Hub** (or any compatible **ESP32**-based IR + RF hub)  
- **USB-to-Mini-USB cable** (for power and flashing)  
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) or [esptool.py](https://github.com/espressif/esptool) installed  

> 💡 **Note:** No external UART adapter is required — the Kincony AG Hub supports native USB flashing via its mini-USB port.


#### ⚙️ Flashing via esptool.py

1. Connect your Kincony AG Hub to your computer via USB.  
2. Open a terminal and navigate to the directory containing the firmware.  
3. Run the following command (replace `/dev/ttyUSB0` with your serial port):

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600   write_flash 0x1000 bin/Haptique_Kincony_AG_Firmware_V1.bin
```
#### 🪟 Alternative Flash Method (Kincony Flash Tool)

If you prefer a graphical flashing tool on Windows, you can use the official **Kincony Flash Tool**:

1. Download the latest version from [Kincony’s official site](https://www.kincony.com/esp-module-flash-download-tools.html)  
2. Open the tool and select the firmware file: `bin/Haptique_Kincony_AG_Firmware_V1.bin`
3. Choose your COM port and click **Start** to flash.

> ⚙️ **Note:**  
> This tool provides a user-friendly interface but is **Windows-only**.  
> For full cross-platform compatibility and automation, use [esptool.py](https://github.com/espressif/esptool).
  


> 💡 **Tip:**  
> You can also use `idf.py flash` if you have the ESP-IDF environment configured.

---

#### 🔄 Reset the Device
After flashing, restart your Kincony AG Hub — it will boot into the new firmware.  
By default, it will broadcast the setup Wi-Fi network:

```
SSID: HAP_IRHUB
Password: 12345678
```

You can then follow the [developer instructions](#haptique-extender--developer-instructions) below to configure Wi-Fi and APIs.

---

### 💻 Building from Source (Arduino Version)

This repository also includes the **open-source Arduino code** for developers who want to build or modify the firmware.

**Source location:** [`/KinkonyAGFW_Arduino`](./KinkonyAGFW_Arduino)  
**Main file:** [Haptique_Kincony_RF_RI_code.ino](./KinkonyAGFW_Arduino/Haptique_Kincony_RF_RI_code.ino)

#### 🧩 Build Steps

1. Install [Arduino IDE](https://www.arduino.cc/en/software) or [PlatformIO](https://platformio.org/).  
2. Install **ESP32 Board Support** via Arduino Boards Manager.  
3. Open `KinkonyAGFW_Arduino/Haptique_Kincony_RF_RI_code.ino` in Arduino IDE.  
4. Under **Tools → Board**, select `ESP32 Dev Module`.  
5. Select the correct serial port under **Tools → Port**.  
6. Click **Upload** to flash the firmware.

> ⚙️ **Optional:**  
> Adjust **Flash Size** or **Partition Scheme** under Tools if your board differs from the standard ESP32 configuration.

---

## 🧭 Table of Contents
- [Overview](#overview)
- [Purpose](#purpose)
- [Device Wi-Fi Access Point](#device-wi-fi-access-point-for-initial-setup)
- [Hostname](#hostname)
- [API Summary](#api-summary)
  - [1. Save Wi-Fi Credentials (POST)](#1-save-wi-fi-credentials-post)
  - [2. Get Device Details / Status (GET)](#2-get-device-details--status-get)
  - [3. Change Device Name / Hostname (POST)](#3-change-device-name--hostname-post)
  - [4. IR Send (POST)](#4-ir-send-post)
  - [5. IR Capture / Last IR (GET)](#5-ir-capture--last-ir-get)
- [Development Notes & Troubleshooting](#development-notes--troubleshooting)
- [Example Integration Flows](#example-integration-flows)
  - [1. Provisioning Flow](#1-provisioning-flow-mobile-or-laptop)
  - [2. Capturing and Testing an IR Command](#2-capturing-and-testing-an-ir-command)
- [Contributing](#contributing)
- [License](#license)

---

## Overview
**KinkonyAGFW** is an open firmware for using the **Kinkony AG IR+RF Hub** with **Haptique RS90** and other compatible devices.  
It enables Wi-Fi provisioning, HTTP-based control, IR transmission and capture, and hostname customization for integration into smart home ecosystems.

---

## Purpose
This document explains how to:
- Connect the device  
- Configure Wi-Fi  
- Use the available HTTP APIs  
- Send and capture IR commands  

Save this file as `.md` and share it with engineers integrating the device.

---

## Device Wi-Fi Access Point (for initial setup)

**SSID:** `HAP_IRHUB`  
**Password:** `12345678`

> ⚠️ **Important:** This access point is used **only for initial configuration**.  
> Connect to it using a 2.4 GHz network before proceeding with API setup.

---

## Hostname

**Default Hostname:** `haptique-extender`

If mDNS/NetBIOS name resolution works, use the hostname directly.  
Otherwise, use the IP found in your router’s DHCP list or from `/api/status`.

---

## API Summary

**Base (hostname):** [http://haptique-extender](http://haptique-extender)  
**Base (IP):** `http://<device-ip>`

---

### 1. Save Wi-Fi Credentials (POST)
```bash
POST http://haptique-extender/api/wifi/save
```
**Body:**
```json
{
  "ssid": "YourSSID(2.4Ghz)",
  "pass": "YourPassword"
}
```
**Example:**
```bash
curl -X POST -H "Content-Type: application/json"   -d '{"ssid":"HomeNetwork","pass":"MyWifiPass"}'   http://haptique-extender/api/wifi/save
```

---

### 2. Get Device Details / Status (GET)
```bash
curl http://haptique-extender/api/status
```

---

### 3. Change Device Name / Hostname (POST)
```bash
curl -X POST -H "Content-Type: application/json"   -d '{"instance":"LivingRoomExtender"}'   http://haptique-extender/api/hostname
```

---

### 4. IR Send (POST)
```bash
curl -X POST -H "Content-Type: application/json"   -d '{"freq_khz":38,"duty":33,"repeat":1,"raw":[9000,4500,560,560,560,560]}'   http://192.168.1.100/api/ir/send
```

---

### 5. IR Capture / Last IR (GET)
```bash
curl http://192.168.1.100/api/ir/last
```

---

## Development Notes & Troubleshooting
- Use `HAP_IRHUB` AP only for provisioning.
- If `haptique-extender` doesn’t resolve:
  - Check router DHCP list for IP.
  - Use direct IP for all API calls.
- Always ensure **Content-Type: application/json** in POST requests.
- Only **2.4 GHz** networks are supported.
- Change your network after provisioning for better security.
- Future firmware updates will add HTTPS & authentication.
- Check device logs or serial console for debug info.

---

## Example Integration Flows

### 1. Provisioning Flow
1. Connect to SSID `HAP_IRHUB` (password `12345678`)
2. `POST` credentials to `/api/wifi/save`
3. Wait 20–60 s for Wi-Fi connection
4. `GET /api/status` to confirm connection
5. Use the IP or hostname for all further API requests

---

### 2. Capturing and Testing an IR Command
1. Press a remote button near the IR receiver  
2. `GET /api/ir/last` to retrieve captured data  
3. `POST /api/ir/send` using that data to replay the signal  

---

## Contributing
Pull requests and improvements are welcome.  
Fork this repo, make your changes, and submit a PR.

---

## License
This project is licensed under the **MIT License** — see the [LICENSE](./LICENSE) file.

---

© 2025 Cantata Communication Solutions  
*Developed for the Haptique RS90 platform and Kincony AG Hub integration.*
