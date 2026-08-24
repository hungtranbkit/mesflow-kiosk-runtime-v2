#!/usr/bin/env python3
"""Generates test QR/barcode PNGs for physical GM65 smoke testing (§31 of
the E2E test-runner prompt).

These encode the SAME raw scan values the mock backend already accepts
(WF|EMP|00152, WF|OP|OP-001) — no protocol changes for testing. Meant to be
shown on another screen/printed and scanned by the real GM65 sensor; this
script does NOT itself perform a physical scan, and generating these PNGs
does not constitute a physical-hardware PASS on its own.

Usage: python3 tools/generate_test_qr.py [--out artifacts/e2e/qr]
"""
import argparse
import os

import qrcode


CODES = {
    "test-employee": "WF|EMP|00152",
    "test-operation": "WF|OP|OP-001",
    "invalid-code": "WF|EMP|99999",
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="artifacts/e2e/qr")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    for name, value in CODES.items():
        img = qrcode.make(value)
        path = os.path.join(args.out, f"{name}.png")
        img.save(path)
        print(f"{path}  <-  \"{value}\"")


if __name__ == "__main__":
    main()
