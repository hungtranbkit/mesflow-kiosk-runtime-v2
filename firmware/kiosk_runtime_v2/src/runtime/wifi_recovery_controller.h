#pragma once

#include <Arduino.h>

#include "../network/wifi_setup_portal.h"
#include "../ui/renderer.h"
#include "event_bus.h"

namespace kiosk::runtime {

// Owns the "hold '*' for 10s -> local Wi-Fi recovery" behavior
// (docs/WIFI_RECOVERY.md). Deliberately a separate class from KioskRuntime:
// this is an ESP32 BUILT-IN emergency feature that must keep working even
// once a real backend-driven state machine exists later (Phase 2+) — it is
// not part of the business/session state machine and must never become
// dependent on it.
class WifiRecoveryController {
 public:
  WifiRecoveryController(EventBus& bus, kiosk::ui::Renderer& renderer,
                          kiosk::network::WifiSetupPortal& portal)
      : bus_(bus), renderer_(renderer), portal_(portal) {}

  void begin();

  // Registered as an EventBus subscriber (alongside KioskRuntime's).
  void handle_local_event(const LocalEvent& event);

  // Call every loop() iteration — drives the hold-timer countdown even
  // between discrete key events, and lets the portal service its web/DNS
  // server and timeout while active.
  void tick();

  bool portal_active() const { return portal_.active(); }

 private:
  EventBus& bus_;
  kiosk::ui::Renderer& renderer_;
  kiosk::network::WifiSetupPortal& portal_;

  bool star_held_ = false;
  unsigned long star_down_ms_ = 0;
  int last_shown_seconds_ = -1;

  // Tracked from WIFI_STATE bus events (same events KioskRuntime sees) so
  // the waiting screen this controller redraws after a cancelled hold /
  // closed portal still shows an accurate Wi-Fi indicator instead of a
  // blank "--" until the next unrelated state change.
  kiosk::ui::WifiIndicator wifi_indicator_ = kiosk::ui::WifiIndicator::UNKNOWN;
};

}  // namespace kiosk::runtime
