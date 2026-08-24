#!/usr/bin/env python3
"""Serial Visual Debug Fallback capture tool (docs/VISUAL_DEBUG.md).

Same output as tools/capture_screen.py, but goes straight to USB serial --
for when the host isn't on the same LAN/subnet as the device's WiFi and
HTTP /debug/* is simply unreachable. capture_screen.py's own --serial flag
covers the "try HTTP, then fall back" case; this is the direct entry point
for when you already know HTTP won't work.

Usage:
  python3 tools/capture_screen_serial.py /dev/serial/by-id/... [--baud 115200] [--out artifacts/debug]
"""
import argparse
import sys

from capture_screen import capture_via_serial, write_capture_bundle


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("serial_port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", default="artifacts/debug")
    parser.add_argument("--timeout", type=float, default=20,
                         help="Seconds to wait for each of the three payloads.")
    args = parser.parse_args()

    try:
        img, shot_meta, ui_state, device_state = capture_via_serial(
            args.serial_port, args.baud, timeout=args.timeout)
    except Exception as exc:  # noqa: BLE001 -- surfacing ANY failure honestly, not swallowing it
        print(f"ERROR: serial capture failed: {exc}", file=sys.stderr)
        sys.exit(1)

    write_capture_bundle(img, shot_meta, ui_state, device_state, args.out,
                          "SERIAL", args.serial_port)


if __name__ == "__main__":
    main()
