# Haptique Extender (Kincony AG Hub) — Setup Guide

This guide helps you configure the **Haptique Extender firmware (KinkonyAGFW)** on your Kincony AG Hub and connect it with your Haptique RS90 system.

---

## ⚙️ Step 1: Hardware & Requirements

- Kincony AG Hub (ESP32-based)
- USB-to-Mini-USB cable
- Firmware file: `bin/Haptique_Kincony_AG_Firmware_V1.bin`
- A computer (Windows/macOS/Linux)

---

## ⚡ Step 2: Flash the Firmware

### Option A — Using esptool.py (Cross-Platform)

1. Download and install [esptool.py](https://github.com/espressif/esptool).
2. Connect your Kincony AG Hub via Mini-USB.
3. Open a terminal and run:
   ```bash
   esptool.py --chip esp32 write_flash 0x00000 bin/Haptique_Kincony_AG_Firmware_V1.bin
   ```
4. Wait until flashing completes and the device restarts.

> 💡 Tip: No external UART adapter is needed — the mini-USB port supports native flashing.

---

### Option B — Using Kincony Flash Tool (Windows)

1. Download from [Kincony Flash Tool page](https://www.kincony.com/esp-module-flash-download-tools.html).  
2. Open the tool and select the firmware file:  
   `bin/Haptique_Kincony_AG_Firmware_V1.bin`  
3. Choose the correct COM port and click **Start** to flash.

> ⚙️ The Kincony tool provides a simple UI but works on **Windows only**.

---

## 🌐 Step 3: Connect to Device Wi-Fi

1. Power the device.
2. Connect your computer/phone to the AP:  
   - **SSID:** `HAP_IRHUB`  
   - **Password:** `12345678`
3. Open `http://192.168.4.1/api/status` or `http://haptique-extender/api/status` in a browser.

---

## 📶 Step 4: Configure Wi-Fi

Use a REST client (like **curl** or **Postman**) to send:

```bash
curl -X POST -H "Content-Type: application/json" -d '{"ssid":"YourWiFi","pass":"YourPass"}' http://192.168.4.1/api/wifi/save
```

Once connected, find the new IP address in your router’s DHCP list or call `/api/status` again.

---

## 🎯 Step 5: Verify Connection

After successful setup:
- `/api/status` → returns device info and IP
- `/api/ir/last` → retrieves the last captured IR signal
- `/api/ir/send` → transmits a test IR signal

---

## 🧠 Troubleshooting

| Issue | Possible Cause | Solution |
|--------|----------------|-----------|
| Hostname not resolving | mDNS disabled on router | Use device IP instead |
| POST request fails | Wrong content type | Use `Content-Type: application/json` |
| IR send not working | Incorrect format | Check JSON body syntax |
| Wi-Fi fails | 5 GHz network | Use 2.4 GHz SSID only |

---

## 🧩 Next Steps

- Integrate with **Haptique Dashboard** or **RS90 App**
- Contribute improvements via GitHub pull requests
- Share your feedback on new API features

---

© 2025 Cantata Communication Solutions — All rights reserved.
