#!/usr/bin/env bash
# Captures a real screenshot + ui-state + device-state from a running kiosk
# (MESFLOW_DEBUG_API dev build) and writes a self-contained bundle under
# artifacts/debug/<timestamp>/ (see docs/VISUAL_DEBUG.md).
#
# Tries HTTP first; if unreachable (e.g. this host and the device are on
# different subnets) and --serial is given, falls back to USB serial
# automatically (same bundle shape either way -- see "Serial Visual Debug
# Fallback" in docs/VISUAL_DEBUG.md, or scripts/capture-screen-serial.sh to
# go straight to serial without trying HTTP first).
#
# Usage: scripts/capture-screen.sh <device-ip> [--port 8081] [--serial <serial-by-id-path>]
set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -lt 1 ]; then
  echo "Usage: $0 <device-ip> [--port 8081] [--serial <serial-by-id-path>]" >&2
  exit 1
fi

python3 tools/capture_screen.py "$@"
