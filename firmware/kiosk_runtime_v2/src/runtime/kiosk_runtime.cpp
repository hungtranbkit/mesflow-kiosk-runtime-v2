#include "kiosk_runtime.h"

#include <cstdlib>

#include "../health/structured_log.h"
#include "../network/endpoint_utils.h"
#include "../protocol/protocol_codec.h"

namespace kiosk::runtime {

namespace {
using kiosk::security::ProvisioningState;

// §5: maps a non-ACTIVE provisioning state to the taxonomy error code
// (docs/PROTOCOL.md) used when a scan is rejected without ever contacting
// the backend.
const char* identity_error_code(ProvisioningState state) {
  switch (state) {
    case ProvisioningState::UNPROVISIONED:
    case ProvisioningState::PROVISIONING:
      return "IDENTITY_NOT_PROVISIONED";
    case ProvisioningState::SUSPENDED:
      return "IDENTITY_INVALID";
    case ProvisioningState::REVOKED:
      return "IDENTITY_REVOKED";
    case ProvisioningState::ACTIVE:
      return "";
  }
  return "IDENTITY_INVALID";
}
}  // namespace

void KioskRuntime::begin(const String& boot_id) {
  // Must happen here (called from setup(), after Arduino's own NVS init),
  // NOT in DeviceSequence's constructor -- device_seq_ is a member of this
  // globally-constructed object, and global C++ static initializers run
  // before nvs_flash_init(). See ids.h's doc comment for the real bug this
  // fixes (found on real hardware: device_seq reset to 1 every reboot).
  device_seq_.init();

  device_id_ = identity_.device_id();
  hardware_id_ = identity_.hardware_id();
  boot_id_ = boot_id;
  api_endpoint_ = config_.api_endpoint();

  bus_.subscribe([this](const LocalEvent& event) { handle_local_event(event); });

  refresh_idle_screen();

  kiosk::health::log_structured("INFO", "STATE_READY_LOCAL", "kiosk_runtime",
                                 "BOOT -> READY_LOCAL (placeholder state, no backend yet)");
}

void KioskRuntime::refresh_idle_screen() {
  render_current_business_state();
}

void KioskRuntime::on_bootstrap_result(const kiosk::network::BootstrapResult& result) {
  if (result.status != kiosk::network::BootstrapStatus::OK) return;

  if (!result.has_snapshot) {
    kiosk::health::log_structured("WARN", "STATE_SYNC_FAIL", "kiosk_runtime",
                                   "bootstrap OK but response carried no state{} snapshot");
    return;
  }

  auto apply_result = state_projection_.apply(result.snapshot);
  kiosk::health::log_structured(
      "INFO", "STATE_APPLY", "kiosk_runtime",
      (std::string("source=BOOTSTRAP apply=") + kiosk::protocol::apply_result_to_string(apply_result) +
       " state=" + kiosk::protocol::business_state_to_string(result.snapshot.state) +
       " version=" + std::to_string(result.snapshot.state_version))
          .c_str());

  if (apply_result == kiosk::protocol::ApplyResult::APPLIED ||
      apply_result == kiosk::protocol::ApplyResult::APPLIED_IDENTICAL) {
    refresh_idle_screen();
  }
  // else (STALE/INCONSISTENT/UNSUPPORTED): a bootstrap snapshot failing to
  // apply as the very first-ever snapshot this boot would be a genuinely
  // strange server bug (there's nothing "stale" to be older than) -- leave
  // the pre-bootstrap waiting screen up rather than pretend anything.
}

// §4/invariant 13: RESYNCING is a transient LOCAL condition, not one of the
// 6 canonical business states -- StateProjection never holds it.
void KioskRuntime::render_current_business_state(const String& transient_message, bool is_error,
                                                  bool is_network_error) {
  ProvisioningState state = identity_.state();
  if (state != ProvisioningState::ACTIVE) {
    renderer_.draw_identity_screen(state, hardware_id_, wifi_indicator_);
    return;
  }
  if (resyncing_) {
    renderer_.draw_resyncing_screen(wifi_indicator_);
    return;
  }
  if (!state_projection_.has_snapshot()) {
    // Pre-bootstrap (or bootstrap not yet OK this boot) -- no server
    // authority to render yet. Known minor gap: transient_message is not
    // shown in this fallback (draw_waiting_screen has no message slot);
    // it is still captured in the structured log by the caller.
    renderer_.draw_waiting_screen(wifi_indicator_);
    return;
  }

  // Phase 4.1: an actual error (business rejection or network/backend
  // failure) takes over the WHOLE screen via draw_error_view() -- a real,
  // full-screen presentation instead of one line squeezed into the normal
  // layout, matching legacy's own visual weight for errors. The
  // authoritative business state underneath is untouched (still whatever
  // state_projection_ holds); the NEXT normal render call (any state
  // change, resync, or new input) naturally returns to it -- same dismiss
  // mechanism transient_message already used, no new timing/state machine
  // invented.
  if (is_error && transient_message.length() > 0) {
    renderer_.draw_error_view(transient_message, is_network_error, wifi_indicator_);
    return;
  }

  const kiosk::protocol::StateSnapshot& snap = state_projection_.current();

  // GOOD/DEFECT/REWORK quantity flow: DEFECT/REWORK_DECISION/REWORK are
  // LOCAL sub-steps the UI bundle schema has no concept of (a bundle screen
  // is keyed by server business state only) -- always hardcoded, bundle or
  // no bundle, same giant-digit visual style the GOOD step already uses.
  // GOOD itself still goes through the normal bundle-first path below
  // unchanged (screen_id "state_quantity_input" already exists in the
  // active bundle).
  if (snap.state == kiosk::protocol::BusinessState::QUANTITY_INPUT && qty_step_ != QtyStep::GOOD) {
    switch (qty_step_) {
      case QtyStep::DEFECT:
        renderer_.draw_quantity_defect_screen(snap.view, local_qty_buffer_.c_str(), qty_good_,
                                              transient_message, is_error, wifi_indicator_);
        break;
      case QtyStep::REWORK_DECISION:
        renderer_.draw_rework_decision_screen(snap.view, qty_good_, qty_defect_, transient_message,
                                              is_error, wifi_indicator_);
        break;
      case QtyStep::REWORK:
        renderer_.draw_quantity_rework_screen(snap.view, local_qty_buffer_.c_str(), qty_defect_,
                                              transient_message, is_error, wifi_indicator_);
        break;
      default:
        break;  // GOOD handled by the normal path below; unreachable here
    }
    return;
  }

  // Phase 4: prefer the active UI bundle if one is installed and it
  // actually defines a screen for this state; otherwise fall back to the
  // built-in hardcoded screens (§16 "factory default" -- a brand-new
  // device with no bundle yet, or a bundle that simply doesn't cover this
  // state, must still render SOMETHING correct, never a blank screen).
  if (ui_bundle_store_.has_active_bundle()) {
    const char* screen_id = kiosk::protocol::screen_id_for_business_state(snap.state);
    const kiosk::protocol::UiScreen* screen =
        kiosk::protocol::find_ui_screen(ui_bundle_store_.active_bundle(), screen_id);
    if (screen != nullptr) {
      renderer_.draw_from_bundle(*screen, snap.view, local_qty_buffer_.c_str(), transient_message, is_error,
                                 wifi_indicator_);
      return;
    }
  }

  if (snap.state == kiosk::protocol::BusinessState::QUANTITY_INPUT) {
    renderer_.draw_quantity_input_screen(snap.view, local_qty_buffer_.c_str(), transient_message,
                                         is_error, wifi_indicator_);
  } else {
    renderer_.draw_business_state(snap, transient_message, is_error, wifi_indicator_);
  }
}

String KioskRuntime::state_endpoint_url() const {
  String base = kiosk::network::derive_sibling_endpoint(api_endpoint_, "state");
  if (base.length() == 0) return "";
  return base + "?device_id=" + device_id_;
}

void KioskRuntime::start_resync() {
  resyncing_ = true;
  renderer_.draw_resyncing_screen(wifi_indicator_);
  String url = state_endpoint_url();
  bool started = state_fetcher_.fetch(url);
  if (!started) {
    // Already busy with a fetch in flight -- fine, that one's completion
    // will be handled by handle_resync_result(); nothing to do here beyond
    // having already shown the resyncing screen above.
    kiosk::health::log_structured("INFO", "STATE_RESYNC_REQUIRED", "kiosk_runtime",
                                   "resync requested while a fetch was already in flight");
  } else {
    kiosk::health::log_structured("WARN", "STATE_RESYNC_REQUIRED", "kiosk_runtime", url.c_str());
  }
}

void KioskRuntime::handle_resync_result(const kiosk::network::StateFetchOutcome& outcome) {
  if (!outcome.ok) {
    kiosk::health::log_structured(
        "WARN", "STATE_SYNC_FAIL", "kiosk_runtime",
        (std::string("http_status=") + std::to_string(outcome.http_status)).c_str());
    return;  // stays resyncing_; next scan/key/conflict will retry
  }

  kiosk::protocol::StateSnapshot snap;
  if (!kiosk::protocol::parse_state_snapshot_json(outcome.response_body, snap)) {
    kiosk::health::log_structured("ERROR", "STATE_SNAPSHOT_INCONSISTENT", "kiosk_runtime",
                                   "GET /state response body did not parse");
    return;  // stays resyncing_
  }

  auto apply_result = state_projection_.apply(snap);
  bool ok = apply_result == kiosk::protocol::ApplyResult::APPLIED ||
            apply_result == kiosk::protocol::ApplyResult::APPLIED_IDENTICAL;
  kiosk::health::log_structured(ok ? "INFO" : "ERROR", "STATE_SYNC_OK", "kiosk_runtime",
                                 kiosk::protocol::apply_result_to_string(apply_result));

  if (ok) {
    resyncing_ = false;
    local_qty_buffer_.clear();  // any pre-conflict local input is now stale
    render_current_business_state();
  }
  // else: a resync GET itself coming back stale/inconsistent/unsupported is
  // a deeper problem than this runtime can resolve on its own -- stay in
  // RESYNCING rather than fabricate progress (never mark this PASS from a
  // guess).
}

void KioskRuntime::handle_local_event(const LocalEvent& event) {
  switch (event.kind) {
    case LocalEventKind::SCAN:
      handle_scan(event.text, event.timestamp_ms);
      break;
    case LocalEventKind::KEYPAD_RAW:
      kiosk::health::log_structured("INFO", "INPUT_KEYPAD_RAW", "kiosk_runtime",
                                     "uncalibrated keypad activity (see 'keypad-calibrate')");
      break;
    case LocalEventKind::KEY_DOWN:
    case LocalEventKind::KEY_UP: {
      // The '*' hold-to-recover flow is owned entirely by
      // WifiRecoveryController (docs/WIFI_RECOVERY.md) -- it must keep
      // working independent of whatever business/session logic lives here,
      // and independent of provisioning state (§40).
      last_key_ = event.key;
      if (event.key != '*') {
        char msg[32];
        snprintf(msg, sizeof(msg), "key=%c %s", event.key,
                 event.kind == LocalEventKind::KEY_DOWN ? "down" : "up");
        kiosk::health::log_structured("INFO", "INPUT_KEY", "kiosk_runtime", msg);
        // Only act on press, not release, so a single physical key press
        // doesn't fire twice.
        if (event.kind == LocalEventKind::KEY_DOWN) handle_business_key(event.key);
      }
      break;
    }
    case LocalEventKind::WIFI_STATE: {
      kiosk::health::log_structured("INFO", "NET_WIFI_STATE", "kiosk_runtime",
                                     event.text.c_str());
      if (event.text == "CONNECTED") {
        wifi_indicator_ = kiosk::ui::WifiIndicator::CONNECTED;
      } else if (event.text == "CONNECTING") {
        wifi_indicator_ = kiosk::ui::WifiIndicator::CONNECTING;
      } else if (event.text == "DISCONNECTED") {
        wifi_indicator_ = kiosk::ui::WifiIndicator::DISCONNECTED;
      }
      // Refresh the idle screen so the indicator update is visible right
      // away rather than waiting for the next scan. Wi-Fi state changes
      // infrequently enough (connect/drop/reconnect) that this doesn't
      // fight with anything else trying to own the screen -- unless a scan
      // result is currently pending, in which case leave that screen alone.
      if (!scan_pending_result_) refresh_idle_screen();
      break;
    }
    case LocalEventKind::WIFI_RECOVERY_STATE:
      // Also owned by WifiRecoveryController for rendering; just log here.
      kiosk::health::log_structured("INFO", "NET_WIFI_RECOVERY_STATE", "kiosk_runtime",
                                     event.text.c_str());
      break;
  }
}

void KioskRuntime::handle_scan(const String& raw_code, unsigned long timestamp_ms) {
  last_scan_ = raw_code;

  // Immediate LOCAL presentation feedback — before any network attempt.
  // §12: this must render well under the 100ms local-feedback target since
  // it's a direct draw call with no I/O in between. Shown even when the
  // scan will be rejected below -- the operator should always see "the
  // scanner read something" regardless of what happens next.
  renderer_.draw_scan_received(raw_code, wifi_indicator_);

  ProvisioningState state = identity_.state();
  if (state != ProvisioningState::ACTIVE) {
    const char* code = identity_error_code(state);
    kiosk::health::log_structured("WARN", code, "kiosk_runtime",
                                   "scan rejected: device not ACTIVE, no backend contacted");
    renderer_.draw_scan_result(raw_code, "Thiet bi chua san sang (xem man hinh chinh)", code,
                               wifi_indicator_);
    return;
  }

  kiosk::protocol::OptionalQuantity none;  // SCAN never carries quantity_good
  send_business_event(kiosk::protocol::EventType::SCAN, raw_code, none);
  (void)timestamp_ms;  // superseded by send_business_event's own millis() read
}

// §14: this NEVER decides a business transition -- it only decides which
// generic EventType/payload to send, based on which state the SERVER most
// recently put the device in. The server alone decides whether the press
// was valid (STATE_INVALID_TRANSITION otherwise).
void KioskRuntime::reset_quantity_flow() {
  qty_step_ = QtyStep::GOOD;
  qty_good_ = 0;
  qty_defect_ = 0;
  local_qty_buffer_.clear();
}

void KioskRuntime::submit_final_quantity(int32_t good, int32_t defect, int32_t rework) {
  kiosk::protocol::OptionalQuantity q_good, q_defect, q_rework;
  q_good.present = true;
  q_good.value = good;
  q_defect.present = true;
  q_defect.value = defect;
  q_rework.present = true;
  q_rework.value = rework;
  send_business_event(kiosk::protocol::EventType::QUANTITY_SUBMITTED, "", q_good, q_defect, q_rework);
}

void KioskRuntime::handle_business_key(char key) {
  if (identity_.state() != ProvisioningState::ACTIVE) return;
  if (!state_projection_.has_snapshot() || resyncing_) return;  // no server authority to act against yet

  kiosk::protocol::BusinessState state = state_projection_.current().state;

  // GOOD/DEFECT/REWORK quantity flow: purely local sub-steps within the one
  // server state QUANTITY_INPUT (see QtyStep in kiosk_runtime.h). Keypad
  // semantics stay deterministic across all three digit-entry steps (§18 of
  // the task): digits append, '*' clears the CURRENT field, '#' confirms
  // the current step and advances.
  if (state == kiosk::protocol::BusinessState::QUANTITY_INPUT) {
    if (qty_step_ == QtyStep::REWORK_DECISION) {
      // Not a digit field -- a real Y/N choice, not overloaded onto */#/
      // digits, matching the task's own explicit "1 = CO, 2 = KHONG" ask.
      if (key == '1') {
        qty_step_ = QtyStep::REWORK;
        local_qty_buffer_.clear();
        render_current_business_state();
      } else if (key == '2') {
        submit_final_quantity(qty_good_, qty_defect_, 0);  // not repairable -> rework=0, finish now
      }
      return;
    }

    // GOOD / DEFECT / REWORK: identical digit-entry mechanics, differing
    // only in what happens once '#' confirms this step.
    if (key >= '0' && key <= '9') {
      if (local_qty_buffer_.size() < 6) {  // cap -- purely a local UI guard, not a business rule
        local_qty_buffer_ += key;
      }
      render_current_business_state();
      return;
    }
    if (key == '*') {
      local_qty_buffer_.clear();
      render_current_business_state();
      return;
    }
    if (key == '#') {
      int32_t value = local_qty_buffer_.empty()
                           ? 0
                           : static_cast<int32_t>(std::strtol(local_qty_buffer_.c_str(), nullptr, 10));
      if (qty_step_ == QtyStep::GOOD) {
        qty_good_ = value;
        local_qty_buffer_.clear();
        qty_step_ = QtyStep::DEFECT;
        render_current_business_state();
        return;
      }
      if (qty_step_ == QtyStep::DEFECT) {
        qty_defect_ = value;
        if (qty_defect_ == 0) {
          // §: "If DEFECT == 0: finish immediately" -- no repairable
          // question, rework is meaninglessly 0. Deliberately NOT clearing
          // local_qty_buffer_ here (real bug found live, 2026-08-24): the
          // "Dang gui..." immediate-feedback frame send_business_event()
          // draws right after this call re-renders whatever qty_step_/
          // buffer state currently is -- clearing first made it flash an
          // empty/zero digit screen instead of the value just entered.
          // apply_event_response() already clears the buffer once the real
          // response arrives (same as it always did for the old single-
          // quantity flow), so this still ends up clean, just not early.
          submit_final_quantity(qty_good_, 0, 0);
        } else {
          local_qty_buffer_.clear();  // safe here: DEFECT step is over, no submit is about to render this buffer
          qty_step_ = QtyStep::REWORK_DECISION;
          render_current_business_state();
        }
        return;
      }
      if (qty_step_ == QtyStep::REWORK) {
        // Local validation FIRST (§12: "do NOT submit" if over) -- avoids a
        // pointless round trip for the common typo case. Server still
        // independently re-validates (§15, REWORK_EXCEEDS_DEFECT) as
        // defense in depth, not trusting the device alone.
        if (value > qty_defect_) {
          local_qty_buffer_.clear();
          render_current_business_state("SO LUONG SUA KHONG DUOC LON HON SO LUONG LOI", true, false);
          return;
        }
        // Same "don't clear before the submit-triggered re-render" fix as
        // the DEFECT==0 path above.
        submit_final_quantity(qty_good_, qty_defect_, value);
        return;
      }
    }
    return;
  }

  if (state == kiosk::protocol::BusinessState::SESSION_ACTIVE && key == '#') {
    // Optional compatibility shortcut only (§18 of the quantity-flow task:
    // "Do not overload # with session finish in SESSION_ACTIVE as the
    // primary operator workflow" -- canonical finish is the employee-card
    // rescan, handled entirely server-side in kiosk_v2.py's SESSION_ACTIVE
    // SCAN branch, not here).
    kiosk::protocol::OptionalQuantity none;
    send_business_event(kiosk::protocol::EventType::FINISH_REQUESTED, "", none);
    return;
  }
}

void KioskRuntime::send_business_event(kiosk::protocol::EventType type, const String& raw_payload,
                                       const kiosk::protocol::OptionalQuantity& quantity,
                                       const kiosk::protocol::OptionalQuantity& quantity_defect,
                                       const kiosk::protocol::OptionalQuantity& quantity_rework) {
  if (api_endpoint_.length() == 0) {
    kiosk::health::log_structured("WARN", "CONFIG_BACKEND_NOT_SET", "kiosk_runtime",
                                   "event rejected: no backend configured");
    render_current_business_state("Chua cau hinh may chu (api-endpoint)", true, true);
    return;
  }

  if (sender_.busy()) {
    // §14 of the original spec / Phase 3: no durable journal exists yet, so
    // there is nowhere honest to queue this -- say so plainly rather than
    // silently dropping it or pretending it was queued.
    kiosk::health::log_structured("WARN", "EVENT_DROPPED_BUSY", "kiosk_runtime",
                                   "previous event still sending; no journal yet to queue this one");
    render_current_business_state("Dang gui su kien truoc - CHUA duoc luu", true, true);
    return;
  }

  kiosk::protocol::KioskEvent event;
  event.device.device_id = device_id_.c_str();
  event.device.hardware_id = hardware_id_.c_str();
  event.device.boot_id = boot_id_.c_str();
  event.event.event_id = kiosk::protocol::generate_random_hex_id(16);  // generated ONCE; retries reuse it
  event.event.device_seq = device_seq_.next();                        // allocated ONCE per event
  event.event.type = type;
  event.time.uptime_ms = millis();
  event.time.sync_status = time_sync_.status();
  event.time.sync_age_s = time_sync_.sync_age_s();
  event.time.timestamp_device_iso = time_sync_.iso8601_now().c_str();  // "" if not SYNCED/STALE
  event.payload.source = type == kiosk::protocol::EventType::SCAN ? "GM65" : "KEYPAD";
  event.payload.raw = raw_payload.c_str();
  event.quantity_good = quantity;
  event.quantity_defect = quantity_defect;
  event.quantity_rework = quantity_rework;
  // §7: the device's optimistic-concurrency claim -- "this is the version I
  // last saw". 0 if there is no snapshot yet at all (shouldn't happen: keys
  // are gated on has_snapshot() above; SCAN can reach here pre-bootstrap,
  // in which case 0 is the honest "I don't know any version yet" value).
  event.context.expected_state_version =
      state_projection_.has_snapshot() ? state_projection_.current().state_version : 0;
  event.context.workflow_version =
      state_projection_.has_snapshot() ? state_projection_.current().workflow_version : 0;

  std::string json_body = kiosk::protocol::encode_event_json(event);

  kiosk::health::log_structured(
      "INFO", "EVENT_CREATED", "kiosk_runtime",
      (std::string("event_id=") + event.event.event_id +
       " type=" + kiosk::protocol::event_type_to_string(type) +
       " device_seq=" + std::to_string(event.event.device_seq) +
       " expected_state_version=" + std::to_string(event.context.expected_state_version))
          .c_str());

  scan_pending_result_ = true;
  pending_raw_code_ = raw_payload;
  last_event_id_ = event.event.event_id;
  last_event_type_ = type;

  // Immediate LOCAL presentation feedback (§12) -- the SCAN case already
  // got its own "Da nhan ma" flash from draw_scan_received() in handle_scan;
  // for keypad-triggered events, show a lightweight "sending" note on top
  // of the current (still-authoritative-until-proven-otherwise) screen.
  if (type != kiosk::protocol::EventType::SCAN) {
    render_current_business_state("Dang gui...", false);
  }

  bool started = sender_.send(api_endpoint_, json_body.c_str(), event.event.event_id,
                              event.event.device_seq);
  if (!started) {
    // send() only refuses if already busy, which we already checked above
    // -- a race between the check and here is possible in principle but
    // harmless: report it honestly rather than assume success.
    scan_pending_result_ = false;
    kiosk::health::log_structured("ERROR", "EVENT_SEND_START_FAILED", "kiosk_runtime",
                                   event.event.event_id.c_str());
    render_current_business_state("Loi gui su kien - CHUA duoc luu", true, true);
  }
}

void KioskRuntime::poll() {
  kiosk::network::SendOutcome result;
  if (sender_.poll(result)) {
    scan_pending_result_ = false;
    has_scanned_ = true;
    last_backend_ok_ = result.outcome.ok;
    last_http_status_ = result.outcome.http_status;
    last_error_code_ = result.outcome.error_code;
    last_latency_ms_ = result.total_latency_ms;
    last_retry_count_ = result.attempts > 0 ? result.attempts - 1 : 0;

    // §66/§27: structured evidence tying this result back to the specific
    // event_id/device_seq it belongs to.
    char msg[160];
    snprintf(msg, sizeof(msg),
             "event_id=%s device_seq=%llu http_status=%d latency_ms=%u retry_count=%d error_code=%s",
             result.event_id.c_str(), static_cast<unsigned long long>(result.device_seq),
             result.outcome.http_status, result.total_latency_ms, last_retry_count_,
             result.outcome.error_code.c_str());
    kiosk::health::log_structured(result.outcome.ok ? "INFO" : "WARN",
                                   result.outcome.ok ? "EVENT_ACKED" : "EVENT_FAILED", "kiosk_runtime",
                                   msg);

    if (!result.outcome.ok) {
      // Transport-level failure (no response at all, or a 4xx/5xx that
      // never reached the business layer -- §83: every business outcome,
      // accept/reject/conflict, is HTTP 200) -- the authoritative state is
      // simply unknown to have changed; stay on the current screen and
      // surface an honest transient error.
      String msg_line = result.outcome.error_code == "NET_WIFI_DOWN"
                            ? String("Khong co Wi-Fi - CHUA duoc luu")
                            : String("Loi ket noi may chu - CHUA duoc luu");
      render_current_business_state(msg_line, true, true);
    } else {
      kiosk::protocol::EventResponse resp;
      bool parsed = kiosk::protocol::parse_event_response_json(result.response_body, resp);
      apply_event_response(parsed, resp, result.event_id);
    }
  }

  kiosk::network::StateFetchOutcome fetch_result;
  if (state_fetcher_.poll(fetch_result)) {
    handle_resync_result(fetch_result);
  }
}

void KioskRuntime::apply_event_response(bool parsed, const kiosk::protocol::EventResponse& resp,
                                        const std::string& event_id) {
  using kiosk::protocol::ApplyResult;
  using kiosk::protocol::EventOutcomeKind;

  if (!parsed || resp.kind == EventOutcomeKind::MALFORMED) {
    kiosk::health::log_structured("ERROR", "EVENT_RESPONSE_MALFORMED", "kiosk_runtime", event_id.c_str());
    render_current_business_state("Phan hoi khong hop le tu server", true, true);
    return;
  }

  if (resp.server_seq >= 0) last_server_seq_ = resp.server_seq;

  if (resp.kind == EventOutcomeKind::CONFLICT_RESYNC) {
    kiosk::health::log_structured(
        "WARN", "STATE_CONFLICT", "kiosk_runtime",
        (std::string("event_id=") + event_id +
         " current_state_version=" + std::to_string(resp.current_state_version))
            .c_str());
    start_resync();
    return;
  }

  // Captured BEFORE apply() so the QUANTITY_INPUT-entry check below can
  // tell "just arrived here" from "already here, this is a rejected
  // final-submit response" (see reset_quantity_flow() call further down).
  kiosk::protocol::BusinessState prev_state = state_projection_.has_snapshot()
      ? state_projection_.current().state
      : kiosk::protocol::BusinessState::WAIT_EMPLOYEE;  // sentinel when no snapshot exists yet -- never QUANTITY_INPUT, so a first-ever apply() into QUANTITY_INPUT still correctly resets

  // SUCCESS or BUSINESS_REJECTED both carry a snapshot -- apply it as
  // authoritative truth regardless of accept/reject (invariants 13/14: the
  // device's own guess about what "should" happen never substitutes for
  // what the server actually decided).
  ApplyResult apply_result = state_projection_.apply(resp.snapshot);
  char log_msg[160];
  snprintf(log_msg, sizeof(log_msg), "event_id=%s apply=%s state=%s version=%llu",
           event_id.c_str(), kiosk::protocol::apply_result_to_string(apply_result),
           kiosk::protocol::business_state_to_string(resp.snapshot.state),
           static_cast<unsigned long long>(resp.snapshot.state_version));
  kiosk::health::log_structured("INFO", "STATE_APPLY", "kiosk_runtime", log_msg);

  local_qty_buffer_.clear();  // whatever was pending locally is resolved (accepted or rejected) now

  switch (apply_result) {
    case ApplyResult::APPLIED:
    case ApplyResult::APPLIED_IDENTICAL: {
      bool is_err = resp.kind == EventOutcomeKind::BUSINESS_REJECTED;
      // Fresh entry into QUANTITY_INPUT (from SESSION_ACTIVE via rescan or
      // the FINISH_REQUESTED shortcut) always starts the local GOOD/DEFECT/
      // REWORK sub-flow at GOOD. A response that LEAVES the device in
      // QUANTITY_INPUT (e.g. REWORK_EXCEEDS_DEFECT on the final submit) must
      // NOT reset -- §25 of the task: "quantity flow remains recoverable",
      // meaning the operator lands back at the step they were correcting,
      // not all the way back at GOOD.
      if (resp.snapshot.state == kiosk::protocol::BusinessState::QUANTITY_INPUT &&
          prev_state != kiosk::protocol::BusinessState::QUANTITY_INPUT) {
        reset_quantity_flow();
      }
      String msg_line = "";
      if (is_err) {
        msg_line = resp.error_message.empty() ? String(resp.error_code.c_str())
                                              : String(resp.error_message.c_str());
      }
      render_current_business_state(msg_line, is_err);
      break;
    }
    case ApplyResult::REJECTED_STALE:
      kiosk::health::log_structured("ERROR", "STATE_VERSION_REGRESSION", "kiosk_runtime", event_id.c_str());
      start_resync();
      break;
    case ApplyResult::REJECTED_INCONSISTENT:
      kiosk::health::log_structured("ERROR", "STATE_SNAPSHOT_INCONSISTENT", "kiosk_runtime",
                                     event_id.c_str());
      start_resync();
      break;
    case ApplyResult::REJECTED_UNSUPPORTED:
      kiosk::health::log_structured("ERROR", "STATE_UNSUPPORTED", "kiosk_runtime", event_id.c_str());
      render_current_business_state("Trang thai server khong duoc ho tro (firmware cu?)", true, true);
      break;
  }
}

}  // namespace kiosk::runtime
