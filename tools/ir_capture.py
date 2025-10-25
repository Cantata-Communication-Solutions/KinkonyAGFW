#!/usr/bin/env python3
"""
ir_capture.py
Fetch the last captured IR frame from /api/ir/last

Usage:
  python ir_capture.py --host haptique-extender
  python ir_capture.py --host 192.168.1.100 --raw-only
"""

import argparse
import requests
import sys
import json

DEFAULT_TIMEOUT = 8.0

def fetch_last(host, timeout=DEFAULT_TIMEOUT):
    url = f"http://{host}/api/ir/last"
    r = requests.get(url, timeout=timeout)
    return r

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True)
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    p.add_argument("--raw-only", action="store_true", help="print just the combined raw CSV (if present)")
    args = p.parse_args()

    try:
        r = fetch_last(args.host, timeout=args.timeout)
    except Exception as e:
        print("[ERROR] Request failed:", e)
        sys.exit(2)

    if r.status_code == 404:
        print("[INFO] No capture present yet.")
        sys.exit(1)

    try:
        j = r.json()
    except Exception:
        print("[ERROR] Non-JSON response:", r.text)
        sys.exit(3)

    if args.raw_only:
        combined = j.get("combined") or j.get("combined", [])
        if isinstance(combined, list):
            print(",".join(str(int(x)) for x in combined))
        elif isinstance(combined, str):
            print(combined)
        else:
            print(json.dumps(j, indent=2))
        sys.exit(0)

    print(json.dumps(j, indent=2))
    sys.exit(0)

if __name__ == "__main__":
    main()
