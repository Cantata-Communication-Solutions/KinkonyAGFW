#!/usr/bin/env python3
"""
wifi_config.py
Send Wi-Fi SSID/password to /api/wifi/save

Usage:
  python wifi_config.py --host haptique-extender --ssid MySSID --pass MyPass
  python wifi_config.py --host 192.168.1.100 -s MySSID -p MyPass --wait 30
"""

import argparse
import requests
import sys
import time

DEFAULT_TIMEOUT = 10.0

def send_wifi(host, ssid, password, timeout=DEFAULT_TIMEOUT, wait_after=None):
    url = f"http://{host}/api/wifi/save"
    payload = {"ssid": ssid, "pass": password}
    try:
        r = requests.post(url, json=payload, timeout=timeout)
    except Exception as e:
        print(f"[ERROR] Request failed: {e}")
        return False, None

    try:
        j = r.json()
    except Exception:
        j = {"status_code": r.status_code, "text": r.text}

    print("[INFO] Response:", j)
    if wait_after:
        print(f"[INFO] Waiting {wait_after} seconds for device to attempt connection...")
        time.sleep(wait_after)
    return r.ok, j

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True, help="device hostname or IP (e.g. haptique-extender or 192.168.1.100)")
    p.add_argument("--ssid", "-s", required=True, help="Wi-Fi SSID (2.4 GHz)")
    p.add_argument("--pass", "-p", dest="password", required=True, help="Wi-Fi password")
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    p.add_argument("--wait", type=int, default=20, help="seconds to wait after POST for device to connect (optional)")
    args = p.parse_args()

    ok, resp = send_wifi(args.host, args.ssid, args.password, timeout=args.timeout, wait_after=args.wait)
    if ok:
        print("[OK] Wi-Fi save request accepted.")
        sys.exit(0)
    else:
        print("[FAIL] Wi-Fi save failed.")
        sys.exit(2)

if __name__ == "__main__":
    main()
