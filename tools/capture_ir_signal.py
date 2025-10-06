#!/usr/bin/env python3
import requests, sys

DEVICE_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.100"
url = f"http://{DEVICE_IP}/api/ir/last"

print(f"📡 Retrieving last IR capture from {url} ...")

try:
    res = requests.get(url, timeout=5)
    res.raise_for_status()
    print("✅ Last captured IR data:")
    print(res.json())
except requests.exceptions.RequestException as e:
    print("❌ Failed to get IR capture:")
    print(e)
