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

  // 2026-08-26 UX-hardening pass, §3 "Configured Target vs Actual Server":
  // the operator-declared "which environment should this device be talking
  // to" -- raw string as typed (e.g. "DEV"/"test"/"Prod"), mapped via
  // kiosk::protocol::environment_from_config_string() at the call site, not
  // here (this class is pure storage, no policy). "" (never configured) is
  // the same deliberate fail-closed default as api_endpoint() above --
  // maps to Environment::UNKNOWN, which environment_matches() never treats
  // as a match against anything.
  String expected_environment();
  void set_expected_environment(const String& env);

  void set_wifi_credentials(const String& ssid, const String& password);

  // Validates first (kiosk::protocol::validate_backend_url, host-tested);
  // only persists (atomically -- a single NVS putString) if valid. Returns
  // the validation result so the caller can report
  // CONFIG_BACKEND_NOT_SET/CONFIG_BACKEND_INVALID precisely (§42: never
  // write a partial/invalid config).
  kiosk::protocol::BackendUrlValidation set_api_endpoint(const String& url);

  // 2026-09-08: the GM65 scanner's UART baud is a setting stored inside the
  // module itself, not this firmware, and it does NOT necessarily match
  // across physical units (found live: one unit's module was reconfigured
  // to 115200 at some point, a second unit is still the factory 9600 --
  // see hardware_pins.h's SCANNER_BAUD comment). One firmware build must
  // serve both without a recompile per unit, so this is per-device NVS
  // state, not a macro. 0 (the default, never configured) means "use
  // hardware_pins.h's SCANNER_BAUD" -- callers must treat 0 as "unset",
  // the same fail-safe-default convention as api_endpoint()'s "" above.
  long scanner_baud();
  // Only accepts a small allow-list of real GM65-supported bauds (never an
  // arbitrary typed number) -- a wrong value here doesn't fail loudly like
  // set_api_endpoint()'s URL validation does; it just makes the scanner go
  // silent again with no error signal (one-way RX UART), so it's cheaper to
  // reject nonsense here than to debug it live on hardware afterward.
  // Returns false (and does not persist) if baud isn't in the allow-list.
  bool set_scanner_baud(long baud);

 private:
  bool initialized_ = false;
};

}  // namespace kiosk::storage
