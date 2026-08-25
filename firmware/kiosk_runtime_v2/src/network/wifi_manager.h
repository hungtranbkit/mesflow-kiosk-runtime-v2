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

  // Counts CONNECTED transitions after the first (i.e. reconnects, not the
  // initial connect) -- added 2026-08-25 for the net-diag connectivity
  // investigation, so a high count is visible as a hard number instead of
  // inferred from log scrollback.
  uint32_t reconnect_count() const { return reconnect_count_; }

  // §4/§5 of the 2026-08-25 recovery-menu follow-up: "RETRY NETWORK" from
  // the local recovery menu. A no-op unless currently DISCONNECTED and
  // waiting out the retry cooldown -- CONNECTING/CONNECTED already have
  // their own real attempt in progress/succeeded, nothing to force there.
  void retry_now();

 private:
  kiosk::runtime::EventBus& bus_;
  WifiState state_ = WifiState::DISCONNECTED;
  String ssid_;
  String password_;
  unsigned long connect_started_ms_ = 0;
  unsigned long next_retry_ms_ = 0;
  uint32_t reconnect_count_ = 0;
  bool ever_connected_ = false;

  void set_state(WifiState new_state);
};

}  // namespace kiosk::network
