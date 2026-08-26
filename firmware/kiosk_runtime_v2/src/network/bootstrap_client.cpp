#include "bootstrap_client.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include "../config/build_info.h"
#include "../config/hardware_pins.h"
#include "../config/runtime_config.h"
#include "../health/structured_log.h"
#include "../protocol/json_extract.h"    // whitespace-tolerant field extraction
#include "../protocol/protocol_codec.h"  // json_escape
#include "../protocol/retry_policy.h"    // kResponseTooLargeMarker
#include "endpoint_utils.h"

namespace kiosk::network {

const char* bootstrap_status_to_string(BootstrapStatus status) {
  switch (status) {
    case BootstrapStatus::NOT_ATTEMPTED: return "NOT_ATTEMPTED";
    case BootstrapStatus::OK: return "OK";
    case BootstrapStatus::REJECTED: return "REJECTED";
    case BootstrapStatus::FAILED: return "FAILED";
  }
  return "UNKNOWN";
}

BootstrapResult BootstrapClient::attempt(const String& events_url, const String& device_id,
                                         const String& hardware_id, const String& boot_id,
                                         uint32_t last_device_seq) {
  BootstrapResult result;

  if (WiFi.status() != WL_CONNECTED || events_url.length() == 0) {
    result.status = BootstrapStatus::FAILED;
    last_result_ = result;
    kiosk::health::log_structured("WARN", "BOOTSTRAP_REJECTED", "bootstrap_client",
                                   "no wifi or no backend configured");
    return result;
  }

  // A REAL bug caught live (Phase 2): this used to POST directly to
  // `events_url` (the stored /events endpoint) instead of deriving the
  // /bootstrap sibling endpoint. Every bootstrap request silently landed on
  // the mock backend's /events handler, which correctly rejected it (a
  // bootstrap body has no top-level protocol_version field) and replied
  // with a real HTTP 200 body containing "accepted": false -- indistinguish-
  // able, from this client's point of view, from a genuine bootstrap
  // rejection. heartbeat_client.cpp already derived its own sibling
  // endpoint correctly; bootstrap_client just never did the same swap.
  String url = derive_sibling_endpoint(events_url, "bootstrap");
  if (url.length() == 0) {
    result.status = BootstrapStatus::FAILED;
    last_result_ = result;
    kiosk::health::log_structured("ERROR", "BOOTSTRAP_REJECTED", "bootstrap_client",
                                   "could not derive /bootstrap endpoint from configured URL");
    return result;
  }

  std::string body = "{";
  body += "\"device_id\":\"" + kiosk::protocol::json_escape(device_id.c_str()) + "\",";
  body += "\"hardware_id\":\"" + kiosk::protocol::json_escape(hardware_id.c_str()) + "\",";
  body += "\"boot_id\":\"" + kiosk::protocol::json_escape(boot_id.c_str()) + "\",";
  body += "\"runtime\":{\"version\":\"" KIOSK_RUNTIME_VERSION "\",\"protocol_version\":1},";
  body += "\"hardware\":{\"chip\":\"ESP32-S3\",\"flash_mb\":16,\"psram_mb\":8,";
  body += "\"display\":\"ILI9341\",\"scanner\":\"GM65\",\"keypad\":\"PCF8574\"},";
  body += "\"current\":{\"ui_bundle\":0,\"workflow\":0,\"state_version\":0,\"last_device_seq\":";
  body += std::to_string(last_device_seq);
  body += "}}";

  HTTPClient http;
  http.setTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    result.status = BootstrapStatus::FAILED;
    last_result_ = result;
    kiosk::health::log_structured("ERROR", "BOOTSTRAP_REJECTED", "bootstrap_client",
                                   "HTTPClient::begin failed");
    return result;
  }
  http.addHeader("Content-Type", "application/json");

  int status = http.POST(body.c_str());
  // Response-size guard (2026-08-26): see RUNTIME_MAX_RESPONSE_BODY_BYTES'
  // own doc comment -- reject on Content-Length alone, before ever calling
  // getString(), rather than let a pathological response allocate an
  // unbounded String.
  std::string response;
  if (status > 0) {
    int content_length = http.getSize();
    if (content_length < 0 || content_length > RUNTIME_MAX_RESPONSE_BODY_BYTES) {
      kiosk::health::log_structured(
          "ERROR", "BOOTSTRAP_RESPONSE_TOO_LARGE", "bootstrap_client",
          (std::string("content_length=") + std::to_string(content_length)).c_str());
      status = kiosk::protocol::kResponseTooLargeMarker;
    } else {
      response = std::string(http.getString().c_str());
    }
  }
  http.end();

  if (status <= 0) {
    result.status = BootstrapStatus::FAILED;
    kiosk::health::log_structured("WARN", "BOOTSTRAP_REJECTED", "bootstrap_client",
                                   "no response from backend");
    last_result_ = result;
    return result;
  }

  // A REAL bug caught live (Phase 2): the previous hand-rolled extractor
  // here did an exact substring match for `"accepted":true` with no space,
  // but Python's json.dumps() default output is `"accepted": true` WITH a
  // space after the colon -- every response from mock_backend.py silently
  // failed to parse as accepted, even on a real 200 OK. kiosk::protocol::
  // json_extract_* (already used by state_projection/event_response,
  // host-tested) skips whitespace correctly; reusing it here instead of
  // maintaining a second, buggier parser.
  bool accepted = kiosk::protocol::json_extract_bool(response, "accepted", false);
  std::string protocol_obj = kiosk::protocol::json_extract_object(response, "protocol");
  int64_t accepted_version =
      protocol_obj.empty() ? 0 : kiosk::protocol::json_extract_int(protocol_obj, "accepted_version", 0);

  if (!accepted || (accepted_version != 0 && accepted_version != 1)) {
    result.status = BootstrapStatus::REJECTED;
    result.accepted_protocol_version = static_cast<uint32_t>(accepted_version);
    // §11 of the 2026-08-26 physical field test: a device-disabled
    // rejection (app/mesflow/web/kiosk_v2.py's real HTTP 403, body shape
    // {"error":"FORBIDDEN","message":"...","ok":false}) has a top-level
    // "message" string -- not nested under "error" the way /events'
    // business-rejection shape is. "" if absent (an older backend, or the
    // OTHER rejection cause with no message field -- unsupported
    // protocol_version).
    result.reject_message = kiosk::protocol::json_extract_string(response, "message").c_str();
    kiosk::health::log_structured("WARN", "BOOTSTRAP_REJECTED", "bootstrap_client",
                                   accepted ? "unsupported protocol_version" : "accepted:false");
    last_result_ = result;
    return result;
  }

  result.status = BootstrapStatus::OK;
  result.last_ok_uptime_ms = millis();
  result.accepted_protocol_version = accepted_version != 0 ? static_cast<uint32_t>(accepted_version) : 1;
  result.device_status = kiosk::protocol::json_extract_string(response, "device_status").c_str();
  std::string state_obj = kiosk::protocol::json_extract_object(response, "state");
  result.state_name = state_obj.empty() ? "" : kiosk::protocol::json_extract_string(state_obj, "name").c_str();

  // Same state{}/workflow{}/view{} shape as /events and /state -- seed
  // StateProjection's very first snapshot from THIS response so the device
  // never has to invent a starting business state locally (invariant 15).
  result.has_snapshot = kiosk::protocol::parse_state_snapshot_json(response, result.snapshot);

  // Phase 4: desired.ui_bundle_version/hash -- read once at boot (heartbeat
  // could also carry this for faster propagation between reboots; not
  // implemented yet, see the Phase 4 report's Known Gaps).
  std::string desired_obj = kiosk::protocol::json_extract_object(response, "desired");
  if (!desired_obj.empty()) {
    result.ui_bundle_version =
        static_cast<uint32_t>(kiosk::protocol::json_extract_int(desired_obj, "ui_bundle_version", 0));
    result.ui_bundle_hash = kiosk::protocol::json_extract_string(desired_obj, "ui_bundle_hash").c_str();
  }

  // §2/§3 of the 2026-08-26 UX-hardening pass: top-level fields, not nested
  // under desired{}/state{} -- json_extract_string() on the raw response
  // handles a JSON null (settings.server_role can be None) the same as a
  // missing key, both come back "" here, which is exactly the honest
  // "server didn't say" value environment_label.h maps to UNKNOWN.
  result.server_environment = kiosk::protocol::json_extract_string(response, "environment").c_str();
  result.server_role = kiosk::protocol::json_extract_string(response, "server_role").c_str();
  result.server_version = kiosk::protocol::json_extract_string(response, "version").c_str();

  kiosk::health::log_structured("INFO", "BOOTSTRAP_OK", "bootstrap_client",
                                 result.device_status.c_str());

  last_result_ = result;
  return result;
}

}  // namespace kiosk::network
