#pragma once

#include <Arduino.h>

#include "../network/state_client.h"
#include "../storage/ui_bundle_store.h"

namespace kiosk::runtime {

// Owns the (rare, background) UI bundle download/verify/activate flow,
// separate from KioskRuntime's own RESYNC fetcher so a UI check never
// competes with a business-state resync for the single-in-flight slot.
//
// §24: UI updates must never freeze kiosk interaction -- the download runs
// on AsyncStateFetcher's own background FreeRTOS task (same "never block
// display/keypad/scanner" shape as every other network call in this
// codebase); poll() (called every loop()) picks up the completed result
// non-blockingly and does the (fast, local) verify+stage+activate work.
class UiSyncController {
 public:
  explicit UiSyncController(kiosk::storage::UiBundleStore& store) : store_(store) {}

  // Compares what the server just reported (from a bootstrap/heartbeat
  // response's desired.ui_bundle_version/hash) against what's currently
  // active, and starts a background download if they differ. A no-op if a
  // download is already in flight or if ui_bundle_needs_sync() says no.
  void check_desired(const String& base_events_url, uint32_t desired_version,
                     const std::string& desired_hash);

  // Call every loop() iteration. Returns true exactly on the call where a
  // new bundle just finished activating -- a REAL bug caught live via the
  // Serial Visual Debug Fallback tooling: without this, the store's active
  // bundle switches correctly (device-state's "ui" block already reflects
  // it) but the CURRENTLY DISPLAYED screen is whatever was last drawn
  // before the switch, since nothing else triggers a redraw for a bundle
  // change with no underlying business-state transition. Caller (.ino)
  // must call KioskRuntime::refresh_idle_screen() when this returns true.
  bool poll();

  // Pure observability (debug_server's device-state "ui" block): the most
  // recent desired_version the server has reported, regardless of whether a
  // sync actually started. 0 if check_desired() has never been called yet.
  uint32_t last_desired_version() const { return last_desired_version_; }

 private:
  kiosk::storage::UiBundleStore& store_;
  kiosk::network::AsyncStateFetcher fetcher_;
  uint32_t pending_version_ = 0;
  uint32_t last_desired_version_ = 0;
};

}  // namespace kiosk::runtime
