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
After Wi-Fi setup, the device returns a **token**.  
Include it in headers for all future requests:

**Behavior**
- Saves credentials and attempts to connect to the network.
- Only 2.4 GHz Wi-Fi is supported.

**Example (curl)**
```bash
curl -X POST ^
  -H "Content-Type: application/json" ^
  -d "{\"ssid\": \"YourSSID\", \"pass\": \"YourPassword\"}" ^
  http://haptique-extender.local/api/wifi/save

```
⚠️ Replace YourSSID and YourPassword with your actual Wi-Fi credentials.
---

## 🔍 Device Information API

### `GET /api/status`

with 
**Headers**
```
Content-Type: application/json
Authorization: Bearer{{token}}
```
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
Authorization: Bearer{{token}}
```

**Body Example**
```json
{
  "instance": "LivingRoomExtender"
}
```

**Example (curl)**
```bash
curl -X POST ^
  -H "Content-Type: application/json" ^
  -H "Authorization: Bearer YOUR_TOKEN_HERE" ^
  -d "{\"hostname\": \"haptique-extender\", \"instance\": \"Haptique Extender\"}" ^
  http://haptique-extender.local/api/hostname

```
instance: Friendly name shown in UI/Bonjour
---

## 📤 IR Send API

### `POST /api/ir/send`

Send a captured IR command.

**Headers**
```
Content-Type: application/json
Authorization: Bearer{{token}}
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
```
Content-Type: application/json
Authorization: Bearer{{token}}
````

**Response Example**
```json
{
    "type": "ir_rx",
    "freq_khz": 38,
    "frames": 2,
    "gap_ms": 107,
    "gap_us": 107000,
    "countA": 68,
    "a": [
        4523,4509,560,584,...],
    "countB": 68, "b": [ 4523,4509,560,584,...],
    "combined_count": 136,
    "combined": [ 4523,4509,560,584,...],
       }
```

---

## 🧩 Notes

- All APIs return JSON-formatted data.  
- Authentication is not required in the current firmware (for local use only).  
- Future versions may include HTTPS and token-based authentication.

---

© 2025 Cantata Communication Solutions — All rights reserved.
