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

long ConfigStore::scanner_baud() {
  prefs.begin(kNamespace, true);
  long v = prefs.getLong("scanner_baud", 0);  // 0 = unset -> caller falls back to SCANNER_BAUD
  prefs.end();
  return v;
}

bool ConfigStore::set_scanner_baud(long baud) {
  // Real GM65 datasheet-supported bauds, not an arbitrary range.
  switch (baud) {
    case 1200: case 2400: case 4800: case 9600:
    case 19200: case 38400: case 57600: case 115200:
      break;
    default:
      return false;
  }
  prefs.begin(kNamespace, false);
  prefs.putLong("scanner_baud", baud);
  prefs.end();
  return true;
}

}  // namespace kiosk::storage
