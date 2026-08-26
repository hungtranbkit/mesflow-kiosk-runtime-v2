#pragma once

#include <Arduino.h>

#include <functional>

#include "../network/wifi_setup_portal.h"
#include "../ui/renderer.h"
#include "event_bus.h"

namespace kiosk::runtime {

// Owns the universal '*'-hold escape gesture (docs/WIFI_RECOVERY.md +
// 2026-08-25 finish-anti-stuck-recovery follow-up, §4/§5/§7): ~5s ->
// local recovery menu, keep holding to ~10s -> Wi-Fi setup portal.
// Deliberately a separate class from KioskRuntime: this is an ESP32
// BUILT-IN emergency feature that must keep working from EVERY screen
// (including SAFE_MODE, and independent of provisioning/business state) --
// it is not part of the business/session state machine and must never
// become dependent on it. The recovery menu's 2 non-local actions (retry
// network, resync) are wired in as callbacks for exactly that reason: this
// class calls them, but doesn't depend on KioskRuntime's/WifiManager's
// concrete types to do so.
class WifiRecoveryController {
 public:
  // retry_network/resync/return_to_state: the recovery menu's actions that
  // only KioskRuntime/WifiManager know how to actually perform (§4). Wi-Fi
  // setup (portal_.start()) and reboot are handled directly here -- they
  // don't need anything from the caller.
  // show_device_info: §4 (2026-08-26 UX-hardening pass) -- the recovery
  // menu's new "6" option. Same callback shape as the other three (this
  // class must not depend on KioskRuntime's concrete type to gather the
  // fields a Device Info screen needs).
  WifiRecoveryController(EventBus& bus, kiosk::ui::Renderer& renderer,
                          kiosk::network::WifiSetupPortal& portal, std::function<void()> retry_network,
                          std::function<void()> resync, std::function<void()> return_to_state,
                          std::function<void()> show_device_info)
      : bus_(bus),
        renderer_(renderer),
        portal_(portal),
        retry_network_(std::move(retry_network)),
        resync_(std::move(resync)),
        return_to_state_(std::move(return_to_state)),
        show_device_info_(std::move(show_device_info)) {}

  void begin();

  // Registered as an EventBus subscriber (alongside KioskRuntime's).
  void handle_local_event(const LocalEvent& event);

  // Call every loop() iteration — drives the hold-timer countdown even
  // between discrete key events, and lets the portal service its web/DNS
  // server and timeout while active.
  void tick();

  bool portal_active() const { return portal_.active(); }
  bool menu_active() const { return menu_active_; }

 private:
  EventBus& bus_;
  kiosk::ui::Renderer& renderer_;
  kiosk::network::WifiSetupPortal& portal_;
  std::function<void()> retry_network_;
  std::function<void()> resync_;
  std::function<void()> return_to_state_;
  std::function<void()> show_device_info_;

  bool star_held_ = false;
  unsigned long star_down_ms_ = 0;
  int last_shown_seconds_ = -1;
  // §4: the recovery menu itself -- entered at ~5s of holding, stays open
  // (independent of whether '*' is still held) until a digit selection or
  // the ~10s Wi-Fi-setup threshold is reached by continuing to hold.
  bool menu_active_ = false;
  // §4: Device Info is shown, not part of the numbered-menu-selection flow
  // -- any subsequent key returns to the menu (not all the way back to the
  // business screen), since an operator who opened it from the menu most
  // likely wants the menu back, not to lose their place entirely.
  bool device_info_active_ = false;

  void enter_menu();
  void exit_menu_to_current_state();
  void handle_menu_selection(char key);

  // Tracked from WIFI_STATE bus events (same events KioskRuntime sees) so
  // the waiting screen this controller redraws after a cancelled hold /
  // closed portal still shows an accurate Wi-Fi indicator instead of a
  // blank "--" until the next unrelated state change.
  kiosk::ui::WifiIndicator wifi_indicator_ = kiosk::ui::WifiIndicator::UNKNOWN;
};

}  // namespace kiosk::runtime
