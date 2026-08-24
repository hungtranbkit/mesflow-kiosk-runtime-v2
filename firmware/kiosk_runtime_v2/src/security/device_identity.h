#pragma once

#include <Arduino.h>

namespace kiosk::security {

// Phase 1 device identity (§4/§5/docs/PROVISIONING.md). Two DIFFERENT
// identifiers, deliberately not conflated (Phase 0's ConfigStore::device_id()
// used to auto-derive a "KIOSK-DEV-XXXX" string from the chip's MAC and
// treat that as the business identity -- exactly what §4 forbids):
//
//   hardware_id   immutable-ish, derived from the chip's eFuse MAC.
//                 Always available, never provisioned, never business-
//                 meaningful on its own -- just "which physical chip is
//                 this" for diagnostics/registration.
//
//   device_id     the logical/business identity MESFlow assigns. Empty
//                 until explicitly provisioned (DEV: `provision:<id>`
//                 serial command; real fleet: a real provisioning flow,
//                 docs/PROVISIONING.md). No shared/default fleet identity
//                 exists -- invariant 12.
//
// provisioning_state gates runtime behavior (§5): UNPROVISIONED shows a
// dedicated recovery-only screen and refuses normal scan handling;
// SUSPENDED/REVOKED refuse new business actions/API connections. The
// Wi-Fi recovery trigger (`docs/WIFI_RECOVERY.md`) works in ALL states --
// it is not gated on provisioning, since it exists to make an unreachable
// device reachable again regardless of business state.
enum class ProvisioningState {
  UNPROVISIONED,
  PROVISIONING,
  ACTIVE,
  SUSPENDED,
  REVOKED,
};

const char* provisioning_state_to_string(ProvisioningState state);

class DeviceIdentity {
 public:
  void init();

  // Derived fresh from ESP.getEfuseMac() each call (cheap, no NVS I/O) --
  // "esp32s3-<12 hex chars>", not a business identifier.
  String hardware_id() const;

  // "" if never provisioned.
  String device_id();
  ProvisioningState state();

  // DEV-path provisioning (mirrors the existing wifi:/api-endpoint: serial
  // commands) -- sets device_id and moves state to ACTIVE. A real fleet's
  // provisioning flow (docs/PROVISIONING.md) is a separate, later piece of
  // work; this is deliberately the minimum needed so Phase 1's identity
  // model isn't a no-op.
  bool provision(const String& device_id);

  // For DEV testing of the SUSPENDED/REVOKED behaviors (§5) without a real
  // backend admin action to trigger them.
  void set_state(ProvisioningState state);

 private:
  bool initialized_ = false;
};

}  // namespace kiosk::security
