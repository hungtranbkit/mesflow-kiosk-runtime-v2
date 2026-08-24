#!/usr/bin/env bash
# Builds (DEV profile, always -- see note below) and flashes the artifact.
# NOT run automatically by anything in this repo — this touches real
# hardware. Confirm the target port is a development board you intend to
# overwrite before running this.
#
# Only ever flashes the DEV profile: PROD is not expected to be flashed to
# this dev board (docs/SECURITY.md / scripts/build-prod.sh's own comment).
# If you genuinely need to flash a PROD artifact, build it with
# scripts/build-prod.sh and upload its .bin manually with esptool/arduino-cli
# --input-dir -- deliberately not one-command-easy here.
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${1:-/dev/ttyACM0}"
FQBN="esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=default_8MB,PSRAM=opi,CDCOnBoot=cdc"

echo "About to flash firmware/kiosk_runtime_v2 (DEV profile) to ${PORT} using FQBN ${FQBN}."
echo "This will overwrite whatever firmware currently runs on that device."
read -r -p "Type YES to continue: " confirm
if [ "$confirm" != "YES" ]; then
  echo "Aborted."
  exit 1
fi

./scripts/build-dev.sh
arduino-cli upload -p "$PORT" --fqbn "$FQBN" \
  --input-dir firmware/kiosk_runtime_v2/build-dev/esp32.esp32.esp32s3
