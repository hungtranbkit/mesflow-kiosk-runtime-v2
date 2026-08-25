#include "ui_sync_controller.h"

#include "../health/memory_diag.h"
#include "../health/structured_log.h"
#include "../network/endpoint_utils.h"
#include "../protocol/json_extract.h"
#include "../protocol/ui_bundle.h"

namespace kiosk::runtime {

void UiSyncController::check_desired(const String& base_events_url, uint32_t desired_version,
                                     const std::string& desired_hash) {
  last_desired_version_ = desired_version;
  if (fetcher_.busy()) return;  // a download/resync is already in flight; try again next cycle

  bool needs_sync = kiosk::protocol::ui_bundle_needs_sync(
      store_.active_version(), std::string(store_.active_hash().c_str()), desired_version, desired_hash);
  if (!needs_sync) return;

  store_.set_sync_state(kiosk::storage::UiSyncState::UPDATE_AVAILABLE);
  kiosk::health::log_structured(
      "INFO", "UI_UPDATE_AVAILABLE", "ui_sync_controller",
      (std::string("local_version=") + std::to_string(store_.active_version()) +
       " desired_version=" + std::to_string(desired_version))
          .c_str());

  String bundle_base = kiosk::network::derive_sibling_endpoint(base_events_url, "ui-bundles");
  if (bundle_base.length() == 0) return;
  String url = bundle_base + "/" + String(desired_version);

  pending_version_ = desired_version;
  store_.set_sync_state(kiosk::storage::UiSyncState::DOWNLOADING);
  bool started = fetcher_.fetch(url);
  kiosk::health::log_structured(started ? "INFO" : "WARN", "UI_UPDATE_DOWNLOAD_START", "ui_sync_controller",
                                url.c_str());
}

bool UiSyncController::poll() {
  kiosk::network::StateFetchOutcome result;
  if (!fetcher_.poll(result)) return false;

  if (!result.ok || result.response_body.empty()) {
    store_.set_sync_state(kiosk::storage::UiSyncState::UPDATE_FAILED);
    kiosk::health::log_structured(
        "WARN", "UI_UPDATE_DOWNLOAD_FAILED", "ui_sync_controller",
        (std::string("http_status=") + std::to_string(result.http_status)).c_str());
    // §25: continue using whatever is currently active -- no partial
    // state, no retry storm here; check_desired() will naturally try again
    // on the next bootstrap/heartbeat cycle.
    return false;
  }

  // The manifest's own hash is the ground truth for THIS payload; the
  // server-reported "desired hash" from bootstrap/heartbeat was only used
  // to decide WHETHER to download at all. stage() re-derives and checks
  // sha256(body) against the manifest's declared hash, not against the
  // desired-state hint, so a stale/wrong hint can't cause a bad activation.
  std::string manifest_json = kiosk::protocol::json_extract_object(result.response_body, "manifest");
  std::string expected_hash = kiosk::protocol::json_extract_string(manifest_json, "sha256", "");

  auto stage_result = store_.stage(result.response_body, pending_version_, expected_hash);
  if (stage_result != kiosk::storage::UiStageResult::STAGED) {
    kiosk::health::log_structured(
        "ERROR", "UI_UPDATE_STAGE_FAILED", "ui_sync_controller",
        kiosk::storage::ui_stage_result_to_string(stage_result));
    return false;  // §26: reject, continue on last-known-good -- already logged with the specific reason
  }

  bool activated = store_.activate_staged();
  kiosk::health::log_structured(activated ? "INFO" : "ERROR",
                                activated ? "UI_UPDATE_ACTIVATED" : "UI_ACTIVATE_FAILED",
                                "ui_sync_controller",
                                (std::string("version=") + std::to_string(pending_version_)).c_str());
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_UI_BUNDLE_SYNC");
#endif
  return activated;
}

}  // namespace kiosk::runtime
