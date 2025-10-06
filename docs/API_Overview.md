# Haptique Extender API Overview

This document describes the REST APIs available for the **KinkonyAGFW** firmware (Haptique Extender for Kincony AG Hub).  
These APIs can be accessed via the device hostname or IP address once connected to the same network.

---

## 🌐 Base URLs

- **Hostname-based:** `http://haptique-extender`
- **IP-based:** `http://<device-ip>`

---

## 📡 Wi-Fi Configuration API

### `POST /api/wifi/save`

Save Wi-Fi credentials for the device.

**Headers**
```
Content-Type: application/json
```

**Body Example**
```json
{
  "ssid": "YourSSID(2.4Ghz)",
  "pass": "YourPassword"
}
```

**Behavior**
- Saves credentials and attempts to connect to the network.
- Only 2.4 GHz Wi-Fi is supported.

**Example (curl)**
```bash
curl -X POST -H "Content-Type: application/json" -d '{"ssid":"HomeNetwork","pass":"MyWifiPass"}' http://haptique-extender/api/wifi/save
```

---

## 🔍 Device Information API

### `GET /api/status`

Returns device status and configuration details.

**Response Example**
```json
{
  "wifi_status": "connected",
  "ip": "192.168.1.100",
  "hostname": "haptique-extender",
  "fw_version": "1.0.0",
  "last_ir": "power_on"
}
```

Use this endpoint to verify the Wi-Fi connection and discover the device IP.

---

## 🏷️ Hostname Configuration API

### `POST /api/hostname`

Change the device name/hostname.

**Headers**
```
Content-Type: application/json
```

**Body Example**
```json
{
  "instance": "LivingRoomExtender"
}
```

**Example (curl)**
```bash
curl -X POST -H "Content-Type: application/json" -d '{"instance":"LivingRoomExtender"}' http://haptique-extender/api/hostname
```

---

## 📤 IR Send API

### `POST /api/ir/send`

Send a captured IR command.

**Headers**
```
Content-Type: application/json
```

**Body Example**
```json
{
  "freq_khz": 38,
  "duty": 33,
  "repeat": 1,
  "raw": [9000,4500,560,560,560,560]
}
```

**Response**
```json
{"status":"sent"}
```

---

## 📥 IR Capture API

### `GET /api/ir/last`

Retrieve the most recent captured IR command.

**Example**
```bash
curl http://192.168.1.100/api/ir/last
```

**Response Example**
```json
{
  "protocol": "NEC",
  "address": "0x10",
  "command": "0xEF",
  "raw": [9000,4500,560,560,560,1690,...]
}
```

---

## 🧩 Notes

- All APIs return JSON-formatted data.  
- Authentication is not required in the current firmware (for local use only).  
- Future versions may include HTTPS and token-based authentication.

---

© 2025 Cantata Communication Solutions — All rights reserved.
