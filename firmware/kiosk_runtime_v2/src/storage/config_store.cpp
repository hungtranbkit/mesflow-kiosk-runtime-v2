#include "config_store.h"

#include <Preferences.h>

#include "../config/runtime_config.h"

namespace kiosk::storage {

namespace {
const char* kNamespace = "kiosk_v2";
Preferences prefs;
}  // namespace

void ConfigStore::init() {
  initialized_ = true;
}

String ConfigStore::wifi_ssid() {
  prefs.begin(kNamespace, true);
  String v = prefs.getString("wifi_ssid", "");
  prefs.end();
  return v;
}

String ConfigStore::wifi_password() {
  prefs.begin(kNamespace, true);
  String v = prefs.getString("wifi_pass", "");
  prefs.end();
  return v;
}

String ConfigStore::api_endpoint() {
  prefs.begin(kNamespace, true);
  // No default that looks like a real address (§3) -- "" means
  // CONFIG_BACKEND_NOT_SET, and every caller must treat it that way.
  String v = prefs.getString("api_endpoint", "");
  prefs.end();
  return v;
}

String ConfigStore::expected_environment() {
  prefs.begin(kNamespace, true);
  String v = prefs.getString("expected_env", "");
  prefs.end();
  return v;
}

void ConfigStore::set_expected_environment(const String& env) {
  prefs.begin(kNamespace, false);
  prefs.putString("expected_env", env);
  prefs.end();
}

void ConfigStore::set_wifi_credentials(const String& ssid, const String& password) {
  prefs.begin(kNamespace, false);
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", password);
  prefs.end();
}

kiosk::protocol::BackendUrlValidation ConfigStore::set_api_endpoint(const String& url) {
  auto result = kiosk::protocol::validate_backend_url(url.c_str(), BACKEND_URL_ALLOW_HTTP,
                                                       BACKEND_URL_MAX_LENGTH);
  if (result != kiosk::protocol::BackendUrlValidation::OK) return result;

  prefs.begin(kNamespace, false);
  prefs.putString("api_endpoint", url);
  prefs.end();
  return kiosk::protocol::BackendUrlValidation::OK;
}

}  // namespace kiosk::storage
