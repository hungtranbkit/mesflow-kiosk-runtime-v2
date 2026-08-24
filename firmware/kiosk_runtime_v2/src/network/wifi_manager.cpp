#include "wifi_manager.h"

#include <WiFi.h>

#include "../config/runtime_config.h"
#include "../health/structured_log.h"

namespace kiosk::network {

namespace {
constexpr unsigned long kRetryCooldownMs = 10000;

const char* state_name(WifiState s) {
  switch (s) {
    case WifiState::DISCONNECTED: return "DISCONNECTED";
    case WifiState::CONNECTING: return "CONNECTING";
    case WifiState::CONNECTED: return "CONNECTED";
  }
  return "UNKNOWN";
}
}  // namespace

void WifiManager::begin(const String& ssid, const String& password) {
  ssid_ = ssid;
  password_ = password;

  if (ssid_.length() == 0) {
    kiosk::health::log_structured("WARN", "NET_WIFI_NO_CREDENTIALS", "wifi_manager",
                                   "no SSID configured; staying DISCONNECTED. "
                                   "Set via config_store (Preferences).");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_.c_str(), password_.c_str());
  connect_started_ms_ = millis();
  set_state(WifiState::CONNECTING);
}

void WifiManager::poll() {
  if (ssid_.length() == 0) return;

  if (state_ == WifiState::CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      set_state(WifiState::CONNECTED);
      return;
    }
    if (millis() - connect_started_ms_ > WIFI_CONNECT_TIMEOUT_MS) {
      kiosk::health::log_structured("WARN", "NET_WIFI_TIMEOUT", "wifi_manager",
                                     "connect attempt timed out");
      WiFi.disconnect();
      set_state(WifiState::DISCONNECTED);
      next_retry_ms_ = millis() + kRetryCooldownMs;
    }
    return;
  }

  if (state_ == WifiState::CONNECTED) {
    if (WiFi.status() != WL_CONNECTED) {
      kiosk::health::log_structured("WARN", "NET_WIFI_DROPPED", "wifi_manager",
                                     "lost connection");
      set_state(WifiState::DISCONNECTED);
      next_retry_ms_ = millis() + kRetryCooldownMs;
    }
    return;
  }

  // DISCONNECTED: retry after cooldown.
  if (millis() >= next_retry_ms_) {
    WiFi.begin(ssid_.c_str(), password_.c_str());
    connect_started_ms_ = millis();
    set_state(WifiState::CONNECTING);
  }
}

void WifiManager::set_state(WifiState new_state) {
  if (new_state == state_) return;
  state_ = new_state;

  if (new_state == WifiState::CONNECTED) {
    // IP isn't part of the LocalEvent (that's just a state name for the
    // renderer/log) but it's the one thing an operator/tool needs to reach
    // MESFLOW_DEBUG_API (docs/VISUAL_DEBUG.md) -- log it directly here.
    kiosk::health::log_structured("INFO", "NET_WIFI_IP", "wifi_manager",
                                   WiFi.localIP().toString().c_str());
  }

  kiosk::runtime::LocalEvent event;
  event.kind = kiosk::runtime::LocalEventKind::WIFI_STATE;
  event.text = state_name(new_state);
  event.timestamp_ms = millis();
  bus_.publish(event);
}

}  // namespace kiosk::network
