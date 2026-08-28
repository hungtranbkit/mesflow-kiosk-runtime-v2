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

  // Field report (2026-08-27): the post-FINISH result hold (see
  // apply_event_response's QUANTITY_INPUT-exit branch and
  // check_finish_result_hold_timeout()). A real error in THIS call
  // (is_error) always takes precedence -- never hide a genuine failure
  // behind a stale "HOÀN TẤT" banner from a previous, already-successful
  // submit.
  if (finish_result_hold_active_ && !is_error) {
    renderer_.draw_finish_result_screen(finish_result_employee_name_, finish_result_operation_code_,
                                        finish_result_good_, finish_result_defect_, finish_result_rework_,
                                        wifi_indicator_);
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
      case QtyStep::SUMMARY:
        renderer_.draw_quantity_summary_screen(snap.view, qty_good_, qty_defect_, qty_rework_,
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
// Deliberately reuses the SAME network_/apply_event_response() path a live
// scan/keypress uses (queued as an OFFLINE_REPLAY-kind request, LOW
// priority tier), rather than a second completion handler:
// StateProjection::apply()'s existing version-gating
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
  if (replaying_ || replay_next_ < replay_event_ids_.size()) return;  // already have/working a queue
  auto pending = journal_.pending_in_device_seq_order();
  if (pending.empty()) return;

  // Memory-simplification pass (2026-08-26, "keep the ESP runtime simple
  // and disposable per request"): this used to copy each pending record's
  // FULL payload (the original event's JSON envelope, potentially a few
  // hundred bytes) into a second, parallel ReplayItem vector -- a real
  // duplication with no correctness purpose, since journal_.find() can
  // always look the record back up fresh, in O(log n), at the exact moment
  // it's about to be sent. Only the (small, fixed-size) event_id strings
  // are queued now -- total replay-queue memory is proportional to the
  // NUMBER of pending events, never their combined payload size, matching
  // "replay should never load the full offline history into memory."
  replay_event_ids_.clear();
  replay_event_ids_.reserve(pending.size());
  for (const auto* record : pending) {
    replay_event_ids_.push_back(record->event_id);
  }
  replay_next_ = 0;
  kiosk::health::log_structured(
      "INFO", "OFFLINE_REPLAY_START", "kiosk_runtime",
      (std::string("queued=") + std::to_string(replay_event_ids_.size())).c_str());
}

void KioskRuntime::check_offline_replay() {
  if (replay_next_ >= replay_event_ids_.size()) {
    if (replaying_) {
      // Just finished the last item's send (poll() below clears replaying_
      // once its result lands) -- nothing left to do until the next
      // reconnect calls start_offline_replay_if_needed() again.
    }
    return;
  }
  if (replaying_) return;                          // current item's send is still in flight
  if (network_.high_priority_busy()) return;  // a LIVE scan/keypress send has priority (§3 tier order)
  if (identity_.state() != ProvisioningState::ACTIVE || env_mismatch_) return;

  const std::string& event_id = replay_event_ids_[replay_next_];
  const kiosk::protocol::JournalRecord* record = journal_.find(event_id);
  if (record == nullptr) {
    // PENDING/IN_FLIGHT records are never compacted away (see
    // EventJournalIndex's own compaction-policy comment), so this should
    // never actually happen -- but if the journal state ever changes out
    // from under this queue some other way, skip rather than dereference a
    // null pointer.
    kiosk::health::log_structured("ERROR", "OFFLINE_REPLAY_RECORD_MISSING", "kiosk_runtime", event_id.c_str());
    ++replay_next_;
    return;
  }

  // record->payload is looked up here, on the MAIN thread (safe -- see
  // network_worker.h's own top comment for why the worker thread must never
  // touch EventJournal directly) and copied into the request's fixed body[]
  // buffer by enqueue_offline_replay() itself.
  bool queued = network_.enqueue_offline_replay(record->event_id, record->device_seq, api_endpoint_, record->payload,
                                                identity_.kiosk_token());
  if (queued) {
    replaying_ = true;
    kiosk::health::log_structured(
        "INFO", "OFFLINE_REPLAY_SEND", "kiosk_runtime",
        (std::string("event_id=") + record->event_id + " device_seq=" + std::to_string(record->device_seq) +
         " (" + std::to_string(replay_next_ + 1) + "/" + std::to_string(replay_event_ids_.size()) + ")")
            .c_str());
  }
  // If it didn't queue (LOW-tier queue momentarily full), simply try again
  // on the next poll() -- the record stays PENDING in the journal either way.
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
  bool queued = network_.enqueue_state_fetch(url, identity_.kiosk_token());
  if (!queued) {
    // HIGH-tier queue momentarily full (a business event or bootstrap is
    // ahead of it) -- fine, resyncing_ stays true and the next poll() cycle
    // that finds resyncing_ still set without a result yet will just retry
    // via the same call site (start_resync() is only ever re-invoked by a
    // fresh conflict/scan, not spun here, so nothing extra to unwind).
    kiosk::health::log_structured("INFO", "STATE_RESYNC_REQUIRED", "kiosk_runtime",
                                   "resync requested while the network worker's HIGH queue was full");
  } else {
    kiosk::health::log_structured("WARN", "STATE_RESYNC_REQUIRED", "kiosk_runtime", url.c_str());
  }
}

void KioskRuntime::handle_resync_result(const kiosk::network::NetworkResult& result) {
  if (!result.outcome.ok) {
    kiosk::health::log_structured(
        "WARN", "STATE_SYNC_FAIL", "kiosk_runtime",
        (std::string("http_status=") + std::to_string(result.outcome.http_status)).c_str());
    return;  // stays resyncing_; next scan/key/conflict will retry
  }

  kiosk::protocol::StateSnapshot snap;
  if (!kiosk::protocol::parse_state_snapshot_json(result.response_body, snap)) {
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
  // Scan latency instrumentation: T1 ("QR accepted after debounce", i.e.
  // right after the immediate local feedback draw completes) -- read back
  // in poll() once this scan's server response lands.
  //
  // Real bug found live testing this exact instrumentation on real hardware
  // (2026-08-26): this used to run unconditionally, even while a PREVIOUS
  // scan/event's send was still genuinely in flight (a real, if rare,
  // possibility on a slow/lossy Wi-Fi link -- exactly the condition this
  // whole latency investigation cares about). That overlapping scan gets
  // busy-guard-rejected inside send_business_event() without ever touching
  // last_scan_dispatch_ms_, but this line had already clobbered
  // last_scan_received_ms_ with a NEWER timestamp -- so once the ORIGINAL
  // (still in-flight) scan's response finally landed, poll() computed
  // last_scan_dispatch_ms_ (old, smaller) - last_scan_received_ms_ (new,
  // larger), an unsigned subtraction that UNDERFLOWS to a huge garbage
  // number (observed live: firmware_local_ms=4294961574). scan_pending_
  // result_ is exactly "is a previous send's result still outstanding" --
  // skip the overwrite while that's true and let the busy-rejected scan's
  // own (correct, do-nothing) path run without disturbing the in-flight
  // scan's own timing.
  if (!scan_pending_result_) {
    last_scan_received_ms_ = millis();
  }

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
  qty_rework_ = 0;
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
  // the task): digits append, '#' confirms the current step and advances.
  //
  // CORRECTION (2026-08-27, found live while adding the SUMMARY step
  // below): this comment used to also claim "'*' clears the CURRENT
  // field", matching the `if (key == '*') { local_qty_buffer_.clear(); ...}`
  // branch a few lines down and the "* XÓA" footer hint on the DEFECT/
  // REWORK screens -- neither has ever actually worked. handle_local_event()
  // unconditionally filters '*' out before handle_business_key() is ever
  // called (`event.key != '*' && ...`), since '*' is globally reserved for
  // WifiRecoveryController's hold-to-open-menu gesture, even for a single
  // quick tap nowhere near the hold threshold. The `key == '*'` branch below
  // is therefore genuinely dead code, and the "* XÓA" footer hint on those
  // two screens is misleading. Left as-is for this pass (fixing it means
  // deciding a real replacement key for TWO already-shipped screens, a
  // separate, wider change than this one) -- flagged here so it isn't
  // mistaken for working just because the comment used to say so.
  if (state == kiosk::protocol::BusinessState::QUANTITY_INPUT) {
    if (qty_step_ == QtyStep::SUMMARY) {
      // Real field report (2026-08-27): see QtyStep::SUMMARY's own comment.
      // '#' is the ONLY thing that actually fires the real submit now.
      //
      // Real bug found live while building this: '*' does NOT work here (or
      // anywhere else in this whole GOOD/DEFECT/REWORK flow, despite the
      // existing "* XÓA" footer hint on those screens claiming it clears the
      // current field) -- handle_local_event()'s own KEY_DOWN/KEY_UP
      // dispatch unconditionally filters out '*' before handle_business_key()
      // is ever called (`event.key != '*' && ...`), because '*' is globally
      // reserved for WifiRecoveryController's hold-to-open-menu gesture,
      // even for a single quick tap that never reaches the hold threshold.
      // That pre-existing dead-footer-hint bug is out of scope for this
      // fix (it affects the ALREADY-SHIPPED GOOD/DEFECT/REWORK screens, not
      // just this new one) -- flagged separately, not fixed here. This new
      // SUMMARY screen instead uses '0' (a digit that means nothing at this
      // step -- no digit entry happens here) to restart the whole local
      // entry so a mis-typed value can be fixed before anything is sent.
      if (key == '#') {
        submit_final_quantity(qty_good_, qty_defect_, qty_rework_);
      } else if (key == '0') {
        reset_quantity_flow();
        render_current_business_state();
      }
      return;
    }
    if (qty_step_ == QtyStep::REWORK_DECISION) {
      // Not a digit field -- a real Y/N choice, not overloaded onto */#/
      // digits, matching the task's own explicit "1 = CO, 2 = KHONG" ask.
      if (key == '1') {
        qty_step_ = QtyStep::REWORK;
        local_qty_buffer_.clear();
        render_current_business_state();
      } else if (key == '2') {
        qty_rework_ = 0;  // not repairable -> rework=0
        qty_step_ = QtyStep::SUMMARY;
        render_current_business_state();
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
        local_qty_buffer_.clear();
        if (qty_defect_ == 0) {
          // §: "If DEFECT == 0" -- no repairable question, rework is
          // meaninglessly 0. Used to submit immediately here; now goes to
          // SUMMARY first (see QtyStep::SUMMARY's own comment) so the
          // operator sees GOOD/DEFECT/REWORK together before it's sent.
          qty_rework_ = 0;
          qty_step_ = QtyStep::SUMMARY;
        } else {
          qty_step_ = QtyStep::REWORK_DECISION;
        }
        render_current_business_state();
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
        qty_rework_ = value;
        local_qty_buffer_.clear();
        qty_step_ = QtyStep::SUMMARY;
        render_current_business_state();
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
  // Any new business action means the terminal is already back in active
  // use -- never make an operator wait out the rest of a previous FINISH's
  // result-hold banner (§8 "scanner remains available" is stricter than
  // any presentation timer). The hold's own timeout would eventually clear
  // it anyway; this just makes a real scan/key press dismiss it instantly
  // instead of waiting for whatever's left of kFinishResultHoldMs.
  finish_result_hold_active_ = false;

  if (api_endpoint_.length() == 0) {
    kiosk::health::log_structured("WARN", "CONFIG_BACKEND_NOT_SET", "kiosk_runtime",
                                   "event rejected: no backend configured");
    render_current_business_state("Chưa cấu hình máy chủ (api-endpoint)", true, true);
    return;
  }

  // §11/§12 of the 2026-08-26 "eliminate recurrent server connection
  // failures" pass: there is no more pre-flight busy check here at all.
  // NetworkWorker serializes HTTP execution internally (one request at a
  // time, by construction) and its HIGH-tier queue can hold several
  // requests -- so a second business event arriving while the first is
  // still in flight is simply queued behind it, not dropped. The record
  // this function is about to journal below is what makes that safe even
  // in the rare case the queue is momentarily full: see the enqueue-failure
  // branch further down.
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

  // §11/§12 of the 2026-08-26 "eliminate recurrent server connection
  // failures" pass: journal a PENDING record for this event FIRST,
  // unconditionally, BEFORE any attempt to send it. This used to be labeled
  // "Phase 3A SHADOW MODE" (durability-proving only, never load-bearing) --
  // it is now the real thing: the enqueue-failure branch below relies on
  // this record already existing to make FINISH/START durable even when the
  // worker can't take the request immediately. Only "critical operator
  // action" events are durable per docs/OFFLINE.md ("heartbeat/spinner/
  // telemetry" stay non-durable) -- SCAN/FINISH_REQUESTED/
  // QUANTITY_SUBMITTED/CANCEL_REQUESTED are exactly the events reaching
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
    // Real bug found live (2026-08-27 "Final Reliability Standardization"
    // pass, §10/§11/R1): this return value used to be discarded entirely --
    // a FULL journal (the only realistic failure mode; DUPLICATE_EVENT can't
    // happen here since event_id was just freshly generated above) meant
    // this code proceeded exactly as if the record were durable: still
    // attempted the network send, and on ANY subsequent failure (network
    // queue full, WiFi down, server unreachable) told the operator "ĐÃ LƯU"
    // (SAVED) for an action that was NEVER actually persisted anywhere --
    // the single worst thing this whole reliability contract exists to
    // prevent (R1: never acknowledge locally before durable persistence).
    // Checked now: a failed append aborts here, before any network attempt,
    // with an honest "not recorded" message -- never a false "ĐÃ LƯU".
    bool journaled_ok = journal_.append_event(jr);
#if MESFLOW_DEBUG_API
    kiosk::health::log_memory_snapshot("AFTER_JOURNAL_APPEND");
#endif
    if (!journaled_ok) {
      kiosk::health::log_structured("ERROR", "EVENT_NOT_DURABLE", "kiosk_runtime",
                                    (std::string("event_id=") + event.event.event_id +
                                     " journal_pressure=" +
                                     std::to_string(static_cast<int>(journal_.pressure())))
                                        .c_str());
      render_current_business_state("BỘ NHỚ CHỜ GỬI ĐÃ ĐẦY - CHƯA được lưu", true, true);
      return;
    }
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

  // Scan latency instrumentation: T2 ("HTTP request started"), i.e. right
  // before handing off to NetworkWorker. Set unconditionally (cheap, one
  // millis() read) -- only ever read back for a SCAN in poll(), see there.
  last_scan_dispatch_ms_ = millis();
  bool queued = network_.enqueue_business_event(event.event.event_id, event.event.device_seq, api_endpoint_,
                                                json_body, identity_.kiosk_token());
  if (queued) {
    // PENDING -> IN_FLIGHT now that the worker has actually accepted this
    // request onto its HIGH-priority queue.
    kiosk::protocol::JournalTransition jt;
    jt.event_id = event.event.event_id;
    jt.sync_status = kiosk::protocol::JournalSyncStatus::IN_FLIGHT;
    jt.retry_count = 0;
    jt.last_attempt_uptime_ms = millis();
    journal_.append_transition(jt);
  } else {
    // §7/§11: enqueue only fails if the HIGH-tier queue itself is full (a
    // small, fixed depth -- see network_worker.cpp) -- there is no more
    // per-call task creation to fail, and this is NEVER a reboot condition.
    // The event is already durably PENDING in the journal above, so nothing
    // is lost: the periodic offline-replay fallback (or the next
    // reconnect/foreground interaction) will pick it up and send it exactly
    // once (server-side idempotency by event_id makes this safe even if the
    // queue happens to drain and pick this same event up moments later via
    // a different path). Tell the operator plainly and return to a usable
    // screen -- never a dead end, never a reboot.
    scan_pending_result_ = false;
    kiosk::health::log_structured("WARN", "EVENT_ENQUEUE_QUEUE_FULL", "kiosk_runtime",
                                   event.event.event_id.c_str());
    render_current_business_state("ĐÃ LƯU / SẼ ĐỒNG BỘ KHI CÓ KẾT NỐI", false, false);
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

void KioskRuntime::check_finish_result_hold_timeout() {
  if (!finish_result_hold_active_) return;
  if (static_cast<long>(millis() - finish_result_hold_until_ms_) < 0) return;

  // Same "presentation-only, never blocks input" shape as
  // check_error_view_timeout() right above -- the hold expiring just means
  // it's time to show whatever the actual current authoritative state
  // already is (by now, WAIT_EMPLOYEE -- the card-scan screen).
  finish_result_hold_active_ = false;
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
  if (network_.high_priority_busy()) return;  // a send is already in flight/queued -- let it resolve first

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
  // §2-§5 of the 2026-08-26 "eliminate recurrent server connection
  // failures" pass: ONE unified poll for every request kind, dispatched by
  // result.kind, replacing the old separate sender_.poll()/
  // state_fetcher_.poll() calls (network_ now owns both).
  kiosk::network::NetworkResult result;
  bool was_replay = replaying_;  // captured before any of the shared bookkeeping below runs
  bool got_result = network_.poll(result);
  if (got_result && result.kind == kiosk::network::NetworkRequestKind::STATE_FETCH) {
    handle_resync_result(result);
  } else if (got_result && result.kind == kiosk::network::NetworkRequestKind::BOOTSTRAP) {
    // 2026-08-27: bootstrap moved off its old blocking HTTPClient call onto
    // NetworkWorker -- see take_bootstrap_result()'s own doc comment for
    // why this just stashes the result rather than acting on it directly
    // (the .ino owns the retry/cooldown bookkeeping and calls
    // on_bootstrap_result() itself once it picks this up).
    pending_bootstrap_result_ = result;
    bootstrap_result_ready_ = true;
  } else if (got_result && result.kind == kiosk::network::NetworkRequestKind::UI_BUNDLE_FETCH) {
    // 2026-08-27 "close final two runtime gaps" pass: UiSyncController's
    // download moved off its own per-call AsyncStateFetcher task onto
    // NetworkWorker -- same stash-for-later shape as BOOTSTRAP just above
    // (UiSyncController picks this up via take_ui_bundle_fetch_result() on
    // its own next poll(), called from the .ino right after this one).
    pending_ui_bundle_fetch_result_ = result;
    ui_bundle_fetch_result_ready_ = true;
  } else if (got_result && result.kind == kiosk::network::NetworkRequestKind::HEARTBEAT) {
    // REVERTED (2026-08-27, same day): this used to also feed heartbeat
    // TCP_CONNECT_FAIL results into consecutive_tcp_connect_fail_, so a
    // stale connection could be discovered and healed by the background
    // heartbeat before an employee's own scan hit it. Real regression found
    // live within the hour: force_reconnect() firing MORE often (now from
    // heartbeat too, not just foreground events) cascades into the .ino's
    // existing "a reconnect re-arms bootstrap" logic -- and BootstrapClient
    // is STILL a single, fully SYNCHRONOUS HTTPClient call on the main
    // loop() thread (never migrated onto NetworkWorker -- it doesn't spawn
    // its own per-call task the way event_send/heartbeat/state_fetch used
    // to, so it wasn't part of the original SRAM-leak fix, but that also
    // means it's the one request kind that still blocks loop() directly).
    // One bootstrap
    // attempt can block loop() for up to RUNTIME_HTTP_TIMEOUT_MS (2.5s) --
    // and the reconnect trigger is MOST likely to fire exactly when the
    // network is already degraded, i.e. exactly when that 2.5s block is
    // most likely to actually happen. That's a fully sufficient explanation
    // for the reported "quét thẻ chậm, bấm số phải bấm nhiều lần mới ăn"
    // (scans slow, keypresses dropped) -- a blocked loop() can't poll the
    // keypad/scanner at all for that whole window. The underlying "bootstrap
    // blocks the main loop" gap is real and pre-existing (this fix only
    // made it fire more often, not created it) -- left as a flagged,
    // separate problem rather than attempting a bigger BootstrapClient
    // migration under time pressure right now. Heartbeat's own result goes
    // back to being logged-only (network_worker.cpp) and otherwise ignored
    // here, exactly as before this same-day fix.
  } else if (got_result && (result.kind == kiosk::network::NetworkRequestKind::BUSINESS_EVENT ||
                            result.kind == kiosk::network::NetworkRequestKind::OFFLINE_REPLAY)) {
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
    // Scan latency instrumentation: this response belongs to the exact scan
    // currently being timed only if it's still the most recent SCAN send AND
    // matches its event_id (a later keypress event's response landing while
    // a stale timestamp is still set must never be mistaken for it).
    bool is_timed_scan = last_event_type_ == kiosk::protocol::EventType::SCAN &&
                         result.event_id == last_event_id_ && last_scan_received_ms_ != 0;

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

    // Network self-recovery counter -- see consecutive_tcp_connect_fail()'s
    // doc comment. A success, or any OTHER kind of failure, clears the
    // streak; only back-to-back TCP_CONNECT_FAIL (every attempt of a fully-
    // exhausted send failing to even connect, despite WiFi reporting
    // CONNECTED) counts toward it.
    if (!result.outcome.ok && result.outcome.error_code == "TCP_CONNECT_FAIL") {
      ++consecutive_tcp_connect_fail_;
    } else {
      consecutive_tcp_connect_fail_ = 0;
    }

    // §8 of the 2026-08-27 "Final Runtime Closure" pass: feeds
    // network_state()'s classify_network_state() call -- see that
    // function's own comment. Any failed request of any kind counts
    // (broader than consecutive_tcp_connect_fail_ above, which exists
    // ONLY to drive the WiFi self-recovery reconnect trigger and
    // deliberately ignores every other failure class).
    if (!result.outcome.ok) {
      ++consecutive_request_failures_;
    } else {
      consecutive_request_failures_ = 0;
    }

    if (!result.outcome.ok) {
      // Transport-level failure (no response at all), OR a real non-2xx HTTP
      // response the business layer never parses (see the §14 comment just
      // below -- "every business outcome is HTTP 200" was true for
      // ACCEPTED/REJECTED/CONFLICT, but is NOT true for 401/403 device-not-
      // allowed or 409 idempotency-payload-mismatch, both confirmed live
      // against app/mesflow/web/kiosk_v2.py). The authoritative STATE
      // (state_projection_) is simply unknown to have changed either way;
      // stay on the current screen and surface an honest error.
      //
      // Real bug found live (2026-08-27 "Final Reliability Standardization"
      // pass, §2/§6/R8/R9): this used to journal EVERY !ok outcome as
      // PENDING unconditionally, including outcome.retryable == false cases
      // -- classify_http_result() already correctly marks 401/403/409/4xx
      // as non-retryable, but nothing here ever consulted that flag, so a
      // permanently-doomed event (wrong device, or a genuine payload/
      // event_id reuse conflict) stayed PENDING forever and got resent by
      // EVERY future offline-replay/reconnect cycle -- exactly the "retry a
      // permanent error forever" anti-pattern §2 explicitly forbids, and a
      // real violation of R9 (terminal events may only be compacted after
      // durable TERMINAL state -- this event could never even reach one).
      // A genuinely transient transport failure (DNS/TCP/timeout/5xx/429,
      // all outcome.retryable == true) still correctly stays PENDING --
      // docs/OFFLINE.md's "TIMEOUT DOES NOT MEAN SERVER DID NOT PROCESS"
      // reasoning is unchanged for those.
      bool permanent = !result.outcome.retryable;
      kiosk::protocol::JournalTransition jt;
      jt.event_id = result.event_id;
      jt.sync_status = permanent ? kiosk::protocol::JournalSyncStatus::HUMAN_REVIEW
                                 : kiosk::protocol::JournalSyncStatus::PENDING;
      jt.retry_count = static_cast<uint32_t>(last_retry_count_);
      jt.last_attempt_uptime_ms = millis();
      jt.last_error_code = result.outcome.error_code;
      journal_.append_transition(jt);
      if (permanent) {
        kiosk::health::log_structured(
            "ERROR", "EVENT_PERMANENT_FAILURE", "kiosk_runtime",
            (std::string("event_id=") + result.event_id +
             " http_status=" + std::to_string(result.outcome.http_status) +
             " error_code=" + result.outcome.error_code +
             " -- marked HUMAN_REVIEW, will NOT be retried")
                .c_str());
      }

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
      // Real bug found live (2026-08-27, §9 of the "Final Runtime Closure"
      // pass): this checked "NET_WIFI_DOWN", a string nothing in this
      // codebase has EVER produced -- AsyncEventSender (now NetworkWorker)
      // has always emitted plain "WIFI_DOWN". This branch was dead code;
      // the friendly "Không có Wi-Fi" message could never actually render.
      if (result.outcome.error_code == "WIFI_DOWN") {
        msg_line = "Không có Wi-Fi - CHƯA được lưu";
      } else if (result.outcome.http_status == 401 || result.outcome.http_status == 403) {
        msg_line = "THIẾT BỊ KHÔNG ĐƯỢC PHÉP - Liên hệ quản trị";
      } else if (result.outcome.error_code == "HTTP_5XX") {
        msg_line = "SERVER ĐANG LỖI - Thử lại sau";
      } else {
        msg_line = "Lỗi kết nối máy chủ - CHƯA được lưu";
      }
      render_current_business_state(msg_line, true, true);
      if (is_timed_scan) {
        // A failed round trip is already fully accounted for by the
        // EVENT_FAILED line just above (same latency_ms/retry_count) --
        // just clear the timestamp so it can't leak into a later, unrelated
        // event's SCAN_LATENCY calculation.
        last_scan_received_ms_ = 0;
      }
    } else {
      kiosk::protocol::EventResponse resp;
      bool parsed = kiosk::protocol::parse_event_response_json(result.response_body, resp);
      unsigned long render_t0 = millis();
      apply_event_response(parsed, resp, result.event_id);
      // Defensive: dispatch must never be BEFORE received for a genuinely
      // timed scan -- if it somehow is (an edge case this codebase's own
      // instrumentation bug hunt didn't anticipate, or the ~49-day millis()
      // rollover), skip logging a garbage underflowed number rather than
      // report a false "10000+ second" outlier.
      if (is_timed_scan && last_scan_dispatch_ms_ < last_scan_received_ms_) {
        kiosk::health::log_structured("WARN", "SCAN_LATENCY_SKIPPED", "kiosk_runtime",
                                      "dispatch timestamp precedes received timestamp -- discarding");
        is_timed_scan = false;
        last_scan_received_ms_ = 0;
      }
      if (is_timed_scan) {
        unsigned long render_took = millis() - render_t0;
        unsigned long firmware_local_ms = last_scan_dispatch_ms_ - last_scan_received_ms_;
        unsigned long total_ms = millis() - last_scan_received_ms_;
        char lat_msg[176];
        snprintf(lat_msg, sizeof(lat_msg),
                 "event_id=%s firmware_local_ms=%lu network_ms=%u retry_count=%d render_ms=%lu total_ms=%lu",
                 result.event_id.c_str(), firmware_local_ms, result.total_latency_ms, last_retry_count_,
                 render_took, total_ms);
        kiosk::health::log_structured("INFO", "SCAN_LATENCY", "kiosk_runtime", lat_msg);
        last_scan_received_ms_ = 0;  // consumed -- avoid re-logging against a later, unrelated event
      }
    }
  }

  check_error_view_timeout();
  check_finish_result_hold_timeout();
  check_ui_timeout();
  // Periodic fallback trigger (see last_replay_fallback_check_ms_'s own doc
  // comment) -- independent of the reconnect-driven trigger, so a pending
  // backlog left behind by a transient failure (never a real Wi-Fi drop)
  // still gets retried on a healthy, still-connected link.
  if (millis() - last_replay_fallback_check_ms_ >= kReplayFallbackCheckIntervalMs) {
    last_replay_fallback_check_ms_ = millis();
    start_offline_replay_if_needed();
  }
  check_offline_replay();

  // §6/§18: keep the status bar's queue count current. Cheap (an in-memory
  // map-size read, journal_.counts()) -- fine to do every poll().
  auto journal_counts = journal_.counts();
  renderer_.set_offline_queue_size(journal_counts.pending + journal_counts.in_flight);

  // §8 field-log requirement: log every network_state() TRANSITION (not
  // every poll() -- that would flood the log for no reason), so a field
  // support session reviewing serial/structured logs can see exactly when
  // and how long the device spent OFFLINE_WIFI/OFFLINE_SERVER/AUTH_BLOCKED,
  // not just infer it from scattered per-request lines.
  {
    kiosk::network::NetworkState current = network_state();
    if (current != last_logged_network_state_) {
      kiosk::health::log_structured(
          "INFO", "NETWORK_STATE_CHANGED", "kiosk_runtime",
          (std::string(kiosk::network::network_state_to_string(last_logged_network_state_)) + " -> " +
           kiosk::network::network_state_to_string(current))
              .c_str());
      last_logged_network_state_ = current;
    }
  }
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

  // Also captured BEFORE apply() -- purely for the post-FINISH result hold
  // below. The OUTGOING snapshot (about to be replaced by whatever this
  // response carries, normally WAIT_EMPLOYEE once a session closes) is the
  // only place that still has employee_name/operation_code; the new one
  // won't.
  kiosk::protocol::ViewModel prev_view =
      state_projection_.has_snapshot() ? state_projection_.current().view : kiosk::protocol::ViewModel();

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

  // Real bug found live (2026-08-26, "vẫn bị lỗi kết nối máy chủ" field
  // report -- traced to a DIFFERENT root cause than the message implied):
  // this journal transition used to live INSIDE the apply_result switch's
  // APPLIED/APPLIED_IDENTICAL case only. But whether the SERVER accepted or
  // rejected this event (resp.kind) is a COMPLETELY SEPARATE question from
  // whether the accompanying snapshot happens to be stale relative to what
  // this device already has applied locally (apply_result) -- the two were
  // incorrectly coupled. A replayed event the server genuinely ACKed, but
  // whose snapshot arrived STALE (a real, common case during replay --
  // newer live events had already moved state_projection_ past it by the
  // time this old queued response came back), fell through
  // REJECTED_STALE/REJECTED_INCONSISTENT/REJECTED_UNSUPPORTED below, NONE
  // of which ever wrote a journal transition at all -- the record stayed
  // PENDING forever even though it had already durably succeeded
  // server-side. Confirmed live: the exact same handful of event_ids kept
  // replaying-and-ACKing on every single boot, across a full reflash,
  // because their on-disk status never actually advanced past PENDING.
  // Fixed by deciding the journal outcome from resp.kind ALONE, always,
  // independent of whether the snapshot itself gets applied to the UI --
  // resp.kind is guaranteed SUCCESS or BUSINESS_REJECTED here (MALFORMED
  // and CONFLICT_RESYNC both already returned above).
  bool business_accepted = resp.kind != EventOutcomeKind::BUSINESS_REJECTED;
  {
    kiosk::protocol::JournalTransition jt;
    jt.event_id = event_id;
    jt.sync_status = business_accepted ? kiosk::protocol::JournalSyncStatus::ACKED
                                       : kiosk::protocol::JournalSyncStatus::REJECTED;
    jt.last_error_code = business_accepted ? "" : resp.error_code;
    jt.last_attempt_uptime_ms = millis();
    journal_.append_transition(jt);
  }

  local_qty_buffer_.clear();  // whatever was pending locally is resolved (accepted or rejected) now

  switch (apply_result) {
    case ApplyResult::APPLIED:
    case ApplyResult::APPLIED_IDENTICAL: {
      last_sync_iso_ = time_sync_.iso8601_now().c_str();  // §4: a real applied server response counts as a sync
      bool is_err = !business_accepted;
      // (journal transition already handled above, unconditionally --
      // see this function's own comment for why it moved out of this case)
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

      // Field report (2026-08-27): "khi quet op xong, nên co man hình tổng
      // hợp là tên gì, làm op gì... 5-10 giay gi do mới chuyen qua man hinh
      // quet thẻ". Arm the hold exactly when a FINISH (QUANTITY_SUBMITTED)
      // was just ACCEPTED and it actually closed the session (state left
      // QUANTITY_INPUT) -- never on a REWORK_EXCEEDS_DEFECT-style rejection
      // that leaves the device in QUANTITY_INPUT (that's still an
      // in-progress correction, not a completion), and never on a bare
      // rejection (is_err true implies business_accepted false, already
      // excluded by construction below).
      if (business_accepted && last_event_type_ == kiosk::protocol::EventType::QUANTITY_SUBMITTED &&
          prev_state == kiosk::protocol::BusinessState::QUANTITY_INPUT &&
          resp.snapshot.state != kiosk::protocol::BusinessState::QUANTITY_INPUT) {
        finish_result_employee_name_ =
            prev_view.has_employee_name ? String(prev_view.employee_name.c_str()) : String("");
        finish_result_operation_code_ =
            prev_view.has_operation_code ? String(prev_view.operation_code.c_str()) : String("");
        finish_result_good_ = qty_good_;
        finish_result_defect_ = qty_defect_;
        finish_result_rework_ = qty_rework_;
        finish_result_hold_active_ = true;
        finish_result_hold_until_ms_ = millis() + kFinishResultHoldMs;
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
