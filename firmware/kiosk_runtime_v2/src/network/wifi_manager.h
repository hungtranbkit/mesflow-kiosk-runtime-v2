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

  // Network self-recovery (2026-08-26 field report): unlike retry_now(),
  // this is NOT a no-op while state()==CONNECTED -- that's exactly the
  // case it exists for. Reproduced live, twice: WiFi.status() kept
  // reporting WL_CONNECTED (this manager's own poll() never saw a drop to
  // act on) while every new TCP connection attempt to a verified-healthy
  // backend failed outright (TCP_CONNECT_FAIL) for minutes at a time, only
  // resolved by a full device reboot. A plain WiFi.disconnect() + fresh
  // WiFi.begin() (the SAME reconnect path a genuine drop already takes,
  // just triggered proactively instead of waiting for one) is a much
  // cheaper first thing to try than a reboot -- and, being the existing
  // path, it also naturally re-arms bootstrap/offline-replay via the
  // reconnect_count() bump the .ino already watches for.
  void force_reconnect();

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
