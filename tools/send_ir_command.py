#!/usr/bin/env python3
import requests, sys

DEVICE_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.100"
url = f"http://{DEVICE_IP}/api/ir/send"

payload = {
    "freq_khz": 38,
    "duty": 33,
    "repeat": 1,
    "raw": [9000,4500,560,560,560,560]
}

print(f"📡 Sending IR command to {url} ...")

try:
    res = requests.post(url, json=payload, timeout=5)
    res.raise_for_status()
    print(f"✅ IR command sent successfully (status {res.status_code})")
except requests.exceptions.RequestException as e:
    print("❌ Failed to send IR command:")
    print(e)
