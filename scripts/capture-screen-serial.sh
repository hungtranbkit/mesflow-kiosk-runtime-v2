#!/usr/bin/env bash
# Captures a real screenshot + ui-state + device-state from a running kiosk
# over USB SERIAL instead of HTTP -- for when the host running this isn't on
# the same LAN/subnet as the device's WiFi (see docs/VISUAL_DEBUG.md "Serial
# Visual Debug Fallback"). Writes the SAME artifacts/debug/<timestamp>/
# bundle shape scripts/capture-screen.sh does.
#
# Usage: scripts/capture-screen-serial.sh <serial-by-id-path> [--baud 115200]
set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -lt 1 ]; then
  echo "Usage: $0 <serial-by-id-path> [--baud 115200]" >&2
  exit 1
fi

python3 tools/capture_screen_serial.py "$@"
