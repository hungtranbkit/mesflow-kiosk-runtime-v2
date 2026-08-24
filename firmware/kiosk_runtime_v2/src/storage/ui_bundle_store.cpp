#include "ui_bundle_store.h"

#include <Preferences.h>
#include <SPIFFS.h>
#include <mbedtls/sha256.h>

#include "../health/structured_log.h"

namespace kiosk::storage {

namespace {
const char* kNamespace = "ui_bundle";
Preferences prefs;

// Phase 4 stabilization: the bundle's raw JSON now lives on SPIFFS (the
// `default_8MB` partition table's existing, already-reserved, currently-
// unused 1.5MB "spiffs" partition at 0x670000 -- see
// $ARDUINO15/packages/esp32/hardware/esp32/3.3.11/tools/partitions/
// default_8MB.csv -- NOT a repartition, just finally using space that was
// already there, completely separate from nvs/otadata/app0/app1). NVS keeps
// ONLY small, fixed-size metadata (active slot/version/hash/schema/
// last_good_version), exactly per the "NVS: metadata only" requirement --
// this is what actually removes the ~2-2.3KB practical bundle-size ceiling
// found live during the Phase 4 visual parity pass.
const char* kSpiffsPathA = "/ui_bundle_a.json";
const char* kSpiffsPathB = "/ui_bundle_b.json";

const char* spiffs_path_for(char slot) {
  return slot == 'A' ? kSpiffsPathA : kSpiffsPathB;
}

// Returns "" (not a crash/exception) on any failure -- SPIFFS file I/O
// failing must degrade the SAME way a missing/corrupt bundle already does
// (has_active_=false, fall back to built-in hardcoded screens), never take
// down the runtime.
String read_spiffs_file(const char* path) {
  if (!SPIFFS.exists(path)) return "";
  File f = SPIFFS.open(path, "r");
  if (!f) return "";
  String out;
  out.reserve(f.size());
  while (f.available()) {
    out += static_cast<char>(f.read());
  }
  f.close();
  return out;
}

bool write_spiffs_file(const char* path, const std::string& data) {
  File f = SPIFFS.open(path, "w");
  if (!f) return false;
  size_t written = f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  f.close();
  return written == data.size();
}

// SHA-256 over `data`, hex-encoded lowercase (matches how a backend would
// naturally publish a manifest hash). One-shot API -- no incremental
// hashing needed for a bundle this small.
String sha256_hex(const std::string& data) {
  unsigned char digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest, 0);
  char hex[65];
  for (int i = 0; i < 32; ++i) {
    snprintf(hex + i * 2, 3, "%02x", digest[i]);
  }
  hex[64] = '\0';
  return String(hex);
}

String to_lower(const String& s) {
  String out = s;
  out.toLowerCase();
  return out;
}
}  // namespace

const char* ui_sync_state_to_string(UiSyncState s) {
  switch (s) {
    case UiSyncState::IDLE: return "IDLE";
    case UiSyncState::UPDATE_AVAILABLE: return "UPDATE_AVAILABLE";
    case UiSyncState::DOWNLOADING: return "DOWNLOADING";
    case UiSyncState::VERIFYING: return "VERIFYING";
    case UiSyncState::STAGED: return "STAGED";
    case UiSyncState::ACTIVATING: return "ACTIVATING";
    case UiSyncState::ACTIVE: return "ACTIVE";
    case UiSyncState::UPDATE_FAILED: return "UPDATE_FAILED";
  }
  return "IDLE";
}

const char* ui_stage_result_to_string(UiStageResult r) {
  switch (r) {
    case UiStageResult::STAGED: return "STAGED";
    case UiStageResult::HASH_MISMATCH: return "HASH_MISMATCH";
    case UiStageResult::PARSE_FAILED: return "PARSE_FAILED";
    case UiStageResult::SCHEMA_UNSUPPORTED: return "SCHEMA_UNSUPPORTED";
    case UiStageResult::STORAGE_FAILED: return "STORAGE_FAILED";
  }
  return "STORAGE_FAILED";
}

void UiBundleStore::init() {
  // `true` = format the partition if mount fails (e.g. never-before-used,
  // exactly the state every device shipped before this migration is in --
  // same "brand-new device" honesty as load_active_from_nvs() finding no
  // meta_slot yet). A format failure here is a real hardware/partition
  // problem, not something to retry into a crash loop -- log and continue
  // with has_active_=false, same fallback as any other missing bundle.
  if (!SPIFFS.begin(true)) {
    kiosk::health::log_structured("ERROR", "UI_BUNDLE_SPIFFS_MOUNT_FAILED", "ui_bundle_store",
                                   "SPIFFS.begin(format_if_failed=true) failed -- bundle storage unavailable");
  }
  initialized_ = true;
  load_active_from_nvs();
}

void UiBundleStore::load_active_from_nvs() {
  prefs.begin(kNamespace, true);
  String slot = prefs.getString("meta_slot", "");
  last_good_version_ = prefs.getUInt("last_good_ver", 0);

  if (slot.length() == 0) {
    // Brand-new device -- never activated a bundle. Honest, not an error:
    // caller falls back to the built-in hardcoded screens (§16).
    prefs.end();
    has_active_ = false;
    return;
  }
  active_slot_ = slot[0];

  String ver_key = String("meta_ver_") + active_slot_;
  String hash_key = String("meta_hash_") + active_slot_;
  String schema_key = String("meta_schema_") + active_slot_;

  active_version_ = prefs.getUInt(ver_key.c_str(), 0);
  active_hash_ = prefs.getString(hash_key.c_str(), "");
  active_schema_version_ = prefs.getUInt(schema_key.c_str(), 0);
  prefs.end();

  String raw_json = read_spiffs_file(spiffs_path_for(active_slot_));

  if (raw_json.length() == 0) {
    kiosk::health::log_structured("ERROR", "UI_BUNDLE_MISSING", "ui_bundle_store",
                                   "metadata pointed at a slot with no stored JSON on SPIFFS");
    has_active_ = false;
    return;
  }

  kiosk::protocol::UiBundle parsed;
  if (!kiosk::protocol::parse_ui_bundle_json(std::string(raw_json.c_str()), parsed)) {
    kiosk::health::log_structured("ERROR", "UI_BUNDLE_INVALID", "ui_bundle_store",
                                   "stored active bundle failed to parse at boot");
    has_active_ = false;
    return;
  }

  active_bundle_ = parsed;
  has_active_ = true;
  sync_state_ = UiSyncState::ACTIVE;
  kiosk::health::log_structured(
      "INFO", "UI_BUNDLE_LOADED", "ui_bundle_store",
      (std::string("version=") + std::to_string(active_version_) + " slot=" + active_slot_).c_str());
}

UiStageResult UiBundleStore::stage(const std::string& raw_json, uint32_t version,
                                   const std::string& expected_sha256_hex) {
  sync_state_ = UiSyncState::VERIFYING;

  String actual_hash = sha256_hex(raw_json);
  if (!expected_sha256_hex.empty() && to_lower(actual_hash) != to_lower(String(expected_sha256_hex.c_str()))) {
    last_update_error_ = "HASH_MISMATCH";
    sync_state_ = UiSyncState::UPDATE_FAILED;
    kiosk::health::log_structured("ERROR", "UI_UPDATE_HASH_MISMATCH", "ui_bundle_store",
                                   (std::string("expected=") + expected_sha256_hex +
                                    " actual=" + actual_hash.c_str())
                                       .c_str());
    return UiStageResult::HASH_MISMATCH;
  }

  kiosk::protocol::UiBundle parsed;
  if (!kiosk::protocol::parse_ui_bundle_json(raw_json, parsed)) {
    last_update_error_ = "PARSE_FAILED";
    sync_state_ = UiSyncState::UPDATE_FAILED;
    kiosk::health::log_structured("ERROR", "UI_UPDATE_PARSE_FAILED", "ui_bundle_store", "");
    return UiStageResult::PARSE_FAILED;
  }

  if (parsed.manifest.schema_version < kUiSchemaSupportedMin ||
      parsed.manifest.schema_version > kUiSchemaSupportedMax) {
    last_update_error_ = "SCHEMA_UNSUPPORTED";
    sync_state_ = UiSyncState::UPDATE_FAILED;
    kiosk::health::log_structured(
        "ERROR", "UI_UPDATE_SCHEMA_UNSUPPORTED", "ui_bundle_store",
        (std::string("schema_version=") + std::to_string(parsed.manifest.schema_version)).c_str());
    return UiStageResult::SCHEMA_UNSUPPORTED;
  }

  // Stage into whichever slot is currently INACTIVE -- the active slot's
  // NVS entries AND its SPIFFS file are never touched by this call.
  char target_slot = (active_slot_ == 'A') ? 'B' : 'A';
  String ver_key = String("meta_ver_") + target_slot;
  String hash_key = String("meta_hash_") + target_slot;
  String schema_key = String("meta_schema_") + target_slot;

  // The bulk write (raw JSON, now unbounded by NVS's ~2-2.3KB practical
  // ceiling) goes to SPIFFS; only the small fixed-size metadata goes to
  // NVS. Order matters for power-loss safety: write the file FIRST -- if
  // that fails or power is lost before the metadata commits, the inactive
  // slot's metadata still points at whatever (possibly nothing) was there
  // before, never at a half-written file.
  bool ok = write_spiffs_file(spiffs_path_for(target_slot), raw_json);
  if (ok) {
    prefs.begin(kNamespace, false);
    ok = prefs.putUInt(ver_key.c_str(), version) > 0;
    ok = ok && prefs.putString(hash_key.c_str(), actual_hash) > 0;
    ok = ok && prefs.putUInt(schema_key.c_str(), parsed.manifest.schema_version) > 0;
    prefs.end();
  }

  if (!ok) {
    last_update_error_ = "STORAGE_FAILED";
    sync_state_ = UiSyncState::UPDATE_FAILED;
    kiosk::health::log_structured("ERROR", "UI_UPDATE_STORAGE_FAILED", "ui_bundle_store",
                                   "SPIFFS write to inactive slot or NVS metadata write failed");
    return UiStageResult::STORAGE_FAILED;
  }

  staged_ = true;
  staged_slot_ = target_slot;
  staged_version_ = version;
  last_update_error_ = "";
  sync_state_ = UiSyncState::STAGED;
  kiosk::health::log_structured(
      "INFO", "UI_BUNDLE_STAGED", "ui_bundle_store",
      (std::string("version=") + std::to_string(version) + " slot=" + target_slot).c_str());
  return UiStageResult::STAGED;
}

bool UiBundleStore::activate_staged() {
  if (!staged_) {
    kiosk::health::log_structured("ERROR", "UI_ACTIVATE_FAILED", "ui_bundle_store",
                                   "activate_staged() called with nothing staged");
    return false;
  }
  sync_state_ = UiSyncState::ACTIVATING;

  // The ONE write that changes what's active. Everything the new bundle
  // needs (JSON + its own version/hash/schema) is already durably written
  // under the target slot's own keys from stage() -- this call only flips
  // which slot those keys are read from at next boot/reload. A power loss
  // before this line: old slot still selected, still fully valid. A power
  // loss after this line completes: new slot selected, already verified.
  prefs.begin(kNamespace, false);
  bool ok = prefs.putString("meta_slot", String(staged_slot_)) > 0;
  if (ok) ok = prefs.putUInt("last_good_ver", staged_version_) > 0;
  prefs.end();

  if (!ok) {
    sync_state_ = UiSyncState::UPDATE_FAILED;
    last_update_error_ = "ACTIVATE_STORAGE_FAILED";
    kiosk::health::log_structured("ERROR", "UI_ACTIVATE_FAILED", "ui_bundle_store", "metadata write failed");
    return false;
  }

  // Reload from NVS rather than trust in-memory state, so active_bundle_/
  // active_version_/etc. always reflect exactly what a fresh boot would
  // see -- no drift between "what we just wrote" and "what's actually
  // durable".
  staged_ = false;
  load_active_from_nvs();
  sync_state_ = UiSyncState::ACTIVE;
  kiosk::health::log_structured("INFO", "UI_BUNDLE_ACTIVATED", "ui_bundle_store",
                                (std::string("version=") + std::to_string(active_version_)).c_str());
  return true;
}

}  // namespace kiosk::storage
