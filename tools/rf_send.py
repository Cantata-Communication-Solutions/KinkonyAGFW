#!/usr/bin/env python3
"""
rf_send.py
Send RF 433 MHz code to /api/rf/send

Usage:
  python rf_send.py --host 192.168.1.100 --code 123456 --bits 24 --protocol 1 --repeat 8
  python rf_send.py --host haptique-extender --code 0x1A2B3C --bits 24
"""

import argparse
import requests
import sys
import json

DEFAULT_TIMEOUT = 8.0

def parse_code(s):
    s = s.strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s, 10)

def send_rf(host, code, bits=24, protocol=1, pulselen=0, repeat=8, timeout=DEFAULT_TIMEOUT):
    url = f"http://{host}/api/rf/send"
    payload = {"code": code, "bits": bits, "protocol": protocol, "pulselen": pulselen, "repeat": repeat}
    r = requests.post(url, json=payload, timeout=timeout)
    return r

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True)
    p.add_argument("--code", required=True, help="numeric RF code (decimal or 0xHEX)")
    p.add_argument("--bits", type=int, default=24)
    p.add_argument("--protocol", type=int, default=1)
    p.add_argument("--pulselen", type=int, default=0)
    p.add_argument("--repeat", type=int, default=8)
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    args = p.parse_args()

    try:
        code = parse_code(args.code)
    except Exception as e:
        print("[ERROR] invalid code:", e)
        sys.exit(2)

    try:
        r = send_rf(args.host, code, bits=args.bits, protocol=args.protocol, pulselen=args.pulselen, repeat=args.repeat, timeout=args.timeout)
    except Exception as e:
        print("[ERROR] request failed:", e)
        sys.exit(3)

    try:
        j = r.json()
    except Exception:
        j = {"status_code": r.status_code, "text": r.text}

    if r.ok:
        print("[OK] RF send accepted:", json.dumps(j, indent=2))
        sys.exit(0)
    else:
        print("[ERROR] RF send failed:", json.dumps(j, indent=2))
        sys.exit(4)

if __name__ == "__main__":
    main()
