#include "heartbeat_client.h"

#include <WiFi.h>

#include "../config/runtime_config.h"
#include "../health/structured_log.h"
#include "../runtime/status_snapshot.h"
#include "endpoint_utils.h"

namespace kiosk::network {

void HeartbeatClient::poll(const String& backend_url) {
  unsigned long now = millis();
  if (now - last_attempt_ms_ < HEARTBEAT_INTERVAL_MS) return;
  last_attempt_ms_ = now;

  if (WiFi.status() != WL_CONNECTED || backend_url.length() == 0) {
    return;  // best-effort telemetry -- just skip this cycle, no error UX (§43)
  }

  // Request-priority policy (§3 of the 2026-08-26 "eliminate recurrent
  // server connection failures" pass): "foreground scan > ... > heartbeat"
  // -- see KioskRuntime::network_busy()'s own doc comment for the gap this
  // closes. Deferring costs nothing here (this whole call is best-effort
  // telemetry, tried again next cycle).
  if (runtime_.network_busy()) {
    kiosk::health::log_structured("INFO", "HEARTBEAT_SKIPPED_BUSY", "heartbeat_client",
                                  "deferring to an in-flight foreground send");
    return;
  }

  String url = derive_sibling_endpoint(backend_url, "heartbeat");
  // Cheap, in-memory, no I/O -- safe to build on the calling thread.
  std::string body =
      kiosk::runtime::build_status_json(identity_, time_sync_, runtime_, keypad_, selftest_,
                                        diagnostics_, bootstrap_);

  // §2-§5: no more per-call task creation -- just enqueue onto the shared
  // worker's LOW-priority tier. If the LOW queue is momentarily full
  // (offline replay backlog ahead of it), simply skip this cycle -- the
  // exact same "best-effort telemetry tolerates skipping" contract this
  // class has always had, just expressed as an enqueue-full check instead
  // of an in-flight atomic-bool check.
  bool queued = network_.enqueue_heartbeat(url, body);
  if (!queued) {
    kiosk::health::log_structured("INFO", "HEARTBEAT_SKIPPED_BUSY", "heartbeat_client",
                                  "network worker's LOW-priority queue was full");
  }
}

}  // namespace kiosk::network
