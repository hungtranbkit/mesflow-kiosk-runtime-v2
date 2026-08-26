#pragma once

#include <Arduino.h>

#include <atomic>

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

  // Called by the background send task exactly once, when its HTTP attempt
  // (success or failure) is done -- see heartbeat_client.cpp's task_entry.
  // Public because the task is a free function, not a member -- same shape
  // as AsyncEventSender's internal_deposit_result().
  void mark_send_done() { sending_ = false; }

 private:
  kiosk::security::DeviceIdentity& identity_;
  TimeSync& time_sync_;
  kiosk::runtime::KioskRuntime& runtime_;
  kiosk::hardware::KeypadPcf8574& keypad_;
  const kiosk::hardware::SelfTestResult& selftest_;
  kiosk::runtime::BootDiagnostics& diagnostics_;
  const BootstrapClient& bootstrap_;

  unsigned long last_attempt_ms_ = 0;
  // §"scan latency" fix (2026-08-26): a real, confirmed root cause found
  // live -- poll() used to build the status body AND perform the HTTP POST
  // synchronously, right here on the main loop() thread, every
  // HEARTBEAT_INTERVAL_MS (20s). A single slow/hung heartbeat attempt could
  // block loop() for up to RUNTIME_HTTP_TIMEOUT_MS (5s) -- during which
  // g_scanner.poll()/g_keypad.poll()/g_runtime.poll() (the thing that
  // actually renders a completed scan's result) never ran at all. An
  // employee scan landing in that window waited out the ENTIRE heartbeat
  // stall before its own already-completed network response could even be
  // picked up and rendered -- this is a fully plausible, and on a shop-
  // floor Wi-Fi with any real packet loss/latency, a LIKELY contributor to
  // the reported >10s scan-to-name-appears cases (2 unlucky heartbeat
  // timeouts alone account for most of that budget). Fixed the same way
  // AsyncEventSender already handles /events: build the (read-only,
  // in-memory, no I/O) status body on the calling thread as before, but
  // hand the actual HTTP POST off to its own background FreeRTOS task, so
  // loop() -- and therefore every scan/keypress -- is never blocked on it.
  // `sending_` guards against overlapping heartbeat tasks if one attempt
  // happens to run long; best-effort telemetry (§43/§44) tolerates simply
  // skipping a cycle rather than queuing.
  std::atomic<bool> sending_{false};
};

}  // namespace kiosk::network
