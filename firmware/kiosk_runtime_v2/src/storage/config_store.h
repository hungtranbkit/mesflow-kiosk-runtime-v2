#pragma once

#include <Arduino.h>

#include "../protocol/backend_url_validation.h"

namespace kiosk::storage {

// NVS-backed (Preferences) config for wifi credentials and the backend
// endpoint. Device identity (device_id/hardware_id/provisioning_state) now
// lives in kiosk::security::DeviceIdentity (Phase 1, §4) -- deliberately
// split out, not conflated here, since ConfigStore is "how do I reach the
// network" and DeviceIdentity is "who am I".
//
// This is NOT the durable_journal/bundle_store/boot_state/crc_record set
// from the target architecture (docs/OFFLINE.md) — those are Phase 3 and
// intentionally not stubbed out here (project instruction: don't create
// empty files just to look complete).
//
// DEV-ONLY posture: values are stored in plaintext NVS, no flash encryption.
// See docs/SECURITY.md.
class ConfigStore {
 public:
  void init();

  String wifi_ssid();       // default: "" (unset)
  String wifi_password();   // default: "" (unset)
  // "" means CONFIG_BACKEND_NOT_SET -- deliberately no default value that
  // looks like a real address (§3). Callers must check for "" explicitly.
  String api_endpoint();

  void set_wifi_credentials(const String& ssid, const String& password);

  // Validates first (kiosk::protocol::validate_backend_url, host-tested);
  // only persists (atomically -- a single NVS putString) if valid. Returns
  // the validation result so the caller can report
  // CONFIG_BACKEND_NOT_SET/CONFIG_BACKEND_INVALID precisely (§42: never
  // write a partial/invalid config).
  kiosk::protocol::BackendUrlValidation set_api_endpoint(const String& url);

 private:
  bool initialized_ = false;
};

}  // namespace kiosk::storage
