#pragma once

#include <Arduino.h>

#include <string>

#include "../protocol/state_projection.h"

namespace kiosk::network {

// One-shot POST /api/kiosk/v2/bootstrap (§18/§19, docs/PROTOCOL.md). Phase 1
// only parses/stores enough for Phase 2 to build on -- it does not yet act
// on `desired`/`state` (no authoritative state machine exists yet).
enum class BootstrapStatus {
  NOT_ATTEMPTED,
  OK,
  REJECTED,     // backend responded but accepted:false, or unsupported protocol version
  FAILED,       // never got a response at all
};

const char* bootstrap_status_to_string(BootstrapStatus status);

struct BootstrapResult {
  BootstrapStatus status = BootstrapStatus::NOT_ATTEMPTED;
  uint32_t last_ok_uptime_ms = 0;
  uint32_t accepted_protocol_version = 0;
  String device_status;   // "ACTIVE"/etc as reported by the backend, if any
  String state_name;      // desired.state.name, if any (kept for /debug diagnostics)

  // Phase 2: the bootstrap response carries the SAME state{}/workflow{}/
  // view{} shape as /events and /state (docs/PROTOCOL.md) -- this is the
  // seed StateProjection applies as its very first snapshot each boot
  // (invariant 15: never restore business state locally, always start from
  // server authority). has_snapshot is false if the response didn't include
  // one at all (e.g. device_id was empty, or a malformed response).
  bool has_snapshot = false;
  kiosk::protocol::StateSnapshot snapshot;

  // Phase 4: desired.ui_bundle_version/hash -- what UI bundle the server
  // wants this device running. 0/"" means the server didn't say anything
  // (never treated as "matches" by ui_bundle_needs_sync()).
  uint32_t ui_bundle_version = 0;
  String ui_bundle_hash;

  // 2026-08-26 UX-hardening pass, §2/§3: server identity, added to
  // app/mesflow/web/kiosk_v2.py's /bootstrap response the same day. All
  // "" if the response predates this field (an older backend) -- callers
  // must treat "" as UNKNOWN, never assume a match. See
  // environment_label.h for the DEV/TEST/PROD/UNKNOWN mapping this feeds.
  String server_environment;  // raw settings.environment, e.g. "test"/"production"/"local_test"
  String server_role;         // raw settings.server_role, e.g. "DEV"/"PRODUCTION_TEST"/"PRODUCTION"/""
  String server_version;      // mesflow.__version__, e.g. "71.0.0.70"

  // 2026-08-26 physical field test, §11: real gap found live -- a REJECTED
  // bootstrap (e.g. a DISABLED/SUSPENDED kiosk identity, now correctly
  // rejected server-side with a real 403 + human message after the
  // matching app/mesflow/web/kiosk_v2.py fix) had NO message to show at
  // all; the device just silently stayed on the waiting screen forever.
  // "" if the response carried no message/error field to show (an older
  // backend, or the OTHER rejection cause -- unsupported protocol_version
  // -- which has no message field either).
  String reject_message;
};

// Real field report (2026-08-27, "máy quét phải luôn sẵn sàng" -- the kiosk
// must always be ready, intermittent unresponsiveness is unacceptable):
// this used to be "single attempt, blocking" -- a plain synchronous
// HTTPClient POST run directly on the main loop() thread, one of only two
// blocking calls left after the NetworkWorker migration (the other being
// per-call task creation, already fixed). Bootstrap re-fires on every WiFi
// reconnect (kiosk_runtime_v2.ino's own re-verification logic) -- including
// the pre-existing consecutive_tcp_connect_fail_ -> force_reconnect() self-
// heal path -- so a single degraded-network episode could block the
// keypad/scanner from being polled at all for multiple seconds per retry
// attempt. Split into two PURE, no-I/O halves so the .ino can drive the
// actual HTTP request through NetworkWorker (like every other request kind)
// instead of blocking here:
//   build_request_body() -- builds the JSON body, no network at all.
//   parse_response()     -- turns a completed NetworkWorker BOOTSTRAP
//                           result's (http_status, response_body) into a
//                           BootstrapResult, no network at all.
// Neither half touches WiFi/HTTPClient directly -- the .ino owns enqueueing
// via NetworkWorker::enqueue_bootstrap() and the retry/cooldown bookkeeping
// (unchanged in shape, just no longer blocking while it waits).
class BootstrapClient {
 public:
  static std::string build_request_body(const String& device_id, const String& hardware_id,
                                        const String& boot_id, uint32_t last_device_seq);

  // Returns the result (also cached, see last_result()). http_status is
  // NetworkResult::outcome.http_status from the completed BOOTSTRAP request
  // -- 0 (or negative, e.g. kResponseTooLargeMarker) means no usable
  // response was ever received, mapped the same way a direct HTTPClient
  // failure always was.
  BootstrapResult parse_response(int http_status, const std::string& response_body);

  const BootstrapResult& last_result() const { return last_result_; }

 private:
  BootstrapResult last_result_;
};

}  // namespace kiosk::network
