#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <string>

#include "../config/runtime_config.h"
#include "../hardware/display.h"
#include "../hardware/hardware_selftest.h"
#include "../hardware/keypad_pcf8574.h"
#include "../network/bootstrap_client.h"
#include "../network/time_sync.h"
#include "../runtime/boot_diagnostics.h"
#include "../runtime/event_bus.h"
#include "../runtime/kiosk_runtime.h"
#include "../runtime/ui_sync_controller.h"
#include "../security/device_identity.h"
#include "../storage/ui_bundle_store.h"
#include "../ui/renderer.h"

#if MESFLOW_DEBUG_API

namespace kiosk::debug {

// Remote Visual Debug subsystem (docs/VISUAL_DEBUG.md). Exists so a change
// to display/font/layout/keypad-visual-feedback/Wi-Fi-setup-UI can be
// verified by actually looking at the rendered screen, not just trusting
// serial logs (docs/ARCHITECTURE.md invariants 7/8/9).
//
// DEV/LAB profile ONLY (§14) -- gated at compile time by MESFLOW_DEBUG_API,
// not by any runtime check. A production build must not define this macro.
//
// Runs its own WebServer on MESFLOW_DEBUG_API_PORT (8081), deliberately
// separate from the Wi-Fi recovery portal's port 80, so this keeps working
// even while that portal is active (§17).
//
// Endpoints:
//   GET  /debug/screenshot    current framebuffer, custom binary format
//                             (see debug_server.cpp header layout comment)
//   GET  /debug/ui-state      JSON: current screen id + drawn lines, plus
//                             (Phase 4) "layout_id" (alias of screen_id, the
//                             UI bundle's screen key that produced this
//                             frame) and "ui_bundle_version" (0 if currently
//                             rendering a built-in hardcoded screen, not a
//                             downloaded bundle)
//   GET  /debug/device-state  JSON: device/network/memory/hardware/input,
//                             plus (Phase 4) a "ui" block: active_version/
//                             active_hash/schema_version/active_slot/
//                             last_good_version/desired_version/sync_state/
//                             last_update_error -- see storage/ui_bundle_store.h
//   POST /debug/input         JSON: inject SCAN/KEY_DOWN/KEY_UP through the
//                             SAME EventBus real hardware uses (§12/§13:
//                             this tests the application/UI path, NOT
//                             hardware -- physical smoke tests still matter).
//                             Response: {"accepted":true,"input_seq":N} --
//                             an automated test runner can confirm THIS
//                             specific input actually landed (device-state's
//                             input.last_input_seq) before waiting on any
//                             business-level effect (see the Kiosk E2E
//                             runner's layer-by-layer failure classification).
//
//   Serial fallback (no HTTP/LAN required, see write_*_serial above and
//   kiosk_runtime_v2.ino's debug-screenshot/debug-ui-state/debug-device-state
//   commands, docs/VISUAL_DEBUG.md "Serial Visual Debug Fallback"):
//   the exact same three payloads, framed for a USB serial connection
//   instead of an HTTP request.
//
//   POST /debug/qa-session    JSON {"run_id":"...","step":"..."} marks a
//                             test-runner session as active (visible in
//                             device-state's "qa" block AND as a small "QA"
//                             marker on every normal screen) so a person
//                             looking at the physical device can tell at a
//                             glance whether automation is actually driving
//                             it right now. {"active":false} (or an empty
//                             body) clears it. Pure observability -- never
//                             gates or changes any business behavior.
//
// Frame consistency (§27): no mutex is used, and none is needed in Phase 0
// -- WebServer::handleClient() and every draw call both run on the single
// Arduino loopTask, so a screenshot handler can never execute concurrently
// with a draw() call. This stops being true the moment either side moves to
// its own FreeRTOS task; whoever does that next must add real
// synchronization around the framebuffer at that point.
class DebugServer {
 public:
  DebugServer(kiosk::runtime::EventBus& bus, kiosk::hardware::Display& display,
              kiosk::ui::Renderer& renderer, kiosk::runtime::KioskRuntime& runtime,
              kiosk::hardware::KeypadPcf8574& keypad,
              const kiosk::hardware::SelfTestResult& selftest,
              kiosk::runtime::BootDiagnostics& diagnostics,
              kiosk::security::DeviceIdentity& identity, kiosk::network::TimeSync& time_sync,
              const kiosk::network::BootstrapClient& bootstrap,
              const kiosk::storage::UiBundleStore& ui_bundle_store,
              const kiosk::runtime::UiSyncController& ui_sync)
      : bus_(bus),
        display_(display),
        renderer_(renderer),
        runtime_(runtime),
        keypad_(keypad),
        selftest_(selftest),
        diagnostics_(diagnostics),
        identity_(identity),
        time_sync_(time_sync),
        bootstrap_(bootstrap),
        ui_bundle_store_(ui_bundle_store),
        ui_sync_(ui_sync) {}

  void begin();
  void poll();  // call every loop() iteration

  // --- Serial fallback (§ "Serial Visual Debug Fallback") ---
  // The HTTP endpoints above require the host to be on the same LAN/subnet
  // as the device's WiFi -- not always true (e.g. a dev laptop on a
  // different network than the kiosk's test AP). These give the exact same
  // three payloads (screenshot/ui-state/device-state) over USB serial
  // instead, reusing the SAME builder methods the HTTP handlers call --
  // deliberately not a second, separately-maintained JSON/binary builder
  // (the class of bug this project keeps finding: see bootstrap_client.cpp's
  // and debug_server.cpp's own comments on duplicate parsers/builders
  // drifting apart). DEV-only serial commands in kiosk_runtime_v2.ino
  // (debug-screenshot / debug-ui-state / debug-device-state) call these.
  std::string build_ui_state_json();
  std::string build_device_state_json();
  // Writes a self-framed binary blob to `out` (Serial): magic "MFSB",
  // fixed 24-byte header (format_version/pixel_format/rotation/
  // screen_id_len/width/height/frame_id/payload_size/crc32), then
  // payload_size bytes (screen_id followed by raw RGB565 pixel data). CRC32
  // is standard IEEE 802.3 (matches Python's zlib.crc32) computed over the
  // payload only, so the host can verify the transfer wasn't corrupted
  // without needing PNG-decodable framing on the wire. Deliberately NOT
  // base64 -- serial throughput is precious and base64 would add ~33%
  // overhead on top of an already-large raw framebuffer.
  void write_screenshot_serial(Stream& out);
  // JSON payloads are wrapped in a plain-text "##MFDBG-BEGIN <tag>##" /
  // "##MFDBG-END##" marker pair (one JSON object on the single line between
  // them) so a host reader can find them unambiguously even if a structured
  // log line happens to be interleaved right before the command's output.
  void write_ui_state_serial(Stream& out);
  void write_device_state_serial(Stream& out);
  // Same SCAN/KEY_DOWN/KEY_UP dispatch as POST /debug/input (see
  // build_input_event() below, shared by both), over serial -- needed to
  // drive real state transitions (WAIT_OPERATION/SESSION_ACTIVE/
  // QUANTITY_INPUT) for visual-parity testing when there's no HTTP path to
  // the device and no physical scanner/keypad access.
  void write_input_result_serial(const std::string& json_body, Stream& out);

 private:
  // Shared SCAN/KEY_DOWN/KEY_UP/TOUCH dispatch logic for both
  // POST /debug/input (HTTP) and write_input_result_serial() (serial) --
  // one parser, not two (see handle_input()'s own comment on why that
  // matters: a whitespace-handling bug here already bit this project once).
  // Returns true and fills out_event on success; on failure, fills
  // out_http_status/out_error_message and leaves out_event untouched.
  bool build_input_event(const std::string& body, kiosk::runtime::LocalEvent& out_event,
                         int& out_http_status, std::string& out_error_message);
  kiosk::runtime::EventBus& bus_;
  kiosk::hardware::Display& display_;
  kiosk::ui::Renderer& renderer_;
  kiosk::runtime::KioskRuntime& runtime_;
  kiosk::hardware::KeypadPcf8574& keypad_;
  const kiosk::hardware::SelfTestResult& selftest_;
  kiosk::runtime::BootDiagnostics& diagnostics_;
  kiosk::security::DeviceIdentity& identity_;
  kiosk::network::TimeSync& time_sync_;
  const kiosk::network::BootstrapClient& bootstrap_;
  const kiosk::storage::UiBundleStore& ui_bundle_store_;
  const kiosk::runtime::UiSyncController& ui_sync_;

  WebServer web_{MESFLOW_DEBUG_API_PORT};
  unsigned long last_screenshot_ms_ = 0;

  // --- Test-runner observability (pure diagnostics, never read by any
  // business logic) ---
  uint32_t input_seq_ = 0;      // bumped once per accepted /debug/input call
  bool qa_active_ = false;
  String qa_run_id_;
  String qa_step_;

  void handle_screenshot();
  void handle_ui_state();
  void handle_device_state();
  void handle_input();
  void handle_qa_session();
};

}  // namespace kiosk::debug

#endif  // MESFLOW_DEBUG_API
