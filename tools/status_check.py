#!/usr/bin/env python3
"""
status_check.py
Poll /api/status to check connection & IP.

Usage:
  python status_check.py --host haptique-extender
  python status_check.py --host 192.168.1.100 --follow --interval 5 --timeout 60
"""

import argparse
import requests
import time
import sys

DEFAULT_TIMEOUT = 10.0

def fetch_status(host, timeout=DEFAULT_TIMEOUT):
    url = f"http://{host}/api/status"
    try:
        r = requests.get(url, timeout=timeout)
        r.raise_for_status()
        return True, r.json()
    except requests.RequestException as e:
        return False, {"error": str(e)}

def pretty_print_status(j):
    print("=== Device status ===")
    for k in ("hostname","instance","ap_on","ap_ip","sta_ip","sta_ok","sta_ssid","fw_ver","mac"):
        if k in j:
            print(f"{k:12}: {j[k]}")
    # print full JSON if more details
    others = {k:v for k,v in j.items() if k not in ("hostname","instance","ap_on","ap_ip","sta_ip","sta_ok","sta_ssid","fw_ver","mac")}
    if others:
        print("\nFull JSON:")
        import json
        print(json.dumps(j, indent=2))

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True)
    p.add_argument("--follow", action="store_true", help="poll until sta_ok=true or timeout")
    p.add_argument("--interval", type=float, default=5.0, help="poll interval seconds")
    p.add_argument("--timeout", type=float, default=60.0, help="max seconds to wait when --follow")
    p.add_argument("--req-timeout", type=float, default=DEFAULT_TIMEOUT, help="HTTP request timeout")
    args = p.parse_args()

    if not args.follow:
        ok, j = fetch_status(args.host, timeout=args.req_timeout)
        if not ok:
            print("[ERROR] Could not fetch status:", j.get("error"))
            sys.exit(2)
        pretty_print_status(j)
        sys.exit(0)

    # follow mode
    start = time.time()
    while True:
        ok, j = fetch_status(args.host, timeout=args.req_timeout)
        if ok:
            pretty_print_status(j)
            if j.get("sta_ok"):
                print("[OK] Device is connected to Wi-Fi.")
                sys.exit(0)
        else:
            print("[WARN] fetch failed:", j.get("error"))
        if time.time() - start > args.timeout:
            print("[TIMEOUT] Device did not connect within timeout.")
            sys.exit(3)
        time.sleep(args.interval)

if __name__ == "__main__":
    main()
