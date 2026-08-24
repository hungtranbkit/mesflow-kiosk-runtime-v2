#pragma once

#include <Arduino.h>

#include "../runtime/event_bus.h"

namespace kiosk::network {

enum class WifiState {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
};

// Phase 0 scope: connect, report state via LocalEvent, retry on a fixed
// cooldown after a connect timeout. Real exponential backoff + jitter
// (§41) and captive-portal-free enterprise auth etc. are Phase 1 — this
// is intentionally simpler so Phase 0 doesn't need to get retry policy
// "right" before there's a backend worth retrying against.
class WifiManager {
 public:
  explicit WifiManager(kiosk::runtime::EventBus& bus) : bus_(bus) {}

  void begin(const String& ssid, const String& password);

  // Call every loop() iteration.
  void poll();

  WifiState state() const { return state_; }

 private:
  kiosk::runtime::EventBus& bus_;
  WifiState state_ = WifiState::DISCONNECTED;
  String ssid_;
  String password_;
  unsigned long connect_started_ms_ = 0;
  unsigned long next_retry_ms_ = 0;

  void set_state(WifiState new_state);
};

}  // namespace kiosk::network
