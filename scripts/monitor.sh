#!/usr/bin/env bash
set -euo pipefail
PORT="${1:-/dev/ttyACM0}"
arduino-cli monitor -p "$PORT" -c baudrate=115200
