#include "kiosk_runtime.h"

#include <esp_heap_caps.h>

#include <cstdlib>

#include "../health/memory_diag.h"
#include "../health/recovery_supervisor.h"
#include "../health/structured_log.h"
#include "../network/endpoint_utils.h"
#include "../protocol/crc32.h"
#include "../protocol/journal_record.h"
#include "../protocol/protocol_codec.h"
#include "recovery_overlay.h"

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
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_RENDER");
#endif
}

// §2/§3 of the 2026-08-26 UX-hardening pass: real, confirmed gap -- before
// this, the device had no concept of "which environment am I talking to" at
// all, so it could never detect a mismatch (e.g. a TEST-configured kiosk
// accidentally pointed at PRODUCTION via a fat-fingered api-endpoint).
//
// Deliberately asymmetric: expected_environment()=="" (device never
// provisioned with one -- true for every device already in the field
// before this firmware update) does NOT hard-block. Blocking every
// already-deployed kiosk the instant this firmware lands, until someone
// physically visits each one to run `expected-env:<X>`, would be exactly
// the harsh "firmware update bricks the floor" regression this whole task
// exists to avoid ("ESP khó update ngoài xưởng"). Only a device that HAS
// been told what to expect, and gets a DIFFERENT real answer from the
// server, is blocked -- that is the one case with a genuine safety
// consequence (operating against the wrong environment's real data).
bool KioskRuntime::apply_server_environment(const kiosk::network::BootstrapResult& result) {
  using kiosk::protocol::Environment;
  using kiosk::protocol::environment_from_config_string;
  using kiosk::protocol::environment_from_server_role;

  server_environment_ = environment_from_server_role(result.server_role.c_str());
  server_version_ = result.server_version;
  renderer_.set_current_environment(server_environment_);  // §2/§6: status bar reads this on every render

  Environment expected = environment_from_config_string(config_.expected_environment().c_str());
  bool genuine_mismatch =
      expected != Environment::UNKNOWN && server_environment_ != Environment::UNKNOWN && expected != server_environment_;

  if (genuine_mismatch) {
    env_mismatch_ = true;
    env_mismatch_expected_ = expected;
    kiosk::health::log_structured(
        "ERROR", "SERVER_ENV_MISMATCH", "kiosk_runtime",
        (std::string("expected=") + kiosk::protocol::environment_to_string(expected) +
         " actual=" + kiosk::protocol::environment_to_string(server_environment_))
            .c_str());
    render_current_business_state();  // takes over the whole screen -- see the env_mismatch_ check at its top
    return false;
  }

  if (env_mismatch_) {
    // Was mismatched, now resolved (config fixed, or now pointed at the
    // right server) -- clear it and fall through to a normal render.
    kiosk::health::log_structured("INFO", "SERVER_ENV_MISMATCH_RESOLVED", "kiosk_runtime", "");
  }
  env_mismatch_ = false;
  last_sync_iso_ = time_sync_.iso8601_now().c_str();  // §4: a successful bootstrap counts as a sync
  return true;
}

void KioskRuntime::on_bootstrap_result(const kiosk::network::BootstrapResult& result) {
  // §11 of the 2026-08-26 physical field test: real gap found live -- a
  // REJECTED bootstrap (a DISABLED/SUSPENDED kiosk identity, now correctly
  // rejected server-side with a real 403 after the matching
  // app/mesflow/web/kiosk_v2.py fix) previously fell straight through this
  // early return with NO message shown at all -- the device just silently
  // sat on the pre-bootstrap waiting screen forever, giving an operator no
  // way to tell "disabled" apart from "still connecting". Shown directly
  // via draw_error_view() (not render_current_business_state(), which
  // gates everything else behind has_snapshot()/identity checks that don't
  // apply here -- there IS no snapshot to fall back to, by definition, for
  // the very first bootstrap this boot) -- same visual treatment as any
  // other business rejection, generic fallback text if the server didn't
  // send a message (an older backend, or the other rejection cause with no
  // message field).
  if (result.status == kiosk::network::BootstrapStatus::REJECTED) {
    String msg = result.reject_message.length() > 0 ? result.reject_message
                                                     : String("Thiết bị bị từ chối bởi server");
    kiosk::health::log_structured("WARN", "BOOTSTRAP_REJECTED_SHOWN", "kiosk_runtime", msg.c_str());
    renderer_.draw_error_view(msg, /*is_network_error=*/false, wifi_indicator_);
    return;
  }
  if (result.status != kiosk::network::BootstrapStatus::OK) return;

  if (!apply_server_environment(result)) return;  // §3: mismatch -- never apply a snapshot, never proceed

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
    last_activity_ms_ = millis();  // §13: a fresh snapshot this boot starts the idle clock now, not at millis()==0
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
  // §6/§15: default to "not showing the error view" -- the one branch below
  // that actually draws it re-sets this true right before returning. Every
  // OTHER branch in this function (identity/resyncing/waiting/bundle/quantity
  // sub-steps/normal business state) IS itself a real screen transition, so
  // it correctly counts as "the next render call" that dismisses the error
  // view per its own documented design, whether or not it happens to also be
  // carrying its own transient_message. Captured BEFORE the reset below so
  // repeated consecutive error redraws (e.g. two busy-retries in a row)
  // keep counting the timeout from the ORIGINAL onset, not restart it.
  bool was_already_showing_error = showing_error_view_;
  showing_error_view_ = false;

  // §2/§3 of the 2026-08-25 finish-anti-stuck-recovery follow-up: SAFE_MODE
  // takes over the ENTIRE screen, same as the identity/waiting screens
  // below but checked first -- no business flow renders at all while this
  // is true (handle_scan()/handle_business_key() independently refuse to
  // act, so there is nothing for those screens to show anyway).
  if (kiosk::health::is_safe_mode()) {
    renderer_.draw_safe_mode_screen(kiosk::health::safe_mode_reason(), wifi_indicator_);
    return;
  }

  // §3 of the 2026-08-26 UX-hardening pass: a genuine environment mismatch
  // takes over the WHOLE screen, same precedence as SAFE_MODE above it --
  // no business flow renders while this is true (handle_scan()/
  // handle_business_key() independently refuse to act, matching how
  // SAFE_MODE's own refusal is structured). Deliberately has NO auto-
  // dismiss timeout (§3: "KHÔNG CHO PHÉP THAO TÁC" must not silently
  // resume) -- it only clears via apply_server_environment() on a LATER
  // successful bootstrap that no longer disagrees (fixed config, or now
  // pointed at the right server), which keeps running in the background on
  // every reconnect/retry per §28's "even blocked state must continue
  // background connectivity checks".
  if (env_mismatch_) {
    renderer_.draw_server_mismatch_screen(env_mismatch_expected_, server_environment_, wifi_indicator_);
    return;
  }

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
    // §6/§15: remember that we're showing this so poll()'s
    // check_error_view_timeout() can force a way out if nothing else ever
    // triggers a subsequent render (the exact real incident this fixes).
    if (!was_already_showing_error) error_view_shown_at_ms_ = millis();
    showing_error_view_ = true;
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

// §17/§19 of the 2026-08-26 UX-hardening pass: real, active offline replay
// -- before this, EventJournal was durable "shadow mode" recording only
// (never read back), and every network outage just failed/retried via
// AsyncEventSender's bounded 5-attempt backoff, then gave up with an
// honest "CHƯA được lưu" (not saved) message -- correct, never silently
// wrong, but not actually resilient to an outage longer than that backoff
// window. This makes the journal's PENDING/IN_FLIGHT backlog a REAL queue:
// a reconnect now walks it in original device_seq order and resends each
// one, one at a time.
//
// Deliberately reuses the SAME sender_/apply_event_response() path a live
// scan/keypress uses, rather than a second AsyncEventSender + a parallel
// completion handler: StateProjection::apply()'s existing version-gating
// (STALE/INCONSISTENT/UNSUPPORTED, state_projection.h) already protects a
// stale replayed response from ever overwriting state a NEWER live
// interaction has since moved past -- a replay response arriving "late"
// relative to fresher live traffic is handled exactly the same, safe way
// STATE_CONFLICT already is (start_resync()), with zero new conflict logic
// needed. This is also exactly what makes replay itself safe to resend
// verbatim: app/mesflow/web/kiosk_v2.py's /events handler is genuinely
// idempotent by (device_id, event_id) -- an event that actually landed
// before a drop just gets its original cached response back, never
// re-applied (see kiosk_v2.py:588-593, `SELECT payload_hash,response_json
// FROM kiosk_v2_events WHERE device_id=%s AND event_id=%s`).
//
// Known, accepted limitation of reusing the live path: apply_event_response()
// always calls render_current_business_state() with whatever
// transient_message the response carried, which can (rarely -- only when a
// reconnect happens to have a real backlog) briefly flash over whatever a
// DIFFERENT, currently-interacting operator is looking at, since this
// runtime has no separate "silent background" rendering mode. Documented
// here rather than solved -- see the final report's Known Gaps.
void KioskRuntime::start_offline_replay_if_needed() {
  if (replaying_ || replay_next_ < replay_queue_.size()) return;  // already have/working a queue
  auto pending = journal_.pending_in_device_seq_order();
  if (pending.empty()) return;

  replay_queue_.clear();
  replay_queue_.reserve(pending.size());
  for (const auto* record : pending) {
    replay_queue_.push_back(ReplayItem{record->event_id, record->payload, record->device_seq});
  }
  replay_next_ = 0;
  kiosk::health::log_structured(
      "INFO", "OFFLINE_REPLAY_START", "kiosk_runtime",
      (std::string("queued=") + std::to_string(replay_queue_.size())).c_str());
}

void KioskRuntime::check_offline_replay() {
  if (replay_next_ >= replay_queue_.size()) {
    if (replaying_) {
      // Just finished the last item's send (poll() below clears replaying_
      // once its result lands) -- nothing left to do until the next
      // reconnect calls start_offline_replay_if_needed() again.
    }
    return;
  }
  if (replaying_) return;                          // current item's send is still in flight
  if (sender_.busy() || send_retry_pending_) return;  // a LIVE scan/keypress send has priority
  if (identity_.state() != ProvisioningState::ACTIVE || env_mismatch_) return;

  const ReplayItem& item = replay_queue_[replay_next_];
  bool started = sender_.send(api_endpoint_, item.payload.c_str(), item.event_id, item.device_seq);
  if (started) {
    replaying_ = true;
    kiosk::health::log_structured(
        "INFO", "OFFLINE_REPLAY_SEND", "kiosk_runtime",
        (std::string("event_id=") + item.event_id + " device_seq=" + std::to_string(item.device_seq) +
         " (" + std::to_string(replay_next_ + 1) + "/" + std::to_string(replay_queue_.size()) + ")")
            .c_str());
  }
  // If it didn't start (sender_ raced busy between the check above and
  // here), simply try again on the next poll() -- no state to unwind.
}

void KioskRuntime::request_manual_resync() {
  if (identity_.state() != ProvisioningState::ACTIVE) {
    kiosk::health::log_structured("INFO", "RECOVERY_MENU_RESYNC_SKIPPED", "kiosk_runtime",
                                  "identity not ACTIVE -- nothing to resync against");
    return;
  }
  kiosk::health::log_structured("INFO", "RECOVERY_MENU_RESYNC", "kiosk_runtime",
                                "operator-requested resync from recovery menu");
  start_resync();
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
    last_activity_ms_ = millis();  // §13: a resync landing is real activity, not idle time
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
      // §5 of the 2026-08-25 follow-up: while the recovery menu or the
      // Wi-Fi setup portal owns the screen, a digit key is meant for THAT
      // overlay (WifiRecoveryController handles it directly, same bus
      // event), never for whatever business input happens to be underneath
      // -- must never silently mutate business state (e.g. QUANTITY_INPUT's
      // local digit buffer) from a keypress the operator aimed at the menu.
      if (event.key != '*' && !kiosk::runtime::recovery_overlay_active()) {
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
  // §2/§3 of the 2026-08-25 finish-anti-stuck-recovery follow-up: SAFE_MODE
  // runs no business flow at all -- no network call, no journal write, just
  // an honest log line. The SAFE_MODE screen (drawn by
  // render_current_business_state()'s own top-of-function gate) is the only
  // thing on screen; recovery happens via the '*'-hold menu, not scans.
  if (kiosk::health::is_safe_mode()) {
    kiosk::health::log_structured("INFO", "SAFE_MODE_SCAN_IGNORED", "kiosk_runtime",
                                  "scan ignored -- device is in SAFE_MODE");
    return;
  }
  if (env_mismatch_) {
    // §3: "KHÔNG CHO PHÉP THAO TÁC" -- no scan reaches the backend at all
    // while mismatched, same posture as SAFE_MODE above.
    kiosk::health::log_structured("WARN", "ENV_MISMATCH_SCAN_IGNORED", "kiosk_runtime",
                                  "scan ignored -- server environment mismatch");
    return;
  }
  last_scan_ = raw_code;
  last_activity_ms_ = millis();  // §13: any scan counts as activity, valid or not

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
    renderer_.draw_scan_result(raw_code, "Thiết bị chưa sẵn sàng (xem màn hình chính)", code,
                               wifi_indicator_);
    return;
  }

  kiosk::protocol::OptionalQuantity none;  // SCAN never carries quantity_good
  send_business_event(kiosk::protocol::EventType::SCAN, raw_code, none);
  (void)timestamp_ms;  // superseded by send_business_event's own millis() read
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_SCAN_EVENT");
#endif
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
  qty_timeout_warned_ = false;
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
  if (kiosk::health::is_safe_mode()) return;  // SAFE_MODE: no business flow -- see handle_scan()'s own comment
  if (env_mismatch_) return;  // §3: no business flow while server environment is mismatched
  if (identity_.state() != ProvisioningState::ACTIVE) return;
  if (!state_projection_.has_snapshot() || resyncing_) return;  // no server authority to act against yet

  last_activity_ms_ = millis();  // §13: any accepted business keypress counts as activity

  kiosk::protocol::BusinessState state = state_projection_.current().state;

  // §7/§12 of the 2026-08-26 UX-hardening pass: WAIT_OPERATION had no way
  // out short of the '*'-hold Wi-Fi-recovery menu (a diagnostic tool, not a
  // business action) -- '#' is unused in this state (handle_business_key
  // previously had no branch for it at all), so it's free to mean "Hủy"
  // here. Reuses CANCEL_REQUESTED, which app/mesflow/web/kiosk_v2.py's
  // _apply_event() already implements correctly server-side (refuses only
  // when a real work_session_id is open, which WAIT_OPERATION never has --
  // that only exists after SESSION_ACTIVE/QUANTITY_INPUT) -- no new backend
  // work needed, this was simply never dispatched from any keypad path.
  if (state == kiosk::protocol::BusinessState::WAIT_OPERATION && key == '#') {
    kiosk::protocol::OptionalQuantity none;
    send_business_event(kiosk::protocol::EventType::CANCEL_REQUESTED, "", none);
    return;
  }

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
          // "Đang gửi..." immediate-feedback frame send_business_event()
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
          render_current_business_state("SỐ LƯỢNG SỬA KHÔNG ĐƯỢC LỚN HƠN SỐ LƯỢNG LỖI", true, false);
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
    render_current_business_state("Chưa cấu hình máy chủ (api-endpoint)", true, true);
    return;
  }

  // send_retry_pending_ counts as "busy" too (2026-08-24 self-recovery task)
  // -- sender_.busy() alone is false while a task-creation-failure retry is
  // scheduled (xTaskCreate never actually started, so busy_ never flipped
  // true), which would otherwise let a NEW scan/keypress race in during the
  // ~kSendRetryDelayMs window, steal the sender, and make check_send_retry()
  // misread the sender's real busy-with-something-else state as "the retry
  // itself failed again" and wrongly escalate to a reboot.
  if (sender_.busy() || send_retry_pending_) {
    // §14 of the original spec / Phase 3: no durable journal exists yet, so
    // there is nowhere honest to queue this -- say so plainly rather than
    // silently dropping it or pretending it was queued.
    kiosk::health::log_structured("WARN", "EVENT_DROPPED_BUSY", "kiosk_runtime",
                                   "previous event still sending/retrying; no journal yet to queue this one");
    render_current_business_state("Đang gửi sự kiện trước - CHƯA được lưu", true, true);
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

  // Phase 3A SHADOW MODE (§16 of the durable-journal task): journal a
  // PENDING copy of this event's lifecycle for durability-proving purposes
  // ONLY -- this does NOT send anything itself, does NOT gate/delay the
  // real send below, and its result is never consulted by any business
  // decision. Same event_id/device_seq/payload as what's about to actually
  // be sent, so the shadow record can be verified against the real outcome.
  // Only "critical operator action" events are durable per docs/OFFLINE.md
  // ("heartbeat/spinner/telemetry" stay non-durable) -- SCAN/
  // FINISH_REQUESTED/QUANTITY_SUBMITTED are exactly the events reaching
  // this function, so no extra filtering is needed here.
  {
    kiosk::protocol::JournalRecord jr;
    jr.record_version = 1;
    jr.event_id = event.event.event_id;
    jr.device_seq = event.event.device_seq;
    jr.boot_id = boot_id_.c_str();
    jr.event_type = kiosk::protocol::event_type_to_string(type);
    jr.payload = json_body;
    char hash_hex[9];
    snprintf(hash_hex, sizeof(hash_hex), "%08x",
             kiosk::protocol::crc32(reinterpret_cast<const uint8_t*>(json_body.data()), json_body.size()));
    jr.payload_hash = hash_hex;
    jr.expected_state_version = event.context.expected_state_version;
    jr.created_uptime_ms = event.time.uptime_ms;
    jr.time_sync_status = kiosk::protocol::time_sync_status_to_string(event.time.sync_status);
    jr.sync_status = kiosk::protocol::JournalSyncStatus::PENDING;
    journal_.append_event(jr);  // failure (FULL/DUPLICATE) is logged internally; shadow mode never blocks on it
#if MESFLOW_DEBUG_API
    kiosk::health::log_memory_snapshot("AFTER_JOURNAL_APPEND");
#endif
  }

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
    render_current_business_state("Đang gửi...", false);
  }

  bool started = sender_.send(api_endpoint_, json_body.c_str(), event.event.event_id,
                              event.event.device_seq);
  if (started) {
    // Shadow-mode lifecycle: PENDING -> IN_FLIGHT now that the real async
    // send has actually started (§18 of the task).
    kiosk::protocol::JournalTransition jt;
    jt.event_id = event.event.event_id;
    jt.sync_status = kiosk::protocol::JournalSyncStatus::IN_FLIGHT;
    jt.retry_count = 0;
    jt.last_attempt_uptime_ms = millis();
    journal_.append_transition(jt);
  } else {
    // send() only refuses if already busy (checked above -- a race between
    // that check and here is possible in principle but harmless) or if
    // xTaskCreate() itself failed (API_ERR_TASK_CREATE_FAILED) -- the real,
    // reproduced root cause behind this task ("stuck on error screen" after
    // journal-driven RAM exhaustion). §10: retry ONCE after a bounded delay
    // rather than leaving this as an immediate, uninvestigated dead end --
    // the event is already durably PENDING in the journal above regardless
    // of which path this takes, so nothing is lost either way.
    kiosk::health::log_structured("ERROR", "EVENT_SEND_START_FAILED", "kiosk_runtime",
                                   event.event.event_id.c_str());
    if (!send_retry_pending_) {
      send_retry_pending_ = true;
      send_retry_at_ms_ = millis() + kSendRetryDelayMs;
      retry_json_body_ = json_body;
      retry_event_id_ = event.event.event_id;
      retry_device_seq_ = event.event.device_seq;
      // Opportunistic: if the journal itself is under pressure, a
      // compaction right now is the single most likely fix for exactly
      // this failure mode -- don't wait for the periodic 30s check.
      if (journal_.should_consider_compaction()) journal_.compact();
      render_current_business_state("Lỗi gửi sự kiện - đang thử lại...", true, true);
    } else {
      // A retry was ALREADY pending when this happened again -- i.e. two
      // consecutive task-creation failures. Treat this as
      // application-level-exhausted for this boot: leaving the operator
      // parked on a screen that will never recover on its own is exactly
      // the "no dead-end" invariant this task exists to enforce, so
      // escalate to a controlled reboot instead.
      scan_pending_result_ = false;
      uint32_t mem_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      uint32_t mem_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      kiosk::health::request_controlled_reboot(
          kiosk::health::RecoveryCode::TASK_CREATE_FAILED,
          ("second consecutive xTaskCreate failure, event_id=" + event.event.event_id).c_str(),
          static_cast<uint8_t>(journal_.pressure()), mem_free, mem_largest);
      // request_controlled_reboot() never returns.
    }
  }
}

void KioskRuntime::check_send_retry() {
  if (!send_retry_pending_) return;
  if (millis() < send_retry_at_ms_) return;

  bool started = sender_.send(api_endpoint_, retry_json_body_.c_str(), retry_event_id_, retry_device_seq_);
  if (started) {
    send_retry_pending_ = false;
    kiosk::protocol::JournalTransition jt;
    jt.event_id = retry_event_id_;
    jt.sync_status = kiosk::protocol::JournalSyncStatus::IN_FLIGHT;
    jt.retry_count = 1;
    jt.last_attempt_uptime_ms = millis();
    journal_.append_transition(jt);
    kiosk::health::log_structured("INFO", "EVENT_SEND_RETRY_OK", "kiosk_runtime", retry_event_id_.c_str());
  } else {
    // Second consecutive failure for the SAME event -- send_business_event()
    // won't be called again for it, so drive the same escalation from here.
    send_retry_pending_ = false;
    scan_pending_result_ = false;
    uint32_t mem_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    uint32_t mem_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    kiosk::health::request_controlled_reboot(
        kiosk::health::RecoveryCode::TASK_CREATE_FAILED,
        ("retry itself failed to spawn a task, event_id=" + retry_event_id_).c_str(),
        static_cast<uint8_t>(journal_.pressure()), mem_free, mem_largest);
    // never returns.
  }
}

void KioskRuntime::check_error_view_timeout() {
  if (!showing_error_view_) return;
  if (millis() - error_view_shown_at_ms_ < kErrorViewTimeoutMs) return;

  // §6/§15: nothing else has triggered a redrawing render call within the
  // timeout -- force one now rather than leave the operator stuck. This is
  // a PRESENTATION-only recovery: it does not touch state_projection_,
  // does not resend/cancel anything in flight, and does not fabricate a
  // business outcome -- it only returns to whatever the current
  // authoritative state actually is (or the identity/waiting screen, same
  // as any other refresh).
  uint32_t mem_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  uint32_t mem_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  kiosk::health::record_recovery_event(kiosk::health::RecoveryCode::UI_STALL,
                                       "error view auto-timeout -- returning to current authoritative state",
                                       static_cast<uint8_t>(journal_.pressure()), mem_free, mem_largest);
  refresh_idle_screen();
}

// §13 of the 2026-08-26 ESP kiosk UX-hardening pass: central inactivity
// timeout, replacing "no timeout at all" for WAIT_OPERATION/QUANTITY_INPUT
// (the only two business states ui_timeout_policy.h assigns a nonzero
// value -- see that file for why the others are 0).
void KioskRuntime::check_ui_timeout() {
  if (kiosk::health::is_safe_mode()) return;
  if (env_mismatch_) return;  // §3: no business action (including a timeout-driven cancel) while mismatched
  if (identity_.state() != ProvisioningState::ACTIVE) return;
  if (!state_projection_.has_snapshot() || resyncing_) return;
  if (sender_.busy() || send_retry_pending_) return;  // a send is already in flight -- let it resolve first

  kiosk::protocol::BusinessState state = state_projection_.current().state;
  uint32_t timeout_ms = ui_timeout_ms_for_state(state);
  if (timeout_ms == 0) return;

  uint32_t elapsed_ms = static_cast<uint32_t>(millis() - last_activity_ms_);
  if (!ui_state_should_timeout(state, elapsed_ms)) return;

  if (state == kiosk::protocol::BusinessState::WAIT_OPERATION) {
    // Same CANCEL_REQUESTED path as the '#' key (handle_business_key) --
    // the backend cleanly returns WAIT_EMPLOYEE since no work_session_id is
    // open yet in this state. Reset the clock immediately so this doesn't
    // refire every poll() while the cancel itself is in flight.
    last_activity_ms_ = millis();
    kiosk::health::log_structured("INFO", "UI_TIMEOUT_CANCEL", "kiosk_runtime",
                                   "WAIT_OPERATION idle timeout -> CANCEL_REQUESTED");
    kiosk::protocol::OptionalQuantity none;
    send_business_event(kiosk::protocol::EventType::CANCEL_REQUESTED, "", none);
    return;
  }

  if (state == kiosk::protocol::BusinessState::QUANTITY_INPUT) {
    // Deliberately NOT a CANCEL_REQUESTED here (see ui_timeout_policy.h's
    // header comment): a real work_session is already open by the time the
    // device reaches QUANTITY_INPUT, and the backend correctly refuses to
    // cancel one (CANCEL_NOT_SUPPORTED) -- silently discarding an
    // in-progress FINISH would be worse than leaving the screen up. First
    // timeout: warn and re-arm for one more window. Second consecutive
    // timeout: give up trying to prompt further and just re-render the
    // current authoritative state (still QUANTITY_INPUT -- a human must
    // complete it, or a supervisor must intervene server-side).
    last_activity_ms_ = millis();
    if (!qty_timeout_warned_) {
      qty_timeout_warned_ = true;
      kiosk::health::log_structured("INFO", "UI_TIMEOUT_WARN", "kiosk_runtime",
                                     "QUANTITY_INPUT idle timeout -- warning operator, session stays open");
      // Real usability bug found live on the test board (physical field
      // test, 2026-08-26): passing is_error=true here routed through
      // render_current_business_state()'s full-screen draw_error_view()
      // takeover -- a red "LỖI" (ERROR) banner for what is actually a
      // benign "please continue" reminder, AND it replaced the quantity
      // screen entirely, so the operator's already-in-progress digit entry
      // context visually vanished (even though nothing was actually lost
      // server-side -- state_projection_ never changed). is_error=false
      // instead falls through to the normal QUANTITY_INPUT rendering
      // path, which shows this same message INLINE alongside the digit
      // buffer the operator was already looking at -- correct severity,
      // no lost context.
      render_current_business_state("Vui lòng nhập số lượng", false, false);
    } else {
      kiosk::health::log_structured("INFO", "UI_TIMEOUT_STAY", "kiosk_runtime",
                                     "QUANTITY_INPUT idle timeout again -- staying (real session open, no safe reset)");
      refresh_idle_screen();
    }
    return;
  }
}

void KioskRuntime::poll() {
  kiosk::network::SendOutcome result;
  bool was_replay = replaying_;  // captured before any of the shared bookkeeping below runs
  if (sender_.poll(result)) {
    if (was_replay) {
      // §17/§19: exactly one attempt per item per reconnect cycle -- a
      // failure leaves it PENDING in the journal (the existing failure-path
      // journal_.append_transition() below already does this, replay or
      // not) and the NEXT reconnect's start_offline_replay_if_needed() will
      // naturally pick it up again. Advance regardless of outcome so a
      // single stuck item can never wedge the rest of the backlog forever.
      replaying_ = false;
      ++replay_next_;
    }
    scan_pending_result_ = false;
    has_scanned_ = true;
    last_backend_ok_ = result.outcome.ok;
    last_http_status_ = result.outcome.http_status;
    last_error_code_ = result.outcome.error_code;
    last_latency_ms_ = result.total_latency_ms;
    last_retry_count_ = result.attempts > 0 ? result.attempts - 1 : 0;

    // §66/§27: structured evidence tying this result back to the specific
    // event_id/device_seq it belongs to.
    char msg[192];
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
      //
      // Shadow-mode journal: docs/OFFLINE.md's "TIMEOUT DOES NOT MEAN
      // SERVER DID NOT PROCESS" -- a transport failure is NEVER recorded as
      // REJECTED/CONFLICT here (that would be a guess), only as still-
      // PENDING with the attempt/error recorded. Phase 3B's replay logic is
      // exactly what will later act on this; Phase 3A only observes it.
      kiosk::protocol::JournalTransition jt;
      jt.event_id = result.event_id;
      jt.sync_status = kiosk::protocol::JournalSyncStatus::PENDING;
      jt.retry_count = static_cast<uint32_t>(last_retry_count_);
      jt.last_attempt_uptime_ms = millis();
      jt.last_error_code = result.outcome.error_code;
      journal_.append_transition(jt);

      // §14/§Error Recovery of the 2026-08-26 UX-hardening pass: verified
      // (not assumed) against retry_policy.cpp before changing anything --
      // classify_http_result() already correctly buckets every 4xx as
      // non-retryable and every 5xx as retryable (both accurate already),
      // but collapses 401/403/404/409/etc. into one generic "HTTP_4XX"
      // error_code, discarding the real status -- this device's message
      // for a deauthorized/forbidden device looked IDENTICAL to a plain
      // network blip ("Lỗi kết nối máy chủ"), even though
      // result.outcome.http_status (a separate field, always populated)
      // already has the real answer. Genuine gap: fixed here without
      // touching retry_policy.cpp's own classification, which was already
      // correct.
      String msg_line;
      if (result.outcome.error_code == "NET_WIFI_DOWN") {
        msg_line = "Không có Wi-Fi - CHƯA được lưu";
      } else if (result.outcome.http_status == 401 || result.outcome.http_status == 403) {
        msg_line = "THIẾT BỊ KHÔNG ĐƯỢC PHÉP - Liên hệ quản trị";
      } else if (result.outcome.error_code == "HTTP_5XX") {
        msg_line = "SERVER ĐANG LỖI - Thử lại sau";
      } else {
        msg_line = "Lỗi kết nối máy chủ - CHƯA được lưu";
      }
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

  check_send_retry();
  check_error_view_timeout();
  check_ui_timeout();
  check_offline_replay();

  // §6/§18: keep the status bar's queue count current. Cheap (an in-memory
  // map-size read, journal_.counts()) -- fine to do every poll().
  auto journal_counts = journal_.counts();
  renderer_.set_offline_queue_size(journal_counts.pending + journal_counts.in_flight);
}

void KioskRuntime::apply_event_response(bool parsed, const kiosk::protocol::EventResponse& resp,
                                        const std::string& event_id) {
  using kiosk::protocol::ApplyResult;
  using kiosk::protocol::EventOutcomeKind;

  last_activity_ms_ = millis();  // §13: any server round-trip landing (parsed or not) counts as activity

  if (!parsed || resp.kind == EventOutcomeKind::MALFORMED) {
    kiosk::health::log_structured("ERROR", "EVENT_RESPONSE_MALFORMED", "kiosk_runtime", event_id.c_str());
    // Same "don't guess" reasoning as the transport-failure case above --
    // a malformed response body is not evidence of accept or reject.
    {
      kiosk::protocol::JournalTransition jt;
      jt.event_id = event_id;
      jt.sync_status = kiosk::protocol::JournalSyncStatus::PENDING;
      jt.last_error_code = "EVENT_RESPONSE_MALFORMED";
      jt.last_attempt_uptime_ms = millis();
      journal_.append_transition(jt);
    }
    render_current_business_state("Phản hồi không hợp lệ từ server", true, true);
    return;
  }

  if (resp.server_seq >= 0) last_server_seq_ = resp.server_seq;

  if (resp.kind == EventOutcomeKind::CONFLICT_RESYNC) {
    kiosk::health::log_structured(
        "WARN", "STATE_CONFLICT", "kiosk_runtime",
        (std::string("event_id=") + event_id +
         " current_state_version=" + std::to_string(resp.current_state_version))
            .c_str());
    {
      kiosk::protocol::JournalTransition jt;
      jt.event_id = event_id;
      jt.sync_status = kiosk::protocol::JournalSyncStatus::CONFLICT;
      jt.last_error_code = "STATE_CONFLICT";
      jt.last_attempt_uptime_ms = millis();
      journal_.append_transition(jt);
    }
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
      last_sync_iso_ = time_sync_.iso8601_now().c_str();  // §4: a real applied server response counts as a sync
      bool is_err = resp.kind == EventOutcomeKind::BUSINESS_REJECTED;
      // Shadow-mode journal: the one clean terminal transition -- the
      // server gave a real, parsed, applied business answer. ACKED for a
      // genuine accept, REJECTED for a genuine business rejection (never
      // guessed; this is exactly what resp.kind already tells us).
      {
        kiosk::protocol::JournalTransition jt;
        jt.event_id = event_id;
        jt.sync_status = is_err ? kiosk::protocol::JournalSyncStatus::REJECTED
                                : kiosk::protocol::JournalSyncStatus::ACKED;
        jt.last_error_code = is_err ? resp.error_code : "";
        jt.last_attempt_uptime_ms = millis();
        journal_.append_transition(jt);
      }
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
      render_current_business_state("Trạng thái server không được hỗ trợ (firmware cũ?)", true, true);
      break;
  }
}

}  // namespace kiosk::runtime
