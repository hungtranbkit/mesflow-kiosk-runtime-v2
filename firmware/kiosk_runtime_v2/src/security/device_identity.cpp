#include "device_identity.h"

#include <Preferences.h>

#include "../health/structured_log.h"

namespace kiosk::security {

namespace {
const char* kNamespace = "kiosk_identity";
Preferences prefs;

ProvisioningState state_from_string(const String& s) {
  if (s == "PROVISIONING") return ProvisioningState::PROVISIONING;
  if (s == "ACTIVE") return ProvisioningState::ACTIVE;
  if (s == "SUSPENDED") return ProvisioningState::SUSPENDED;
  if (s == "REVOKED") return ProvisioningState::REVOKED;
  return ProvisioningState::UNPROVISIONED;
}
}  // namespace

const char* provisioning_state_to_string(ProvisioningState state) {
  switch (state) {
    case ProvisioningState::UNPROVISIONED: return "UNPROVISIONED";
    case ProvisioningState::PROVISIONING: return "PROVISIONING";
    case ProvisioningState::ACTIVE: return "ACTIVE";
    case ProvisioningState::SUSPENDED: return "SUSPENDED";
    case ProvisioningState::REVOKED: return "REVOKED";
  }
  return "UNKNOWN";
}

void DeviceIdentity::init() {
  initialized_ = true;
}

String DeviceIdentity::hardware_id() const {
  char buf[24];
  uint64_t mac = ESP.getEfuseMac();
  // Full 48-bit MAC, not the truncated 16 bits Phase 0's device_id used --
  // this is the immutable hardware identifier now, so it should actually
  // be the full thing.
  snprintf(buf, sizeof(buf), "esp32s3-%012llX", static_cast<unsigned long long>(mac));
  return String(buf);
}

String DeviceIdentity::device_id() {
  prefs.begin(kNamespace, true);
  String id = prefs.getString("device_id", "");
  prefs.end();
  return id;
}

ProvisioningState DeviceIdentity::state() {
  String id = device_id();
  if (id.length() == 0) return ProvisioningState::UNPROVISIONED;

  prefs.begin(kNamespace, true);
  String s = prefs.getString("state", "ACTIVE");
  prefs.end();
  return state_from_string(s);
}

bool DeviceIdentity::provision(const String& device_id) {
  if (device_id.length() == 0) return false;
  prefs.begin(kNamespace, false);
  bool ok = prefs.putString("device_id", device_id) > 0;
  prefs.putString("state", "ACTIVE");
  prefs.end();
  if (ok) {
    kiosk::health::log_structured("INFO", "IDENTITY_PROVISIONED", "device_identity",
                                   device_id.c_str());
  }
  return ok;
}

void DeviceIdentity::set_state(ProvisioningState new_state) {
  prefs.begin(kNamespace, false);
  prefs.putString("state", provisioning_state_to_string(new_state));
  prefs.end();
  kiosk::health::log_structured("INFO", "IDENTITY_STATE_CHANGED", "device_identity",
                                 provisioning_state_to_string(new_state));
}

String DeviceIdentity::kiosk_token() {
  prefs.begin(kNamespace, true);
  String token = prefs.getString("kiosk_token", "");
  prefs.end();
  return token;
}

bool DeviceIdentity::set_kiosk_token(const String& token) {
  prefs.begin(kNamespace, false);
  bool ok = prefs.putString("kiosk_token", token) > 0 || token.length() == 0;
  prefs.end();
  // Never logs the token value itself -- same posture as wifi_password()'s
  // own handling elsewhere in this codebase (a credential, not diagnostic
  // data safe to put in structured logs).
  kiosk::health::log_structured("INFO", "IDENTITY_KIOSK_TOKEN_SET", "device_identity",
                                 token.length() > 0 ? "token stored" : "token cleared");
  return ok;
}

}  // namespace kiosk::security
