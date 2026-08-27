#!/usr/bin/env bash
# Builds and runs every host-side (no ESP toolchain) test in test/host/.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p test/host/build
FAIL=0

for test_src in test/host/test_*.cpp; do
  name="$(basename "$test_src" .cpp)"
  bin="test/host/build/$name"

  # Each test only needs the specific portable .cpp source files it
  # exercises -- listed alongside the test rather than globbed, so a test
  # accidentally pulling in an Arduino-dependent file fails to compile
  # loudly instead of silently.
  extra_srcs=()
  case "$name" in
    test_protocol_codec)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/protocol_codec.cpp)
      ;;
    test_backend_url_validation)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/backend_url_validation.cpp)
      ;;
    test_retry_policy)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/retry_policy.cpp)
      ;;
    test_json_extract)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/json_extract.cpp)
      ;;
    test_state_projection)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/json_extract.cpp
                  firmware/kiosk_runtime_v2/src/protocol/state_projection.cpp)
      ;;
    test_event_response)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/json_extract.cpp
                  firmware/kiosk_runtime_v2/src/protocol/state_projection.cpp
                  firmware/kiosk_runtime_v2/src/protocol/event_response.cpp)
      ;;
    test_ui_bundle)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/json_extract.cpp
                  firmware/kiosk_runtime_v2/src/protocol/state_projection.cpp
                  firmware/kiosk_runtime_v2/src/protocol/ui_bundle.cpp)
      ;;
    test_sequence_reservation) ;;  # header-only
    test_ap_ssid)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/ap_ssid.cpp)
      ;;
    test_vn_font_core)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/vn_font_core.cpp)
      ;;
    test_event_journal)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/json_extract.cpp
                  firmware/kiosk_runtime_v2/src/protocol/protocol_codec.cpp
                  firmware/kiosk_runtime_v2/src/protocol/journal_record.cpp
                  firmware/kiosk_runtime_v2/src/protocol/event_journal_core.cpp
                  firmware/kiosk_runtime_v2/src/protocol/event_journal_index.cpp)
      ;;
    test_low_memory_supervisor)
      extra_srcs=(firmware/kiosk_runtime_v2/src/health/low_memory_supervisor.cpp)
      ;;
    test_ui_timeout_policy)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/state_projection.cpp
                  firmware/kiosk_runtime_v2/src/protocol/json_extract.cpp
                  firmware/kiosk_runtime_v2/src/runtime/ui_timeout_policy.cpp)
      ;;
    test_environment_label)
      extra_srcs=(firmware/kiosk_runtime_v2/src/protocol/environment_label.cpp)
      ;;
    test_network_state)
      extra_srcs=(firmware/kiosk_runtime_v2/src/network/network_state.cpp)
      ;;
  esac

  echo "--- $name ---"
  g++ -std=c++17 -Wall -Wextra -o "$bin" "$test_src" "${extra_srcs[@]}"
  if ! "$bin"; then
    FAIL=1
  fi
  echo
done

exit $FAIL
