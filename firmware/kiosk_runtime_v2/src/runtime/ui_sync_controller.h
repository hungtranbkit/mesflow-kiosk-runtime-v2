#pragma once

#include <Arduino.h>

#include "../storage/ui_bundle_store.h"
#include "kiosk_runtime.h"

namespace kiosk::runtime {

// Owns the (rare, background) UI bundle download/verify/activate flow,
// separate from KioskRuntime's own RESYNC fetch so a UI check never
// competes with a business-state resync for priority (see
// NetworkRequestKind::UI_BUNDLE_FETCH's own comment: same worker, LOW tier,
// distinct kind).
//
// §24: UI updates must never freeze kiosk interaction. 2026-08-27 "close
// final two runtime gaps" pass: this used to run on AsyncStateFetcher's own
// per-call background FreeRTOS task (state_client.cpp) -- the LAST
// remaining per-call xTaskCreate in the whole network stack. Migrated onto
// the shared, persistent NetworkWorker via KioskRuntime's
// enqueue_ui_bundle_fetch()/take_ui_bundle_fetch_result() (same
// stash-and-collect shape BootstrapClient's own migration already
// established) -- poll() (called every loop()) still picks up the
// completed result non-blockingly and does the (fast, local)
// verify+stage+activate work, unchanged.
class UiSyncController {
 public:
  UiSyncController(kiosk::storage::UiBundleStore& store, KioskRuntime& runtime)
      : store_(store), runtime_(runtime) {}

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
  KioskRuntime& runtime_;
  // Tracked locally now (NetworkWorker has no per-kind busy() query) --
  // set true on a successful enqueue, cleared the moment poll() actually
  // collects that fetch's result (success OR failure), same lifetime the
  // old fetcher_.busy() had.
  bool download_in_flight_ = false;
  uint32_t pending_version_ = 0;
  uint32_t last_desired_version_ = 0;
};

}  // namespace kiosk::runtime
