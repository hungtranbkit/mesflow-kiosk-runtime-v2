#include "wifi_recovery_controller.h"

#include "../config/runtime_config.h"
#include "../health/structured_log.h"
#include "recovery_overlay.h"

namespace kiosk::runtime {

void WifiRecoveryController::begin() {
  bus_.subscribe([this](const LocalEvent& event) { handle_local_event(event); });
}

void WifiRecoveryController::enter_menu() {
  menu_active_ = true;
  set_recovery_overlay_active(true);
  kiosk::health::log_structured("INFO", "RECOVERY_MENU_OPENED", "wifi_recovery_controller",
                                "held '*' for ~5s");
  renderer_.draw_recovery_menu(wifi_indicator_);
}

void WifiRecoveryController::exit_menu_to_current_state() {
  menu_active_ = false;
  set_recovery_overlay_active(portal_.active());  // still true if the portal took over instead
  if (!portal_.active() && return_to_state_) return_to_state_();
}

void WifiRecoveryController::handle_menu_selection(char key) {
  // REAL bug found live (2026-08-25, verified via debug-input testing): a
  // digit selection did not clear star_held_ -- if the '*' KEY_UP that
  // normally accompanies a physical press was somehow missed/delayed (or,
  // as reproduced here, deliberately never sent), tick()'s hold-timer kept
  // counting in the background through and after the menu selection, and
  // could unexpectedly trigger the 10s Wi-Fi-setup portal well AFTER the
  // operator had already finished interacting with the menu. Selecting an
  // option is a deliberate, terminating action for that hold gesture --
  // require a fresh press-and-hold to trigger Wi-Fi setup afterward,
  // rather than letting a stale hold silently continue.
  star_held_ = false;
  last_shown_seconds_ = -1;

  switch (key) {
    case '1':
      kiosk::health::log_structured("INFO", "RECOVERY_MENU_SELECT", "wifi_recovery_controller",
                                    "1 RETRY NETWORK");
      if (retry_network_) retry_network_();
      exit_menu_to_current_state();
      break;
    case '2':
      kiosk::health::log_structured("INFO", "RECOVERY_MENU_SELECT", "wifi_recovery_controller",
                                    "2 RESYNC");
      if (resync_) resync_();
      // resync_() (KioskRuntime::request_manual_resync -> start_resync())
      // draws its OWN "resyncing" screen -- exit the overlay without also
      // calling return_to_state_() over top of it.
      menu_active_ = false;
      set_recovery_overlay_active(portal_.active());
      break;
    case '3':
      kiosk::health::log_structured("INFO", "RECOVERY_MENU_SELECT", "wifi_recovery_controller",
                                    "3 WIFI SETUP");
      menu_active_ = false;
      portal_.start("recovery_menu");  // set_recovery_overlay_active(true) already covers this via portal_.active()
      break;
    case '4':
      kiosk::health::log_structured("INFO", "RECOVERY_MENU_SELECT", "wifi_recovery_controller",
                                    "4 RETURN");
      exit_menu_to_current_state();
      break;
    case '5':
      kiosk::health::log_structured("WARN", "RECOVERY_MENU_SELECT", "wifi_recovery_controller",
                                    "5 REBOOT (operator-requested from recovery menu)");
      Serial.flush();
      delay(100);
      ESP.restart();
      break;
    case '6':
      // §4: Device Info -- stays under the recovery-overlay umbrella
      // (menu_active_ off, device_info_active_ on) so KioskRuntime's own
      // key handling keeps ignoring keypad input the whole time this is
      // shown, same protection the menu itself already has.
      kiosk::health::log_structured("INFO", "RECOVERY_MENU_SELECT", "wifi_recovery_controller",
                                    "6 DEVICE INFO");
      menu_active_ = false;
      device_info_active_ = true;
      if (show_device_info_) show_device_info_();
      break;
    default:
      break;  // not one of the 6 options -- ignore, stay in the menu
  }
}

void WifiRecoveryController::handle_local_event(const LocalEvent& event) {
  if (event.kind == LocalEventKind::WIFI_STATE) {
    if (event.text == "CONNECTED") wifi_indicator_ = kiosk::ui::WifiIndicator::CONNECTED;
    else if (event.text == "CONNECTING") wifi_indicator_ = kiosk::ui::WifiIndicator::CONNECTING;
    else if (event.text == "DISCONNECTED") wifi_indicator_ = kiosk::ui::WifiIndicator::DISCONNECTED;
    return;
  }

  if (portal_.active()) {
    // While the portal owns the screen/network, ignore further '*' hold
    // bookkeeping -- the operator is interacting with the web portal now,
    // not the keypad. React only to the portal's own state transitions.
    if (event.kind == LocalEventKind::WIFI_RECOVERY_STATE) {
      if (event.text == "AP_ACTIVE") {
        renderer_.draw_wifi_portal_active(portal_.ap_ssid());
      } else if (event.text == "TESTING") {
        renderer_.draw_wifi_portal_testing(portal_.ap_ssid());
      } else if (event.text == "FAILED") {
        renderer_.draw_wifi_portal_failed(portal_.last_error());
      } else if (event.text == "INACTIVE") {
        set_recovery_overlay_active(false);
        if (return_to_state_) return_to_state_();
        else renderer_.draw_waiting_screen(wifi_indicator_);
      }
    }
    return;
  }

  if (event.kind == LocalEventKind::WIFI_RECOVERY_STATE && event.text == "INACTIVE") {
    set_recovery_overlay_active(false);
    if (return_to_state_) return_to_state_();
    else renderer_.draw_waiting_screen(wifi_indicator_);
    return;
  }

  // §4: while Device Info is shown, ANY key returns to the menu (not a
  // specific selection -- there's nothing to choose here, just a way out,
  // per §12 "cancel everywhere"). Checked before the menu-selection block
  // below since device_info_active_ and menu_active_ are never both true.
  if (device_info_active_ && (event.kind == LocalEventKind::KEY_DOWN)) {
    device_info_active_ = false;
    menu_active_ = true;
    renderer_.draw_recovery_menu(wifi_indicator_);
    return;
  }

  // §4/§5: while the menu is open, digit keys 1-6 are menu selections, not
  // business input -- must be checked BEFORE the '*'-only filter below.
  if (menu_active_ && event.kind == LocalEventKind::KEY_DOWN && event.key >= '1' && event.key <= '6') {
    handle_menu_selection(event.key);
    return;
  }

  if (event.kind != LocalEventKind::KEY_DOWN && event.kind != LocalEventKind::KEY_UP) return;
  if (event.key != '*') return;

  if (event.kind == LocalEventKind::KEY_DOWN) {
    star_held_ = true;
    star_down_ms_ = event.timestamp_ms;
    last_shown_seconds_ = -1;
  } else {  // KEY_UP
    if (menu_active_) {
      // The menu stays open on release -- it waits for a digit selection,
      // not for '*' to keep being held (§4: "opened by holding *", not
      // "shown only while held").
    } else if (star_held_ && last_shown_seconds_ >= 0) {
      // Held long enough to have shown progress, but released before EVEN
      // the menu threshold -- go back to the normal screen rather than
      // leaving the countdown frozen on screen.
      renderer_.draw_waiting_screen(wifi_indicator_);
    }
    star_held_ = false;
    last_shown_seconds_ = -1;
  }
}

void WifiRecoveryController::tick() {
  if (portal_.active()) {
    portal_.poll();
    return;
  }

  if (menu_active_) return;  // menu waits for a digit key -- no timer to drive while it's open

  if (!star_held_) return;

  unsigned long held_ms = millis() - star_down_ms_;
  if (held_ms >= WIFI_RECOVERY_HOLD_MS) {
    kiosk::health::log_structured("INFO", "NET_WIFI_RECOVERY_TRIGGERED",
                                   "wifi_recovery_controller", "held '*' for 10s");
    star_held_ = false;
    menu_active_ = false;  // holding straight through the menu threshold to 10s -- portal wins, not the menu
    set_recovery_overlay_active(true);
    portal_.start("held_star_10s");
    return;
  }

  if (held_ms >= RECOVERY_MENU_HOLD_MS && !menu_active_) {
    enter_menu();
    return;
  }

  // Progressive feedback before the menu threshold: nothing before 3s, then
  // a per-second countdown up to the ~5s menu trigger.
  int seconds_held = static_cast<int>(held_ms / 1000);
  int shown = (seconds_held < 3) ? 0 : seconds_held;
  if (shown != last_shown_seconds_) {
    last_shown_seconds_ = shown;
    renderer_.draw_wifi_hold_progress(shown);
  }
}

}  // namespace kiosk::runtime
