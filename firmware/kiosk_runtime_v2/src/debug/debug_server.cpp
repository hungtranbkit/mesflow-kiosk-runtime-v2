#include "debug_server.h"

#if MESFLOW_DEBUG_API

#include <WiFi.h>

#include <algorithm>
#include <string>

#include "../config/build_info.h"
#include "../health/memory_diag.h"
#include "../health/structured_log.h"
#include "../protocol/json_extract.h"     // whitespace-tolerant field extraction
#include "../protocol/protocol_codec.h"  // reuse json_escape
#include "../runtime/status_snapshot.h"

namespace kiosk::debug {

namespace {

// Screenshot binary format (deliberately NOT PNG -- §42: don't get stuck on
// on-device image encoding; a host script converts this to PNG). All
// multi-byte fields little-endian (native ESP32 byte order, no conversion
// needed on-device).
//
//   offset  0  magic "MFSC" (4 bytes)
//   offset  4  format version (uint8) = 1
//   offset  5  pixel_format (uint8) = 0 (RGB565)
//   offset  6  rotation (uint8, Adafruit_GFX rotation value 0-3)
//   offset  7  screen_id_len (uint8)
//   offset  8  width (uint16 LE)
//   offset 10  height (uint16 LE)
//   offset 12  frame_id (uint32 LE)
//   offset 16  screen_id bytes (screen_id_len bytes, ASCII, not null-terminated)
//   offset 16+screen_id_len   raw RGB565 pixel data, row-major, width*height*2 bytes
void write_u16le(uint8_t* buf, uint16_t v) {
  buf[0] = v & 0xFF;
  buf[1] = (v >> 8) & 0xFF;
}
void write_u32le(uint8_t* buf, uint32_t v) {
  buf[0] = v & 0xFF;
  buf[1] = (v >> 8) & 0xFF;
  buf[2] = (v >> 16) & 0xFF;
  buf[3] = (v >> 24) & 0xFF;
}

// Standard IEEE 802.3 CRC32 (reflected, poly 0xEDB88320, init/final
// 0xFFFFFFFF) -- bitwise, no table, since this only ever runs once per
// operator-triggered debug capture (not a hot path). Produces the exact same
// value as Python's zlib.crc32()/binascii.crc32(), which the host-side
// capture tool uses to verify the transfer.
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
  }
  return crc;
}

}  // namespace

void DebugServer::begin() {
  web_.on("/debug/screenshot", HTTP_GET, [this]() { handle_screenshot(); });
  web_.on("/debug/ui-state", HTTP_GET, [this]() { handle_ui_state(); });
  web_.on("/debug/device-state", HTTP_GET, [this]() { handle_device_state(); });
  web_.on("/debug/input", HTTP_POST, [this]() { handle_input(); });
  web_.on("/debug/qa-session", HTTP_POST, [this]() { handle_qa_session(); });
  web_.begin();
  kiosk::health::log_structured("INFO", "DEBUG_API_STARTED", "debug_server",
                                 "MESFLOW_DEBUG_API is ON -- DEV/LAB profile only, see docs/VISUAL_DEBUG.md");
}

void DebugServer::poll() {
  if (paused_) return;
  web_.handleClient();
}

void DebugServer::set_paused(bool paused) {
  if (paused == paused_) return;
  paused_ = paused;
  if (paused_) {
    // Actually release the socket (not just skip handleClient()) -- a
    // client on the open AP shouldn't even be able to complete a TCP
    // connection to this port while paused.
    web_.close();
    kiosk::health::log_structured("INFO", "DEBUG_API_PAUSED", "debug_server",
                                   "setup AP is open (no password) -- debug HTTP API suspended for its duration");
  } else {
    web_.begin();
    kiosk::health::log_structured("INFO", "DEBUG_API_RESUMED", "debug_server",
                                   "setup AP closed -- debug HTTP API back up");
  }
}

void DebugServer::handle_screenshot() {
  unsigned long now = millis();
  if (now - last_screenshot_ms_ < MESFLOW_DEBUG_SCREENSHOT_MIN_INTERVAL_MS) {
    kiosk::health::log_structured("WARN", "DEBUG_RATE_LIMITED", "debug_server",
                                   "/debug/screenshot called too frequently");
    web_.send(429, "text/plain", "DEBUG_RATE_LIMITED: max ~2 captures/sec");
    return;
  }
  last_screenshot_ms_ = now;

  if (!display_.framebuffer_ok()) {
    kiosk::health::log_structured("ERROR", "DEBUG_SCREENSHOT_FAIL", "debug_server",
                                   "no shadow framebuffer (PSRAM allocation failed at boot)");
    web_.send(503, "text/plain", "DEBUG_SCREENSHOT_FAIL: no framebuffer");
    return;
  }

  String screen_id = renderer_.current_screen_id();
  uint8_t screen_id_len = static_cast<uint8_t>(std::min<size_t>(screen_id.length(), 255));
  uint16_t w = display_.width();
  uint16_t h = display_.height();
  size_t pixel_bytes = display_.framebuffer_bytes();

  uint8_t header[16];
  header[0] = 'M'; header[1] = 'F'; header[2] = 'S'; header[3] = 'C';
  header[4] = 1;                        // format version
  header[5] = 0;                        // pixel_format: RGB565
  header[6] = display_.getRotation();
  header[7] = screen_id_len;
  write_u16le(header + 8, w);
  write_u16le(header + 10, h);
  write_u32le(header + 12, display_.frame_id());

  size_t total = sizeof(header) + screen_id_len + pixel_bytes;
  web_.setContentLength(total);
  web_.send(200, "application/octet-stream", "");
  web_.sendContent(reinterpret_cast<const char*>(header), sizeof(header));
  if (screen_id_len > 0) {
    web_.sendContent(screen_id.c_str(), screen_id_len);
  }
  web_.sendContent(reinterpret_cast<const char*>(display_.framebuffer()), pixel_bytes);
  kiosk::health::log_memory_snapshot("AFTER_HTTP_DEBUG_REQUEST");
}

std::string DebugServer::build_ui_state_json() {
  std::string json = "{";
  json += "\"screen_id\":\"" + kiosk::protocol::json_escape(renderer_.current_screen_id().c_str()) + "\",";
  json += "\"frame_id\":" + std::to_string(display_.frame_id()) + ",";
  json += "\"display\":{\"width\":" + std::to_string(display_.width()) +
          ",\"height\":" + std::to_string(display_.height()) +
          ",\"rotation\":" + std::to_string(display_.getRotation()) + "},";
  json += "\"runtime\":{\"firmware_version\":\"" KIOSK_RUNTIME_VERSION "\",";
  json += std::string("\"build_id\":\"") + KIOSK_BUILD_ID + "\"},";

  // Honest, minimal component model (Phase 0 has no rect/font schema --
  // that's Phase 4's bundle renderer, docs/UI_SCHEMA.md). Each entry is
  // exactly one text row this renderer actually drew.
  json += "\"lines\":[";
  bool first = true;
  const auto* lines = renderer_.lines();
  for (int i = 0; i < kiosk::ui::Renderer::kMaxLines; ++i) {
    if (!lines[i].used) continue;
    if (!first) json += ",";
    first = false;
    json += "{\"row\":" + std::to_string(i) + ",";
    json += "\"text\":\"" + kiosk::protocol::json_escape(lines[i].text.c_str()) + "\",";
    json += "\"color565\":" + std::to_string(lines[i].color) + ",";
    json += "\"measured_w\":" + std::to_string(lines[i].measured_w) + ",";
    json += std::string("\"overflow\":") + (lines[i].overflow ? "true" : "false") + "}";
  }
  json += "],";

  // Phase 4 stabilization: true-geometry components (bundle-driven TEXT
  // drawn at their real x/y/font_size, see Renderer::emit_component_text())
  // -- empty for hardcoded (non-bundle) screens, which only ever populate
  // "lines" above. This is what UI-007's TRUE XY regression checks against.
  json += "\"components\":[";
  first = true;
  const auto* components = renderer_.components();
  for (int i = 0; i < kiosk::ui::Renderer::kMaxComponents; ++i) {
    if (!components[i].used) continue;
    if (!first) json += ",";
    first = false;
    json += "{\"x\":" + std::to_string(components[i].x) + ",\"y\":" + std::to_string(components[i].y) + ",";
    json += "\"w\":" + std::to_string(components[i].w) + ",\"h\":" + std::to_string(components[i].h) + ",";
    json += "\"font_size\":" + std::to_string(components[i].font_size) + ",";
    json += "\"text\":\"" + kiosk::protocol::json_escape(components[i].text.c_str()) + "\",";
    json += "\"color565\":" + std::to_string(components[i].color) + ",";
    json += "\"measured_w\":" + std::to_string(components[i].measured_w) + ",";
    json += std::string("\"overflow\":") + (components[i].overflow ? "true" : "false") + "}";
  }
  json += "],";

  // Phase 4: which UI bundle (if any) produced this frame. 0 means the
  // current screen came from a built-in hardcoded screen (no active bundle,
  // or the active bundle has no entry for this screen_id) -- see
  // KioskRuntime::render_current_business_state()'s bundle-then-fallback
  // order.
  json += "\"layout_id\":\"" + kiosk::protocol::json_escape(renderer_.current_screen_id().c_str()) + "\",";
  json += "\"ui_bundle_version\":" +
          std::to_string(ui_bundle_store_.has_active_bundle() ? ui_bundle_store_.active_version() : 0);
  json += "}";
  return json;
}

void DebugServer::handle_ui_state() {
  web_.send(200, "application/json", build_ui_state_json().c_str());
}

std::string DebugServer::build_device_state_json() {
  // Single source of truth shared with the heartbeat POST body
  // (runtime/status_snapshot.*) -- also adds framebuffer_bytes here since
  // that's a debug/screenshot-specific detail heartbeat doesn't need.
  std::string json =
      kiosk::runtime::build_status_json(identity_, time_sync_, runtime_, keypad_, selftest_,
                                        diagnostics_, bootstrap_, wifi_);
  // Splice in framebuffer_bytes/qa/last_input_seq without a second JSON
  // parser: append as sibling top-level fields (simpler and still
  // perfectly valid/parseable JSON for consumers).
  json.insert(json.size() - 1,
              ",\"framebuffer_bytes\":" + std::to_string(display_.framebuffer_bytes()));

  // Test-runner observability (§6/§7 of the E2E runner observability fix):
  // pure diagnostics, read by nothing business-relevant.
  std::string qa_json = ",\"qa\":{\"active\":" + std::string(qa_active_ ? "true" : "false");
  if (qa_active_) {
    qa_json += ",\"run_id\":\"" + kiosk::protocol::json_escape(qa_run_id_.c_str()) + "\"";
    qa_json += ",\"step\":\"" + kiosk::protocol::json_escape(qa_step_.c_str()) + "\"";
  }
  qa_json += "}";
  qa_json += ",\"last_input_seq\":" + std::to_string(input_seq_);
  json.insert(json.size() - 1, qa_json);

  // Phase 4: UI bundle sync state, spliced in the same way (see qa_json
  // above) -- pure observability, read by the E2E runner's UI-00x cases and
  // by nothing on the business-logic path.
  std::string ui_json = ",\"ui\":{";
  ui_json += "\"active_version\":" + std::to_string(ui_bundle_store_.active_version()) + ",";
  ui_json += "\"active_hash\":\"" + kiosk::protocol::json_escape(ui_bundle_store_.active_hash().c_str()) + "\",";
  ui_json += "\"schema_version\":" + std::to_string(ui_bundle_store_.active_schema_version()) + ",";
  ui_json += std::string("\"active_slot\":\"") + ui_bundle_store_.active_slot() + "\",";
  ui_json += "\"last_good_version\":" + std::to_string(ui_bundle_store_.last_good_version()) + ",";
  ui_json += "\"desired_version\":" + std::to_string(ui_sync_.last_desired_version()) + ",";
  ui_json += "\"sync_state\":\"" +
             std::string(kiosk::storage::ui_sync_state_to_string(ui_bundle_store_.sync_state())) + "\",";
  ui_json += "\"last_update_error\":\"" +
             kiosk::protocol::json_escape(ui_bundle_store_.last_update_error().c_str()) + "\"";
  ui_json += "}";
  json.insert(json.size() - 1, ui_json);

  return json;
}

void DebugServer::handle_device_state() {
  web_.send(200, "application/json", build_device_state_json().c_str());
}

void DebugServer::write_screenshot_serial(Stream& out) {
  if (!display_.framebuffer_ok()) {
    // No framing at all on failure -- deliberately fails loud/obvious on the
    // host side (it won't find the "MFSB" magic and will report a clear
    // error) rather than sending a superficially well-formed empty capture.
    out.println("{\"level\":\"ERROR\",\"code\":\"DEBUG_SCREENSHOT_FAIL\",\"module\":\"debug_server\","
                "\"message\":\"no shadow framebuffer (PSRAM allocation failed at boot)\"}");
    return;
  }

  String screen_id = renderer_.current_screen_id();
  uint8_t screen_id_len = static_cast<uint8_t>(std::min<size_t>(screen_id.length(), 255));
  uint16_t w = display_.width();
  uint16_t h = display_.height();
  size_t pixel_bytes = display_.framebuffer_bytes();
  uint32_t payload_size = static_cast<uint32_t>(screen_id_len) + static_cast<uint32_t>(pixel_bytes);

  const uint8_t* fb_bytes = reinterpret_cast<const uint8_t*>(display_.framebuffer());
  uint32_t crc = 0xFFFFFFFFu;
  crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(screen_id.c_str()), screen_id_len);
  crc = crc32_update(crc, fb_bytes, pixel_bytes);
  crc = ~crc;

  uint8_t header[24];
  header[0] = 'M'; header[1] = 'F'; header[2] = 'S'; header[3] = 'B';
  header[4] = 1;                        // format version
  header[5] = 0;                        // pixel_format: RGB565
  header[6] = display_.getRotation();
  header[7] = screen_id_len;
  write_u16le(header + 8, w);
  write_u16le(header + 10, h);
  write_u32le(header + 12, display_.frame_id());
  write_u32le(header + 16, payload_size);
  write_u32le(header + 20, crc);

  out.write(header, sizeof(header));
  if (screen_id_len > 0) {
    out.write(reinterpret_cast<const uint8_t*>(screen_id.c_str()), screen_id_len);
  }
  out.write(fb_bytes, pixel_bytes);
  out.flush();
  kiosk::health::log_memory_snapshot("AFTER_SERIAL_SCREENSHOT");
}

void DebugServer::write_ui_state_serial(Stream& out) {
  out.println("##MFDBG-BEGIN ui-state##");
  out.println(build_ui_state_json().c_str());
  out.println("##MFDBG-END##");
}

void DebugServer::write_device_state_serial(Stream& out) {
  out.println("##MFDBG-BEGIN device-state##");
  out.println(build_device_state_json().c_str());
  out.println("##MFDBG-END##");
}

void DebugServer::handle_qa_session() {
  std::string body(web_.arg("plain").c_str());
  bool has_run_id = kiosk::protocol::json_has_key(body, "run_id");
  bool explicit_inactive = kiosk::protocol::json_has_key(body, "active") &&
                           !kiosk::protocol::json_extract_bool(body, "active", true);

  if (!has_run_id || explicit_inactive) {
    qa_active_ = false;
    qa_run_id_ = "";
    qa_step_ = "";
    renderer_.set_qa_active(false);
    kiosk::health::log_structured("INFO", "QA_SESSION_ENDED", "debug_server", "");
    web_.send(200, "application/json", "{\"active\":false}");
    return;
  }

  qa_active_ = true;
  qa_run_id_ = kiosk::protocol::json_extract_string(body, "run_id").c_str();
  qa_step_ = kiosk::protocol::json_extract_string(body, "step").c_str();
  renderer_.set_qa_active(true);
  kiosk::health::log_structured("INFO", "QA_SESSION_ACTIVE", "debug_server",
                                (std::string("run_id=") + qa_run_id_.c_str() +
                                 " step=" + qa_step_.c_str())
                                    .c_str());
  std::string resp = "{\"active\":true,\"run_id\":\"" + kiosk::protocol::json_escape(qa_run_id_.c_str()) +
                     "\",\"step\":\"" + kiosk::protocol::json_escape(qa_step_.c_str()) + "\"}";
  web_.send(200, "application/json", resp.c_str());
}

bool DebugServer::build_input_event(const std::string& body, kiosk::runtime::LocalEvent& out_event,
                                    int& out_http_status, std::string& out_error_message) {
  // A REAL bug caught by the Kiosk E2E test runner (tools/kiosk_e2e_runner.py):
  // this used to have its own hand-rolled extractor doing an exact,
  // no-whitespace substring match (`"type":"SCAN"`) -- the SAME class of bug
  // already found and fixed once in bootstrap_client.cpp. Standard JSON
  // encoders (Python's json.dumps() among them) emit `"type": "SCAN"` WITH a
  // space after the colon by default; every test-runner-injected /debug/input
  // call was silently rejected as "unknown type" until this was fixed. Manual
  // curl testing during Phase 1/2 never caught this because it happened to
  // always be typed with no space. Reusing the shared, whitespace-tolerant
  // json_extract_string (already host-tested) instead of a second buggy
  // hand-rolled parser. Shared by both POST /debug/input (HTTP) and
  // write_input_result_serial() (serial) -- one parser, not two.
  auto extract = [&](const char* field) -> String {
    return kiosk::protocol::json_extract_string(body, field).c_str();
  };

  String type = extract("type");
  out_event.timestamp_ms = millis();

  if (type == "SCAN") {
    String value = extract("value");
    if (value.length() == 0) {
      out_http_status = 400;
      out_error_message = "SCAN requires \"value\"";
      return false;
    }
    out_event.kind = kiosk::runtime::LocalEventKind::SCAN;
    out_event.text = value;
    return true;
  }
  if (type == "KEY_DOWN" || type == "KEY_UP") {
    String key = extract("key");
    if (key.length() == 0) {
      out_http_status = 400;
      out_error_message = "KEY_DOWN/KEY_UP requires \"key\"";
      return false;
    }
    out_event.kind = type == "KEY_DOWN" ? kiosk::runtime::LocalEventKind::KEY_DOWN
                                       : kiosk::runtime::LocalEventKind::KEY_UP;
    out_event.key = key[0];
    return true;
  }
  if (type == "TOUCH") {
    // §13/docs/HARDWARE.md: touch has no driver in Phase 0 at all -- honest
    // rejection, not a silent no-op pretending touch input was accepted.
    out_http_status = 501;
    out_error_message = "TOUCH not implemented in Phase 0 (no touch driver)";
    return false;
  }
  out_http_status = 400;
  out_error_message = "unknown type (expected SCAN/KEY_DOWN/KEY_UP)";
  return false;
}

void DebugServer::handle_input() {
  std::string body(web_.arg("plain").c_str());
  kiosk::runtime::LocalEvent event;
  int http_status = 400;
  std::string error_message;

  if (!build_input_event(body, event, http_status, error_message)) {
    kiosk::health::log_structured("WARN", "DEBUG_INPUT_REJECTED", "debug_server", error_message.c_str());
    web_.send(http_status, "text/plain", ("DEBUG_INPUT_REJECTED: " + error_message).c_str());
    return;
  }

  // Goes through the SAME EventBus real hardware publishes to (§12/§13) --
  // this is not a shortcut into renderer/UI functions directly.
  ++input_seq_;  // bumped BEFORE publish so last_input_seq is guaranteed to
                 // already reflect this call by the time device-state is
                 // fetched afterward (§8 of the E2E runner observability fix:
                 // a test runner can confirm THIS specific input landed
                 // before waiting on any business-level effect).
  bus_.publish(event);
  std::string resp = "{\"accepted\":true,\"input_seq\":" + std::to_string(input_seq_) + "}";
  web_.send(200, "application/json", resp.c_str());
}

void DebugServer::write_input_result_serial(const std::string& json_body, Stream& out) {
  kiosk::runtime::LocalEvent event;
  int http_status = 400;
  std::string error_message;

  out.println("##MFDBG-BEGIN debug-input##");
  if (!build_input_event(json_body, event, http_status, error_message)) {
    kiosk::health::log_structured("WARN", "DEBUG_INPUT_REJECTED", "debug_server", error_message.c_str());
    std::string resp = "{\"accepted\":false,\"http_status\":" + std::to_string(http_status) +
                       ",\"error\":\"" + kiosk::protocol::json_escape(error_message) + "\"}";
    out.println(resp.c_str());
  } else {
    ++input_seq_;
    bus_.publish(event);
    std::string resp = "{\"accepted\":true,\"input_seq\":" + std::to_string(input_seq_) + "}";
    out.println(resp.c_str());
  }
  out.println("##MFDBG-END##");
}

}  // namespace kiosk::debug

#endif  // MESFLOW_DEBUG_API
