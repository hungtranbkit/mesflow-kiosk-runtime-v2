#pragma once

#include <Arduino.h>

#include "../hardware/hardware_selftest.h"
#include "../hardware/keypad_pcf8574.h"
#include "../runtime/boot_diagnostics.h"
#include "../runtime/kiosk_runtime.h"
#include "../security/device_identity.h"
#include "bootstrap_client.h"
#include "network_worker.h"
#include "time_sync.h"

namespace kiosk::network {

// §43/§44: TELEMETRY, best-effort, NOT durable, never queued with business
// EVENTs (§14/§6 of docs/ARCHITECTURE.md's invariants). A failed heartbeat
// just tries again next cycle -- no retry/backoff of its own, no journal.
//
// 2026-08-26 "eliminate recurrent server connection failures" pass (§2-§5):
// no longer spawns its own per-call FreeRTOS task -- the HTTP POST is
// enqueued onto the shared NetworkWorker's LOW-priority tier instead (§3:
// heartbeat is the lowest-priority traffic, always deferred behind any
// foreground business event/state fetch/bootstrap/offline replay). This
// class no longer tracks or polls its own result at all: the worker's
// result slot is only ever consumed by KioskRuntime::poll() today (its
// dispatch-by-kind switch simply has no HEARTBEAT case yet, so a heartbeat
// result is silently dropped after logging inside the worker itself --
// acceptable, since §43/§44 already say a heartbeat's outcome is telemetry
// only and nothing in this codebase acts on it).
class HeartbeatClient {
 public:
  HeartbeatClient(kiosk::security::DeviceIdentity& identity, TimeSync& time_sync,
                  kiosk::runtime::KioskRuntime& runtime, kiosk::hardware::KeypadPcf8574& keypad,
                  const kiosk::hardware::SelfTestResult& selftest,
                  kiosk::runtime::BootDiagnostics& diagnostics, const BootstrapClient& bootstrap,
                  NetworkWorker& network)
      : identity_(identity),
        time_sync_(time_sync),
        runtime_(runtime),
        keypad_(keypad),
        selftest_(selftest),
        diagnostics_(diagnostics),
        bootstrap_(bootstrap),
        network_(network) {}

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
  NetworkWorker& network_;

  unsigned long last_attempt_ms_ = 0;
};

}  // namespace kiosk::network
