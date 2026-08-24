#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "../runtime/event_bus.h"
#include "../security/device_identity.h"
#include "../storage/config_store.h"

namespace kiosk::network {

// Local Wi-Fi recovery portal: an ESP-hosted AP + captive config page that
// does NOT depend on the backend being reachable, since its entire purpose
// is recovering a device that can't reach the backend. This is one of the
// "ESP32 built-in" emergency features that a backend-controlled UI bundle
// can never override (see docs/ARCHITECTURE.md "Emergency UI priority").
//
// Trigger: holding the calibrated '*' key for 10s (see kiosk_runtime.cpp /
// docs/WIFI_RECOVERY.md), not implemented in this module — this module only
// owns the AP+portal lifecycle once asked to start.
//
// Test-before-commit (a deliberate improvement over legacy, which saves and
// reboots immediately): candidate credentials are only written to
// ConfigStore, and the device only reboots into them, after a real
// WL_CONNECTED is observed. A failed attempt leaves the previously stored
// credentials untouched.
enum class WifiRecoveryState {
  INACTIVE,
  AP_ACTIVE,  // portal is up, waiting for the operator to submit a network
  TESTING,    // attempting to join the candidate network
  FAILED,     // candidate network didn't connect; portal still open to retry
};

class WifiSetupPortal {
 public:
  WifiSetupPortal(kiosk::runtime::EventBus& bus, kiosk::storage::ConfigStore& config,
                  kiosk::security::DeviceIdentity& identity)
      : bus_(bus), config_(config), identity_(identity) {}

  // Starts the AP + captive portal. `reason` is just for logging (e.g.
  // "held_star_10s", "backend_command", "no_network_menu").
  void start(const char* reason);

  // Computes (or re-computes) the AP SSID/password WITHOUT starting the
  // AP/web server -- lets a diagnostic command (e.g. serial
  // "recovery-info") tell an operator what the recovery credentials will
  // be without actually opening the portal. Named after device_id if
  // provisioned, otherwise hardware_id (§40: recovery must work regardless
  // of provisioning state, including UNPROVISIONED, so it can never depend
  // on device_id existing).
  void compute_ap_credentials();

  // Tears down the AP/portal and returns to normal Wi-Fi station mode.
  void stop(const char* reason);

  // Call every loop() iteration while active() is true.
  void poll();

  bool active() const { return state_ != WifiRecoveryState::INACTIVE; }
  WifiRecoveryState state() const { return state_; }
  String ap_ssid() const { return ap_ssid_; }
  String ap_password() const { return ap_password_; }
  String last_error() const { return last_error_; }

 private:
  kiosk::runtime::EventBus& bus_;
  kiosk::storage::ConfigStore& config_;
  kiosk::security::DeviceIdentity& identity_;
  WebServer web_{80};
  DNSServer dns_;

  WifiRecoveryState state_ = WifiRecoveryState::INACTIVE;
  String ap_ssid_;
  String ap_password_;
  String pending_ssid_;
  String pending_password_;
  unsigned long last_activity_ms_ = 0;
  unsigned long testing_started_ms_ = 0;
  String last_error_;

  void set_state(WifiRecoveryState new_state);
  void touch_activity() { last_activity_ms_ = millis(); }

  void handle_root();
  void handle_scan();
  void handle_save();
  void handle_cancel();
  void handle_captive_redirect();
};

}  // namespace kiosk::network
