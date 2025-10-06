# KinkonyAGFW
IR+RF Hub firmware to use Kinkony AG hub with Haptique RS90

Haptique Extender — Developer Instructions

## Purpose

This plain-text developer document explains how to connect the device, configure Wi‑Fi, use the available HTTP APIs, and send/capture IR commands. Save this file as a Notepad (.txt) file and share with engineers working with the device.

## Device Wi‑Fi Access Point (for initial setup)

SSID: HAP_IRHUB
Password: 12345678

Important: This AP is used only for initial configuration. Connect a laptop or mobile to the AP (2.4 GHz only) to call the configuration APIs described below.

## Hostname

Default Hostname: haptique-extender

Use the hostname when the device and your client are on the same link and mDNS/NetBIOS name resolution works. If name resolution does not work, use the device IP discovered via your router DHCP table or the device’s status API.

## APIs Summary

Base (hostname): [http://haptique-extender](http://haptique-extender)
Base (IP): http://<device-ip>

1. Save Wi‑Fi credentials (POST)

---

URL:
POST [http://haptique-extender/api/wifi/save](http://haptique-extender/api/wifi/save)

Headers:
Content-Type: application/json

Body (JSON):
{
"ssid": "YourSSID(2.4Ghz)",
"pass": "YourPassword"
}

Behavior:

* Device will save provided SSID and password and attempt to connect to that network.
* Use a 2.4 GHz network SSID (the device does not support 5 GHz).

Example (curl):
curl -X POST 
-H "Content-Type: application/json" 
-d '{"ssid":"HomeNetwork","pass":"MyWifiPass"}' 
[http://haptique-extender/api/wifi/save](http://haptique-extender/api/wifi/save)

2. Get Device Details / Status (GET)

---

URL:
GET [http://haptique-extender/api/status](http://haptique-extender/api/status)

Returns:

* JSON object containing connection status, current IP (if connected), firmware info, last IR captured, etc. (exact fields depend on firmware release).

Example (curl):
curl [http://haptique-extender/api/status](http://haptique-extender/api/status)

Use this endpoint to verify that the device connected successfully to your Wi‑Fi and to find the assigned IP address.

3. Change the Device Name / Hostname (POST)

---

URL:
POST [http://haptique-extender/api/hostname](http://haptique-extender/api/hostname)

Headers:
Content-Type: application/json

Body (JSON):
{
"instance": "Haptique Extender"
}

Example (curl):
curl -X POST -H "Content-Type: application/json" 
-d '{"instance":"LivingRoomExtender"}' 
[http://haptique-extender/api/hostname](http://haptique-extender/api/hostname)

Note:

* Changing hostname may require reconnecting or waiting a minute for mDNS/NetBIOS advertisement to update.

4. IR Send (POST)

---

URL (use device IP or hostname):
POST http://<device-ip>/api/ir/send

Headers:
Content-Type: application/json

Body:

* Depends on firmware. Typical examples:

  * Send raw pulse array: { "pronto": "850, 856, 950, ...." }
  * Send encoded command id: { "id": "power_on" }

If your firmware accepts a specific format, include that exact JSON. If uncertain, check the attached firmware source for `api/ir/send` handler.

Example {
  "freq_khz": 38,
  "duty": 33,
  "repeat": 1,
  "raw": [9000,4500,560,560,560,560]
} 

[http://192.168.1.100/api/ir/send](http://192.168.1.100/api/ir/send)

5. IR Capture / Last IR (GET)

---

URL:
GET http://<device-ip>/api/ir/last

Returns:

* The most recent IR capture in a JSON structure (e.g. raw timing array, decoded protocol and code, or pronto format depending on firmware).

Example (curl):
curl [http://192.168.1.100/api/ir/last](http://192.168.1.100/api/ir/last)

## Development Notes & Troubleshooting

• Use the device AP (HAP_IRHUB) only to provision Wi‑Fi. Once Wi‑Fi credentials are saved and the device connects, use device IP on your LAN for API calls.

• If hostname `haptique-extender` does not resolve:

* Check router DHCP client list for the device’s IP.
* Use `http://<ip>/api/status`.

• If POST calls return errors:

* Ensure Content-Type: application/json header is set.
* Validate JSON body (use single-line compact JSON for curl or double-quoted strings in programs).

• Note about 2.4 GHz: many IoT modules only support 2.4 GHz networks (802.11b/g/n). Confirm router SSID frequency and disable 5 GHz-only settings when provisioning.

• Security: The initial AP password is simple (12345678). After provisioning, restrict access to the device by placing it on a trusted network or using firewall rules. Consider firmware updates to add HTTPS and authentication.

• Logs: Check the device serial console or firmware logs (if available in source) for debug output when calls fail.

## Examples of integration flows

1. Provisioning flow (mobile or laptop):

* Connect to SSID: HAP_IRHUB (password 12345678)
* POST credentials to /api/wifi/save
* Wait ~20–60 seconds for device to connect
* Call /api/status to get IP and confirm connection
* Use that IP for future API calls (or rely on hostname if resolvable)

2. Capturing and testing an IR command:

* With IR receiver connected, press remote button near device
* Call GET /api/ir/last to read captured code
* Use the returned code body to send the command via POST /api/ir/send

