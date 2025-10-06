# KinkonyAGFW  
**IR + RF Hub Firmware for Kincony AG Hub with Haptique RS90**

![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)
![License](https://img.shields.io/badge/license-MIT-lightgrey.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-orange.svg)

---

## 🔍 Overview

**KinkonyAGFW** is an open firmware designed to use the **Kincony AG IR+RF Hub** with **Haptique RS90** and other compatible devices.  
It provides Wi-Fi provisioning, HTTP-based control, IR signal transmission and capture, and hostname customization — enabling integration with modern smart home systems.

---

## 📦 Firmware Download

The latest stable firmware is available in the [`/bin`](./bin) directory:  
👉 [Haptique_Kincony_AG_Firmware_V1.bin](./bin/Haptique_Kincony_AG_Firmware_V1.bin)

---

## ⚙️ Flashing Instructions

### 🧰 Requirements
- **Kincony AG Hub** (or compatible ESP32-based IR + RF hub)  
- **USB-to-Mini-USB cable** (for power and flashing)  
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) or [esptool.py](https://github.com/espressif/esptool)

> 💡 **Note:** No external UART adapter is required — the Kincony AG Hub supports native USB flashing via its mini-USB port.

---

### 🧑‍💻 Flashing via `esptool.py`
```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600   write_flash 0x1000 bin/Haptique_Kincony_AG_Firmware_V1.bin
```

---

### 🪟 Alternative Flash Method (Kincony Flash Tool)

If you prefer a graphical flashing tool on Windows, you can use the official **Kincony Flash Tool**:

1. Download from [Kincony’s site](https://www.kincony.com/esp-module-flash-download-tools.html)  
2. Open the tool and select the firmware file: `bin/Haptique_Kincony_AG_Firmware_V1.bin`  
3. Choose your COM port and click **Start** to flash  

> ⚙️ The Kincony tool provides a user-friendly interface but is **Windows-only**.  
> For cross-platform flashing, use [esptool.py](https://github.com/espressif/esptool).

---

## 🔄 After Flashing

When the device restarts, it creates a setup Wi-Fi network:

```
SSID: HAP_IRHUB
Password: 12345678
```

You can then follow the [Setup Guide](./docs/Device_Setup_Guide.md) to configure your Wi-Fi and start using the API.

---

## 💻 Building from Source

This repository also includes the **open-source Arduino firmware**.

**Source:** [`/KinkonyAGFW_Arduino`](./KinkonyAGFW_Arduino)  
**Main file:** [Haptique_Kincony_RF_RI_code.ino](./KinkonyAGFW_Arduino/Haptique_Kincony_RF_RI_code.ino)

### 🧩 Build Steps
1. Install [Arduino IDE](https://www.arduino.cc/en/software) or [PlatformIO](https://platformio.org/).  
2. Add **ESP32 Board Support** via Arduino Boards Manager.  
3. Open `KinkonyAGFW_Arduino/Haptique_Kincony_RF_RI_code.ino`.  
4. Select **ESP32 Dev Module** under Tools → Board.  
5. Choose the correct COM port under Tools → Port.  
6. Click **Upload** to flash.

> ⚙️ Optional: Adjust Flash Size or Partition Scheme in Tools if needed.

---

## 🧪 Developer Tools

Useful test and utility scripts are included in the [`/tools`](./tools) directory for developers and testers.

| Script | Description |
|--------|--------------|
| `flash_firmware.sh` | Flash firmware using `esptool.py` |
| `test_api_status.py` | Check device response at `/api/status` |
| `send_ir_command.py` | Send sample IR signal for testing transmission |
| `capture_ir_signal.py` | Retrieve and display last captured IR data |

> 💡 These tools require **Python 3** and the **requests** library (`pip install requests`).  
> The flash script works on Linux/macOS — Windows users can run it in WSL or adapt it for PowerShell.

---

## 📘 Documentation

- [Device Setup Guide](./docs/Device_Setup_Guide.md) — step-by-step configuration and flashing  
- [API Overview](./docs/API_Overview.md) — detailed list of HTTP endpoints and parameters  

---

## 🤝 Contributing

Pull requests and improvements are welcome.  
Fork this repo, make your changes, and submit a PR.

---

## ⚖️ License

This project is licensed under the **MIT License** — see [LICENSE](./LICENSE).

---

© 2025 Cantata Communication Solutions  
*Developed for the Haptique RS90 platform and Kincony AG Hub integration.*
