#include "wifi_manager.h"

#include <WiFi.h>

#include "../config/runtime_config.h"
#include "../health/memory_diag.h"
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

#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("BEFORE_WIFI_MODE_STA");
#endif
  WiFi.mode(WIFI_STA);
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_WIFI_MODE_STA");
#endif
  // Root-cause fix (2026-08-24): disable WiFi modem-sleep power save.
  // Diagnosed live against the real "3..." AP -- both the event-sender
  // (persistent WiFiClientSecure) AND heartbeat_client (fresh WiFiClientSecure
  // via http.begin(url)) failed NET_CONNECT_REFUSED in the SAME ~30s burst
  // window despite excellent RSSI (-39) and wifi_connected staying true the
  // whole time -- ruling out DNS, signal quality, and the persistent-client
  // architecture as the cause (two independent, differently-coded call sites
  // failed together). That pattern -- L2 association fine, NEW TCP/TLS
  // connection attempts stalling/failing in bursts -- is the well-documented
  // ESP32 default modem-sleep (WIFI_PS_MIN_MODEM) behavior: the radio dozes
  // between DTIM beacons and can miss/delay the handshake window for a new
  // connection, especially with some APs' beacon/DTIM timing (host laptop
  // and phone are unaffected because neither uses this power-save mode).
  // setSleep(false) keeps the radio fully awake; costs some power draw, which
  // is acceptable for a mains-powered kiosk. Must be called after mode(STA),
  // before/at begin() -- matches Espressif's own documented guidance.
  WiFi.setSleep(false);
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

void WifiManager::retry_now() {
  if (state_ != WifiState::DISCONNECTED) return;  // already trying or already connected -- nothing to force
  kiosk::health::log_structured("INFO", "NET_WIFI_RETRY_NOW", "wifi_manager",
                                "operator-requested retry from recovery menu");
  next_retry_ms_ = millis();  // poll()'s own DISCONNECTED branch fires on the very next call
}

void WifiManager::force_reconnect() {
  kiosk::health::log_structured(
      "WARN", "NET_WIFI_FORCE_RECONNECT", "wifi_manager",
      "forcing disconnect+reconnect -- sustained TCP-connect failures despite WiFi reporting CONNECTED");
  WiFi.disconnect();
  set_state(WifiState::DISCONNECTED);
  next_retry_ms_ = millis();  // retry immediately, not after the normal 10s cooldown
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
    if (ever_connected_) {
      ++reconnect_count_;
    } else {
      ever_connected_ = true;
    }
  }

  kiosk::runtime::LocalEvent event;
  event.kind = kiosk::runtime::LocalEventKind::WIFI_STATE;
  event.text = state_name(new_state);
  event.timestamp_ms = millis();
  bus_.publish(event);
}

}  // namespace kiosk::network
