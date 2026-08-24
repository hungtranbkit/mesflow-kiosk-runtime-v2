#pragma once

#include <Arduino.h>

#include "../network/api_client.h"
#include "../network/bootstrap_client.h"
#include "../network/state_client.h"
#include "../network/time_sync.h"
#include "../protocol/event_response.h"
#include "../protocol/ids.h"
#include "../protocol/state_projection.h"
#include "../security/device_identity.h"
#include "../storage/config_store.h"
#include "../storage/ui_bundle_store.h"
#include "../ui/renderer.h"
#include "event_bus.h"

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
               kiosk::storage::UiBundleStore& ui_bundle_store)
      : bus_(bus),
        renderer_(renderer),
        config_(config),
        identity_(identity),
        time_sync_(time_sync),
        ui_bundle_store_(ui_bundle_store) {}

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

 private:
  EventBus& bus_;
  kiosk::ui::Renderer& renderer_;
  kiosk::storage::ConfigStore& config_;
  kiosk::security::DeviceIdentity& identity_;
  kiosk::network::TimeSync& time_sync_;
  kiosk::storage::UiBundleStore& ui_bundle_store_;
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
