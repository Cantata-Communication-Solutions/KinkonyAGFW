# KinkonyAGFW  
**IR + RF Hub Firmware for Kinkony AG Hub with Haptique RS90**

![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)
![License](https://img.shields.io/badge/license-MIT-lightgrey.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-orange.svg)

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
**KinkonyAGFW** is an **open firmware** for using the **Kinkony AG IR+RF Hub** with **Haptique RS90** and other compatible devices.  
It enables Wi-Fi provisioning, HTTP-based control, IR transmission and capture, and hostname customization for integration into smart home ecosystems.

---

## Purpose
This developer document explains how to:
- Connect the device  
- Configure Wi-Fi  
- Use the available HTTP APIs  
- Send and capture IR commands  

Save this file as `.txt` or `.md` and share it with engineers working on device integration.

---

## Device Wi-Fi Access Point (for initial setup)

**SSID:** `HAP_IRHUB`  
**Password:** `12345678`

> ⚠️ **Important:** This AP is used **only** for initial configuration.  
> Connect to the AP (2.4 GHz only) using a laptop or mobile device to use the configuration APIs below.

---

## Hostname

**Default Hostname:** `haptique-extender`

Use the hostname if your client and device are on the same local network and mDNS/NetBIOS name resolution works.  
If name resolution fails, use the device IP from your router’s DHCP table or retrieved via the `/api/status` endpoint.

---

## API Summary

**Base (hostname):** [http://haptique-extender](http://haptique-extender)  
**Base (IP):** `http://<device-ip>`

---

### 1. Save Wi-Fi Credentials (POST)

**URL**
```
POST http://haptique-extender/api/wifi/save
```

**Headers**
```http
Content-Type: application/json
```

**Body**
```json
{
  "ssid": "YourSSID(2.4Ghz)",
  "pass": "YourPassword"
}
```

**Behavior**
- The device saves provided credentials and attempts to connect.  
- Only **2.4 GHz** networks are supported.

**Example (curl)**
```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"ssid":"HomeNetwork","pass":"MyWifiPass"}' \
  http://haptique-extender/api/wifi/save
```

---

### 2. Get Device Details / Status (GET)

**URL**
```
GET http://haptique-extender/api/status
```

**Returns**
A JSON object containing:
- Connection status  
- IP address (if connected)  
- Firmware info  
- Last IR capture data  

**Example (curl)**
```bash
curl http://haptique-extender/api/status
```

Use this endpoint to confirm Wi-Fi connection and retrieve the assigned IP.

---

### 3. Change Device Name / Hostname (POST)

**URL**
```
POST http://haptique-extender/api/hostname
```

**Headers**
```http
Content-Type: application/json
```

**Body**
```json
{
  "instance": "Haptique Extender"
}
```

**Example (curl)**
```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"instance":"LivingRoomExtender"}' \
  http://haptique-extender/api/hostname
```

> 💡 Hostname changes may take up to a minute to propagate via mDNS.

---

### 4. IR Send (POST)

**URL**
```
POST http://<device-ip>/api/ir/send
```

**Headers**
```http
Content-Type: application/json
```

**Body Examples**
```json
{ "pronto": "850,856,950,..." }
```
or
```json
{ "id": "power_on" }
```
or detailed format:
```json
{
  "freq_khz": 38,
  "duty": 33,
  "repeat": 1,
  "raw": [9000, 4500, 560, 560, 560, 560]
}
```

**Example (curl)**
```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"freq_khz":38,"duty":33,"repeat":1,"raw":[9000,4500,560,560,560,560]}' \
  http://192.168.1.100/api/ir/send
```

---

### 5. IR Capture / Last IR (GET)

**URL**
```
GET http://<device-ip>/api/ir/last
```

**Returns**
Latest IR capture data in JSON format — may include:
- Raw timing array  
- Decoded protocol/code  
- Pronto string  

**Example (curl)**
```bash
curl http://192.168.1.100/api/ir/last
```

---

## Development Notes & Troubleshooting

- Use AP `HAP_IRHUB` only for provisioning.  
  Once Wi-Fi is configured, use the LAN IP for all further API calls.
- If `haptique-extender` doesn’t resolve:
  - Check your router’s DHCP client list.
  - Use direct IP: `http://<ip>/api/status`
- For POST errors:
  - Verify `Content-Type: application/json` is set.
  - Validate JSON syntax.
- Ensure your router’s SSID is **2.4 GHz** (802.11b/g/n).
- **Security:**  
  The default AP password is weak (`12345678`). After setup, restrict access via network/firewall.  
  Future firmware may add HTTPS & authentication.
- **Logs:**  
  Check device serial console or firmware logs for debugging failed API calls.

---

## Example Integration Flows

### 1. Provisioning Flow (Mobile or Laptop)
1. Connect to SSID `HAP_IRHUB` (password `12345678`)
2. `POST` Wi-Fi credentials to `/api/wifi/save`
3. Wait ~20–60 s for connection
4. `GET /api/status` to confirm connection and IP
5. Use IP or hostname for future API requests

---

### 2. Capturing and Testing an IR Command
1. Press a button on your remote near the IR receiver  
2. `GET /api/ir/last` to read captured data  
3. Use that JSON body in `POST /api/ir/send` to replay the signal  

---

## Contributing
Contributions are welcome!  
If you’d like to improve or extend the firmware:
1. Fork the repository  
2. Create a feature branch (`git checkout -b feature/xyz`)  
3. Commit your changes (`git commit -am "Add new feature"`)  
4. Push to your fork and open a Pull Request  

---

## License
This project is licensed under the **MIT License** — see the [LICENSE](./LICENSE) file for details.

---

**© 2025 Cantata Communication Solutions.**  
_Developed for the Haptique RS90 platform and Kinkony AG Hub integration._
