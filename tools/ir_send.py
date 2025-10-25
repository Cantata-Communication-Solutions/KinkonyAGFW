#!/usr/bin/env python3
"""
ir_send.py
Send IR raw or pronto command to /api/ir/send

Usage:
  python ir_send.py --host 192.168.1.100 --raw 9000,4500,560,560 --freq 38
  python ir_send.py --host haptique-extender --pronto "0000 006D 0022 ..." --repeat 2
"""

import argparse
import requests
import sys
import json

DEFAULT_TIMEOUT = 12.0

def parse_raw(csv):
    parts = [p.strip() for p in csv.split(',') if p.strip()]
    out = []
    for p in parts:
        try:
            out.append(int(p))
        except ValueError:
            raise ValueError(f"Invalid integer in raw data: '{p}'")
    return out

def send_raw(host, raw, freq_khz=38, duty=33, repeat=1, timeout=DEFAULT_TIMEOUT):
    url = f"http://{host}/api/ir/send"
    payload = {"freq_khz": freq_khz, "duty": duty, "repeat": repeat, "raw": raw}
    r = requests.post(url, json=payload, timeout=timeout)
    return r

def send_pronto(host, pronto, timeout=DEFAULT_TIMEOUT):
    url = f"http://{host}/api/ir/send"
    payload = {"pronto": pronto}
    r = requests.post(url, json=payload, timeout=timeout)
    return r

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True)
    p.add_argument("--raw", help="Comma separated raw timings (us)")
    p.add_argument("--pronto", help="Pronto hex/string")
    p.add_argument("--freq", type=int, default=38, help="frequency in kHz")
    p.add_argument("--duty", type=int, default=33)
    p.add_argument("--repeat", type=int, default=1)
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    args = p.parse_args()

    if not args.raw and not args.pronto:
        print("Error: provide either --raw or --pronto")
        sys.exit(2)

    try:
        if args.raw:
            raw = parse_raw(args.raw)
            resp = send_raw(args.host, raw, freq_khz=args.freq, duty=args.duty, repeat=args.repeat, timeout=args.timeout)
        else:
            resp = send_pronto(args.host, args.pronto, timeout=args.timeout)

        try:
            data = resp.json()
        except Exception:
            data = {"status_code": resp.status_code, "text": resp.text}

        if resp.ok:
            print("[OK] IR send accepted:", json.dumps(data, indent=2))
            sys.exit(0)
        else:
            print("[ERROR] IR send failed:", json.dumps(data, indent=2))
            sys.exit(3)
    except Exception as e:
        print("[EXCEPTION]", e)
        sys.exit(4)

if __name__ == "__main__":
    main()
