#!/usr/bin/env python3
import requests, sys

DEVICE_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.100"
url = f"http://{DEVICE_IP}/api/status"

print(f"🌐 Testing connection to {url} ...")

try:
    response = requests.get(url, timeout=5)
    response.raise_for_status()
    print("✅ Status response:")
    print(response.json())
except requests.exceptions.RequestException as e:
    print("❌ Failed to connect or invalid response:")
    print(e)
