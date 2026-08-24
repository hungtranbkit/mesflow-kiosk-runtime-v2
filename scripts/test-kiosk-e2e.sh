#!/usr/bin/env bash
# Kiosk E2E Functional Test Runner wrapper.
#
# Drives a real ESP32 board through the kiosk's basic functionality
# end-to-end, capturing real screenshots and asserting state at every layer.
# See tools/kiosk_e2e_runner.py for the full implementation and
# docs/TEST_PLAN.md for the KIOSK-E2E test list this exercises.
#
# Required env: BACKEND_URL (no safe default -- normally an ephemeral tunnel
# hostname). Optional: ESP_IP (default 192.168.100.81), ESP_PORT (8081),
# DEVICE_ID (KIOSK-LASER-01), SERIAL_PORT (/dev/ttyACM0).
#
# Usage:
#   BACKEND_URL=https://xxxx.lhr.life ./scripts/test-kiosk-e2e.sh
#   BACKEND_URL=https://xxxx.lhr.life ./scripts/test-kiosk-e2e.sh --visual-sweep
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -z "${BACKEND_URL:-}" ]; then
  echo "ERROR: BACKEND_URL env var is required (e.g. https://xxxx.lhr.life)." >&2
  echo "Usage: BACKEND_URL=https://xxxx.lhr.life $0 [--visual-sweep] [--soak-cycles N]" >&2
  exit 2
fi

exec python3 tools/kiosk_e2e_runner.py "$@"
