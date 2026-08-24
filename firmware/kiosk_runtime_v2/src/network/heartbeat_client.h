#pragma once

#include <Arduino.h>

#include "../hardware/hardware_selftest.h"
#include "../hardware/keypad_pcf8574.h"
#include "../runtime/boot_diagnostics.h"
#include "../runtime/kiosk_runtime.h"
#include "../security/device_identity.h"
#include "bootstrap_client.h"
#include "time_sync.h"

namespace kiosk::network {

// §43/§44: TELEMETRY, best-effort, NOT durable, never queued with business
// EVENTs (§14/§6 of docs/ARCHITECTURE.md's invariants). A failed heartbeat
// just tries again next cycle -- no retry/backoff of its own, no journal.
class HeartbeatClient {
 public:
  HeartbeatClient(kiosk::security::DeviceIdentity& identity, TimeSync& time_sync,
                  kiosk::runtime::KioskRuntime& runtime, kiosk::hardware::KeypadPcf8574& keypad,
                  const kiosk::hardware::SelfTestResult& selftest,
                  kiosk::runtime::BootDiagnostics& diagnostics, const BootstrapClient& bootstrap)
      : identity_(identity),
        time_sync_(time_sync),
        runtime_(runtime),
        keypad_(keypad),
        selftest_(selftest),
        diagnostics_(diagnostics),
        bootstrap_(bootstrap) {}

  // Call every loop() iteration; internally paced by HEARTBEAT_INTERVAL_MS.
  void poll(const String& backend_url);

 private:
  kiosk::security::DeviceIdentity& identity_;
  TimeSync& time_sync_;
  kiosk::runtime::KioskRuntime& runtime_;
  kiosk::hardware::KeypadPcf8574& keypad_;
  const kiosk::hardware::SelfTestResult& selftest_;
  kiosk::runtime::BootDiagnostics& diagnostics_;
  const BootstrapClient& bootstrap_;

  unsigned long last_attempt_ms_ = 0;
};

}  // namespace kiosk::network
