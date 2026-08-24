#!/usr/bin/env bash
# Compiles the firmware artifact for a given profile. Does NOT flash anything.
#
# Usage: scripts/build.sh [dev|prod]   (default: dev)
#
# Profile is baked into the binary at compile time (MESFLOW_PROFILE_DEV or
# MESFLOW_PROFILE_PROD, see src/config/build_info.h) -- it is not a runtime
# flag. DEV and PROD artifacts land in separate output directories so one
# never silently overwrites the other.
set -euo pipefail
cd "$(dirname "$0")/.."

PROFILE="${1:-dev}"
case "$PROFILE" in
  dev)  PROFILE_DEFINE="MESFLOW_PROFILE_DEV" ;;
  prod) PROFILE_DEFINE="MESFLOW_PROFILE_PROD" ;;
  *) echo "Usage: $0 [dev|prod]" >&2; exit 1 ;;
esac

FQBN="esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=default_8MB,PSRAM=opi,CDCOnBoot=cdc"
BUILD_ID="$(git rev-parse --short HEAD 2>/dev/null || echo local)-$(date -u +%Y%m%dT%H%M%SZ)-${PROFILE}"
OUTPUT_DIR="firmware/kiosk_runtime_v2/build-${PROFILE}/esp32.esp32.esp32s3"

arduino-cli compile \
  --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=-DKIOSK_BUILD_ID=\"${BUILD_ID}\" -D${PROFILE_DEFINE}=1" \
  --output-dir "$OUTPUT_DIR" \
  firmware/kiosk_runtime_v2

echo "Built profile=${PROFILE} BUILD_ID=${BUILD_ID}"
echo "Artifact: ${OUTPUT_DIR}/kiosk_runtime_v2.ino.bin"
