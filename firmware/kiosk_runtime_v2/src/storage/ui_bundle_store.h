#pragma once

#include <Arduino.h>

#include "../protocol/ui_bundle.h"

namespace kiosk::storage {

// This runtime's supported UI schema range (§27's compatibility matrix,
// device side). A bundle whose schema_version falls outside this range is
// rejected at stage() time, before it ever gets near the active slot.
constexpr uint32_t kUiSchemaSupportedMin = 1;
constexpr uint32_t kUiSchemaSupportedMax = 1;

enum class UiSyncState {
  IDLE,
  UPDATE_AVAILABLE,
  DOWNLOADING,
  VERIFYING,
  STAGED,
  ACTIVATING,
  ACTIVE,
  UPDATE_FAILED,
};
const char* ui_sync_state_to_string(UiSyncState s);

enum class UiStageResult {
  STAGED,
  HASH_MISMATCH,
  PARSE_FAILED,
  SCHEMA_UNSUPPORTED,
  STORAGE_FAILED,
};
const char* ui_stage_result_to_string(UiStageResult r);

// Two-slot, atomic-activation UI bundle store (§5/§28 of the Phase 4 spec):
// a new bundle is verified and written to the INACTIVE slot only; the
// active slot is never touched until a single small metadata write flips
// which slot is "active". A power loss at any point before that single
// write leaves the previously-active slot completely intact and still
// selected.
//
// NVS access happens in init(), called explicitly from setup() -- NOT in
// this class's constructor (same reservation-block-style rule already
// established for DeviceSequence/ids.h: C++ global static initializers run
// before Arduino's own nvs_flash_init()).
class UiBundleStore {
 public:
  void init();

  // False on a brand-new device that has never activated a bundle --
  // caller falls back to the built-in hardcoded screens (§16 "factory
  // default"), never blocks waiting for one.
  bool has_active_bundle() const { return has_active_; }
  const kiosk::protocol::UiBundle& active_bundle() const { return active_bundle_; }

  uint32_t active_version() const { return active_version_; }
  const String& active_hash() const { return active_hash_; }
  uint32_t active_schema_version() const { return active_schema_version_; }
  char active_slot() const { return active_slot_; }
  uint32_t last_good_version() const { return last_good_version_; }

  UiSyncState sync_state() const { return sync_state_; }
  void set_sync_state(UiSyncState s) { sync_state_ = s; }
  const String& last_update_error() const { return last_update_error_; }

  // Verifies sha256(raw_json) == expected_sha256_hex (case-insensitive
  // hex), parses it, and checks schema_version is within
  // [kUiSchemaSupportedMin, kUiSchemaSupportedMax]. On ALL checks passing,
  // writes the bundle to the CURRENTLY-INACTIVE slot (never the active
  // one) and returns STAGED. On any failure, the active bundle/slot is
  // completely untouched; sync_state becomes UPDATE_FAILED and
  // last_update_error() explains why.
  UiStageResult stage(const std::string& raw_json, uint32_t version, const std::string& expected_sha256_hex);

  // Flips the active-slot metadata pointer to whichever slot stage() just
  // wrote to -- the ONE write that actually changes what's active. Must
  // only be called after stage() returned STAGED for the version being
  // activated. Returns false (and leaves the old slot active) if called
  // out of order or if the NVS write itself fails.
  bool activate_staged();

 private:
  bool initialized_ = false;
  bool has_active_ = false;
  kiosk::protocol::UiBundle active_bundle_;
  uint32_t active_version_ = 0;
  String active_hash_;
  uint32_t active_schema_version_ = 0;
  char active_slot_ = 'A';
  uint32_t last_good_version_ = 0;

  bool staged_ = false;
  char staged_slot_ = 'B';
  uint32_t staged_version_ = 0;

  UiSyncState sync_state_ = UiSyncState::IDLE;
  String last_update_error_;

  void load_active_from_nvs();
};

}  // namespace kiosk::storage
