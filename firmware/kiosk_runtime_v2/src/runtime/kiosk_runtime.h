#pragma once

#include <Arduino.h>

#include <string>
#include <vector>

#include "../network/api_client.h"
#include "../network/bootstrap_client.h"
#include "../network/state_client.h"
#include "../network/time_sync.h"
#include "../protocol/event_response.h"
#include "../protocol/ids.h"
#include "../protocol/state_projection.h"
#include "../security/device_identity.h"
#include "../storage/config_store.h"
#include "../storage/event_journal.h"
#include "../storage/ui_bundle_store.h"
#include "../protocol/environment_label.h"
#include "../ui/renderer.h"
#include "event_bus.h"
#include "ui_timeout_policy.h"

namespace kiosk::runtime {

// GOOD/DEFECT/REWORK quantity flow (Phase 4.1 UI centering + quantity task):
// purely LOCAL/transient input sub-steps within the single server-
// authoritative QUANTITY_INPUT business state (§8 of the task: "do not
// necessarily add new authoritative business states if they are only local
// input substeps"). The server never sees/knows about this enum -- it only
// ever receives one final QUANTITY_SUBMITTED carrying all three values once
// the whole local flow completes (§16: no partial/early commit of GOOD
// alone). Renderer draw calls switch on this to show the right screen;
// StateProjection/BusinessState are completely unaware of it.
enum class QtyStep {
  GOOD,             // "SAN PHAM DAT" -- first step, entered every time QUANTITY_INPUT starts
  DEFECT,           // "SAN PHAM LOI" -- after GOOD confirmed
  REWORK_DECISION,  // "LOI CO SUA DUOC KHONG?" -- only reached if DEFECT > 0
  REWORK,           // "SO LUONG CAN SUA" -- only reached if repairable == YES
};

// Phase 2: "SERVER OWNS BUSINESS STATE" (docs/ARCHITECTURE.md invariants
// 13-16). The device is INPUT -> EVENT -> SERVER RESPONSE -> STATE
// PROJECTION -> RENDER: it builds Protocol Envelope v1, sends it
// ASYNCHRONOUSLY (§23 — retries must never block display/keypad/scanner),
// and renders EXACTLY whatever kiosk::protocol::StateProjection ends up
// holding once the server responds. This class makes zero business-
// transition decisions itself -- it never writes `state = X` because of
// what a scan/keypress *looked like*, only because StateProjection::apply()
// accepted a snapshot the SERVER sent.
//
// Gated on kiosk::security::DeviceIdentity::state(): only ACTIVE devices
// attempt to send business events at all (§5). UNPROVISIONED/PROVISIONING/
// SUSPENDED/REVOKED show a persistent identity screen instead of the normal
// waiting screen and reject scans with an honest IDENTITY_* error, without
// ever contacting the backend.
class KioskRuntime {
 public:
  KioskRuntime(EventBus& bus, kiosk::ui::Renderer& renderer, kiosk::storage::ConfigStore& config,
               kiosk::security::DeviceIdentity& identity, kiosk::network::TimeSync& time_sync,
               kiosk::storage::UiBundleStore& ui_bundle_store, kiosk::storage::EventJournal& journal)
      : bus_(bus),
        renderer_(renderer),
        config_(config),
        identity_(identity),
        time_sync_(time_sync),
        ui_bundle_store_(ui_bundle_store),
        journal_(journal) {}

  // `boot_id` is generated once in the .ino entry point and shared with
  // boot_diagnostics, so the diagnostics screen and every protocol event
  // this boot agree on the same id.
  void begin(const String& boot_id);

  // Registered as the EventBus subscriber.
  void handle_local_event(const LocalEvent& event);

  // Call every loop() iteration -- polls the async sender/state-fetcher for
  // a completed result and renders it. This is what keeps Display access
  // confined to the main loopTask (docs/VISUAL_DEBUG.md's frame-consistency
  // invariant): background tasks never touch the renderer directly.
  void poll();

  // Called once by the .ino right after g_bootstrap.attempt() resolves.
  // On a successful bootstrap that carries a state{}/workflow{}/view{}
  // snapshot, seeds StateProjection with it (invariant 15: the device NEVER
  // restores a locally-remembered business state across reboot -- it always
  // starts fresh from whatever the server says this boot).
  void on_bootstrap_result(const kiosk::network::BootstrapResult& result);

  // Re-renders whichever screen is currently correct: the current
  // authoritative business-state screen if StateProjection has a snapshot
  // and identity is ACTIVE, else the identity screen or the pre-bootstrap
  // waiting screen. Called after identity changes (provision/suspend/
  // revoke) and on Wi-Fi state changes.
  void refresh_idle_screen();

  // For /debug/device-state (docs/VISUAL_DEBUG.md) — deliberately minimal,
  // not a full input-history log.
  String last_scan() const { return last_scan_; }
  char last_key() const { return last_key_; }
  String device_id() const { return device_id_; }
  String hardware_id() const { return hardware_id_; }
  String boot_id() const { return boot_id_; }
  uint64_t device_seq() const { return device_seq_.current(); }
  String last_event_id() const { return last_event_id_.c_str(); }
  const char* last_event_type() const { return kiosk::protocol::event_type_to_string(last_event_type_); }
  // Reflects only the outcome of the LAST scan's network attempt, not a
  // continuous liveness probe -- honest about what Phase 0 actually knows.
  bool has_scanned() const { return has_scanned_; }
  bool last_backend_reachable() const { return last_backend_ok_; }
  int last_http_status() const { return last_http_status_; }
  String last_error_code() const { return last_error_code_.c_str(); }
  uint32_t last_latency_ms() const { return last_latency_ms_; }
  int last_retry_count() const { return last_retry_count_; }

  // --- Phase 2 diagnostics (§44-46, /debug/device-state) ---
  bool has_state_snapshot() const { return state_projection_.has_snapshot(); }
  const kiosk::protocol::StateSnapshot& current_state() const { return state_projection_.current(); }
  bool resyncing() const { return resyncing_; }
  int64_t last_server_seq() const { return last_server_seq_; }
  String local_quantity_buffer() const { return local_qty_buffer_.c_str(); }

  // --- Server environment identity (2026-08-26 UX-hardening pass, §2/§3/§4)
  // -- populated from the LAST successful bootstrap's server_role/
  // environment/version fields (app/mesflow/web/kiosk_v2.py, added the same
  // day). UNKNOWN/"" before the first bootstrap this boot ever completes.
  // Exposed publicly for the Device Info screen and /debug/device-state,
  // same visibility level as the other boot/identity accessors above.
  kiosk::protocol::Environment server_environment() const { return server_environment_; }
  bool env_mismatch() const { return env_mismatch_; }
  kiosk::protocol::Environment env_mismatch_expected() const { return env_mismatch_expected_; }
  String server_version() const { return server_version_; }
  String configured_expected_environment() const { return config_.expected_environment(); }

  // Phase 3A durable journal (shadow mode) -- read-only access for
  // /debug/device-state and the heartbeat body (status_snapshot.cpp).
  const kiosk::storage::EventJournal& journal() const { return journal_; }

  // --- Self-recovery (2026-08-25 finish-anti-stuck-recovery follow-up) ---
  // Public forwarding methods so WifiRecoveryController's recovery menu
  // (§4) can trigger these WITHOUT KioskRuntime depending on
  // WifiRecoveryController the other way -- the menu itself lives entirely
  // in WifiRecoveryController, this just gives it the two actions that
  // only KioskRuntime knows how to do.
  //
  // "2 RESYNC": same start_resync() every STATE_CONFLICT/version-regression
  // path already uses -- a no-op if identity isn't ACTIVE yet (nothing
  // meaningful to resync against) or a fetch is already in flight
  // (start_resync() itself handles that quietly).
  void request_manual_resync();
  // "4 RETURN": re-render whatever the CURRENT authoritative state actually
  // is -- reuses refresh_idle_screen() as-is (already public), named here
  // only in this comment for discoverability from the menu's call site.

  // --- Phase 3B real offline replay (2026-08-26 UX-hardening pass, §17/§19)
  // -- called once by the .ino on every Wi-Fi reconnect (same trigger point
  // that re-arms bootstrap), NOT a background timer of its own. A no-op if
  // nothing is PENDING/IN_FLIGHT or a replay is already in progress. See
  // check_offline_replay()'s own comment for why this reuses the SAME
  // single sender_/apply_event_response() path live scans use, deliberately
  // NOT a separate AsyncEventSender/completion handler.
  void start_offline_replay_if_needed();
  // Total PENDING+IN_FLIGHT in the durable journal right now -- the real,
  // persistent backlog size (not just "items left in the current replay
  // pass", which is 0 between reconnects even if a real backlog exists).
  // Same value the status bar's "Q:N" already reads every poll().
  uint32_t offline_queue_size() const {
    auto c = journal_.counts();
    return c.pending + c.in_flight;
  }
  // §4: last successful server round-trip (bootstrap or event ack), for
  // Device Info's "Last sync" field. "" if none yet this boot (never
  // fabricated -- same discipline as EventTimeInfo's own timestamp field).
  String last_sync_iso() const { return last_sync_iso_.c_str(); }
  String api_endpoint_value() const { return api_endpoint_; }

#if MESFLOW_DEBUG_API
  // DEV-only fault injection forwarding (§8) -- AsyncEventSender's hook is
  // private to KioskRuntime (sender_), so expose a narrow pass-through for
  // the serial test command.
  void force_next_task_create_failure_for_test() { sender_.force_next_task_create_failure(); }
#endif

 private:
  EventBus& bus_;
  kiosk::ui::Renderer& renderer_;
  kiosk::storage::ConfigStore& config_;
  kiosk::security::DeviceIdentity& identity_;
  kiosk::network::TimeSync& time_sync_;
  kiosk::storage::UiBundleStore& ui_bundle_store_;
  kiosk::storage::EventJournal& journal_;
  kiosk::network::AsyncEventSender sender_;
  kiosk::network::AsyncStateFetcher state_fetcher_;
  kiosk::protocol::StateProjection state_projection_;

  kiosk::protocol::DeviceSequence device_seq_;
  String device_id_;
  String hardware_id_;
  String boot_id_;
  String api_endpoint_;

  kiosk::ui::WifiIndicator wifi_indicator_ = kiosk::ui::WifiIndicator::UNKNOWN;

  String last_scan_;
  char last_key_ = '\0';
  bool has_scanned_ = false;
  bool last_backend_ok_ = false;
  int last_http_status_ = 0;
  std::string last_error_code_;
  uint32_t last_latency_ms_ = 0;
  int last_retry_count_ = 0;
  std::string last_event_id_;
  kiosk::protocol::EventType last_event_type_ = kiosk::protocol::EventType::SCAN;
  bool scan_pending_result_ = false;  // true between the immediate feedback draw and the async result arriving
  String pending_raw_code_;

  // Phase 2 additions --------------------------------------------------
  bool resyncing_ = false;           // true while a STATE_CONFLICT resync GET /state is outstanding
  int64_t last_server_seq_ = -1;     // -1 = never seen one yet
  std::string local_qty_buffer_;     // LOCAL-ONLY digit buffer for the CURRENT qty_step_ (§14: not business truth)

  // GOOD/DEFECT/REWORK quantity flow -- all local/transient (see QtyStep
  // above). Reset to GOOD/0/0 every time QUANTITY_INPUT is (re-)entered
  // (see render_current_business_state()'s QUANTITY_INPUT dispatch).
  QtyStep qty_step_ = QtyStep::GOOD;
  int32_t qty_good_ = 0;
  int32_t qty_defect_ = 0;

  // --- Self-recovery (2026-08-24) ---
  // §6/§15 of the task: draw_error_view() takes over the whole screen and
  // is otherwise only dismissed by the NEXT render call -- if nothing ever
  // triggers one (no scan, no key, no state change), the operator is stuck
  // looking at it forever. This is exactly the incident that motivated the
  // task ("Lỗi gửi sự kiện - CHƯA được lưu" with no way off the screen).
  // Tracked here, not in Renderer, since only KioskRuntime knows what a
  // safe "next" render actually is.
  bool showing_error_view_ = false;
  unsigned long error_view_shown_at_ms_ = 0;
  static constexpr unsigned long kErrorViewTimeoutMs = 20000;

  // §10: a task-creation failure (API_ERR_TASK_CREATE_FAILED, the real root
  // cause behind the incident above) gets exactly ONE retry after a bounded
  // delay -- not an immediate retry (the memory pressure that caused it
  // needs a moment, plus a chance for the periodic compaction check to
  // run), and never a silent infinite retry loop. A SECOND consecutive
  // failure for the SAME event is treated as application-level-exhausted
  // and escalates to a controlled reboot (§12) rather than leaving the
  // operator stuck.
  bool send_retry_pending_ = false;
  unsigned long send_retry_at_ms_ = 0;
  std::string retry_json_body_;
  std::string retry_event_id_;
  uint64_t retry_device_seq_ = 0;
  static constexpr unsigned long kSendRetryDelayMs = 800;

  // --- Central UI inactivity timeout (2026-08-26 ESP kiosk UX-hardening
  // pass, §13/ui_timeout_policy.h) ---
  // Reset on ANY real interaction: a scan, an accepted business keypress, or
  // a server round-trip actually landing (apply_event_response/bootstrap/
  // resync) -- not merely "time since this business state was entered",
  // since an operator actively typing a multi-digit quantity must never be
  // timed out mid-entry (see handle_business_key()'s QUANTITY_INPUT digit
  // branch, which touches this on every keystroke).
  unsigned long last_activity_ms_ = 0;
  // QUANTITY_INPUT's timeout is two-stage (see check_ui_timeout()'s own
  // comment for why a blind reset/cancel here would be wrong): first firing
  // only warns and re-arms the clock once; only a SECOND consecutive
  // timeout falls back to re-rendering the current authoritative state.
  // Cleared whenever QUANTITY_INPUT is freshly (re-)entered, same lifetime
  // as qty_step_/qty_good_/qty_defect_ (see reset_quantity_flow()).
  bool qty_timeout_warned_ = false;

  // --- Server environment identity / mismatch (2026-08-26, §2/§3) ---
  kiosk::protocol::Environment server_environment_ = kiosk::protocol::Environment::UNKNOWN;
  String server_version_;
  std::string last_sync_iso_;  // §4: last successful server round-trip, "" if none yet this boot
  // §3: env_mismatch_ blocks ALL business input (handle_scan/
  // handle_business_key both check it, render_current_business_state takes
  // over the whole screen) but is DELIBERATELY only set when BOTH sides are
  // known and disagree -- see apply_server_environment()'s own comment for
  // why a never-configured device (expected_environment=="") must degrade
  // gracefully rather than hard-block, unlike a genuine cross-environment
  // mismatch.
  bool env_mismatch_ = false;
  kiosk::protocol::Environment env_mismatch_expected_ = kiosk::protocol::Environment::UNKNOWN;

  // --- Phase 3B real offline replay (2026-08-26, §17/§19) ---
  // Own COPIES of what needs resending, not raw JournalRecord* -- the
  // journal's in-memory index can be mutated (new appends, compaction)
  // across the several poll() cycles a real replay spans, and a dangling
  // pointer into it would be a real bug. Built once per
  // start_offline_replay_if_needed() call from
  // journal_.pending_in_device_seq_order(), which already returns them in
  // the correct device_seq order.
  struct ReplayItem {
    std::string event_id;
    std::string payload;  // exact original envelope JSON -- resent verbatim, never re-derived
    uint64_t device_seq = 0;
  };
  std::vector<ReplayItem> replay_queue_;
  size_t replay_next_ = 0;  // index of the next item to send; == replay_queue_.size() means done
  bool replaying_ = false;  // true while replay_next_'s send is actually in flight

  void check_offline_replay();

  // Computes the mismatch decision from the given bootstrap result's raw
  // server_role/environment/version fields and this device's configured
  // expected_environment, updating server_environment_/server_version_/
  // env_mismatch_*. Returns true if it's now safe to apply a business-state
  // snapshot (no mismatch), false if the caller must NOT apply one (a real
  // mismatch was just detected).
  bool apply_server_environment(const kiosk::network::BootstrapResult& result);

  void check_error_view_timeout();
  void check_send_retry();
  void check_ui_timeout();

  void handle_scan(const String& raw_code, unsigned long timestamp_ms);
  void handle_business_key(char key);
  // Sends the one final QUANTITY_SUBMITTED carrying all three real values
  // (§14/§16: never derive rework from defect, never commit early/partial).
  void submit_final_quantity(int32_t good, int32_t defect, int32_t rework);
  // Resets the local quantity sub-flow to its starting step -- called
  // whenever QUANTITY_INPUT is freshly entered (finish rescan or the
  // FINISH_REQUESTED compatibility shortcut), never on every render.
  void reset_quantity_flow();
  // quantity_defect/quantity_rework default to absent -- only QUANTITY_SUBMITTED's
  // final-submit call (see submit_final_quantity()) ever sets them; SCAN/
  // FINISH_REQUESTED callers are unaffected (GOOD/DEFECT/REWORK quantity flow task).
  void send_business_event(kiosk::protocol::EventType type, const String& raw_payload,
                           const kiosk::protocol::OptionalQuantity& quantity,
                           const kiosk::protocol::OptionalQuantity& quantity_defect = {},
                           const kiosk::protocol::OptionalQuantity& quantity_rework = {});
  void apply_event_response(bool parsed, const kiosk::protocol::EventResponse& resp,
                            const std::string& event_id);
  void start_resync();
  void handle_resync_result(const kiosk::network::StateFetchOutcome& outcome);
  // is_network_error distinguishes a transport/backend-unreachable failure
  // from a business rejection (both set is_error=true) -- Phase 4.1: they
  // must render visually/message-distinct (§3 of the closure task), not
  // just differ in wording. Only meaningful when is_error is true.
  void render_current_business_state(const String& transient_message = "", bool is_error = false,
                                     bool is_network_error = false);
  String state_endpoint_url() const;
};

}  // namespace kiosk::runtime
