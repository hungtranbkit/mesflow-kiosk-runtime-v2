#include "wifi_recovery_controller.h"

#include "../config/runtime_config.h"
#include "../health/structured_log.h"

namespace kiosk::runtime {

void WifiRecoveryController::begin() {
  bus_.subscribe([this](const LocalEvent& event) { handle_local_event(event); });
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
        renderer_.draw_waiting_screen(wifi_indicator_);
      }
    }
    return;
  }

  if (event.kind == LocalEventKind::WIFI_RECOVERY_STATE && event.text == "INACTIVE") {
    renderer_.draw_waiting_screen(wifi_indicator_);
    return;
  }

  if (event.kind != LocalEventKind::KEY_DOWN && event.kind != LocalEventKind::KEY_UP) return;
  if (event.key != '*') return;

  if (event.kind == LocalEventKind::KEY_DOWN) {
    star_held_ = true;
    star_down_ms_ = event.timestamp_ms;
    last_shown_seconds_ = -1;
  } else {  // KEY_UP
    if (star_held_ && last_shown_seconds_ >= 0) {
      // Held long enough to have shown progress, but released before the
      // trigger threshold -- go back to the normal waiting screen rather
      // than leaving the countdown frozen on screen.
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

  if (!star_held_) return;

  unsigned long held_ms = millis() - star_down_ms_;
  if (held_ms >= WIFI_RECOVERY_HOLD_MS) {
    kiosk::health::log_structured("INFO", "NET_WIFI_RECOVERY_TRIGGERED",
                                   "wifi_recovery_controller", "held '*' for 10s");
    star_held_ = false;
    portal_.start("held_star_10s");
    return;
  }

  // Progressive feedback: nothing before 3s, then a "keep holding" message,
  // then a per-second countdown from 7 to 9 (10 is the trigger itself, shown
  // above via the AP_ACTIVE screen instead of a redundant "10" frame).
  int seconds_held = static_cast<int>(held_ms / 1000);
  int shown = (seconds_held < 3) ? 0 : seconds_held;
  if (shown != last_shown_seconds_) {
    last_shown_seconds_ = shown;
    renderer_.draw_wifi_hold_progress(shown);
  }
}

}  // namespace kiosk::runtime
