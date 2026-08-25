#include "heartbeat_client.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include "../config/runtime_config.h"
#include "../health/memory_diag.h"
#include "../health/structured_log.h"
#include "../runtime/status_snapshot.h"
#include "endpoint_utils.h"

namespace kiosk::network {

namespace {
bool g_logged_first_heartbeat = false;
}

void HeartbeatClient::poll(const String& backend_url) {
  unsigned long now = millis();
  if (now - last_attempt_ms_ < HEARTBEAT_INTERVAL_MS) return;
  last_attempt_ms_ = now;

  if (WiFi.status() != WL_CONNECTED || backend_url.length() == 0) {
    return;  // best-effort telemetry -- just skip this cycle, no error UX (§43)
  }

  String url = derive_sibling_endpoint(backend_url, "heartbeat");
  std::string body =
      kiosk::runtime::build_status_json(identity_, time_sync_, runtime_, keypad_, selftest_,
                                        diagnostics_, bootstrap_);

  // Plain HTTP (2026-08-24, see docs/KIOSK_V2_PLAIN_HTTP.md) -- no explicit
  // client object at all needed: http.begin(url) with a "http://" URL
  // auto-selects a plain WiFiClient internally (confirmed by reading
  // HTTPClient::begin(String)/beginInternal()). The WiFiClientSecure +
  // lastError()-based TLS diagnostics that used to live here (added while
  // root-causing a "SSL - Memory allocation failed" failure) are gone along
  // with TLS itself -- that whole class of error can no longer happen on
  // this path.
  HTTPClient http;
  http.setTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) return;
  http.addHeader("Content-Type", "application/json");
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("BEFORE_HTTP_REQUEST");
#endif
  unsigned long connect_t0 = millis();
  int status = http.POST(body.c_str());
  unsigned long connect_took = millis() - connect_t0;
  http.end();
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_HTTP_REQUEST");
  if (!g_logged_first_heartbeat) {
    g_logged_first_heartbeat = true;
    kiosk::health::log_memory_snapshot("AFTER_FIRST_HEARTBEAT");
  }
#endif

  kiosk::health::log_structured(status > 0 && status < 300 ? "INFO" : "WARN", "HEARTBEAT_SENT",
                                 "heartbeat_client",
                                 (std::string("status=") + std::to_string(status) +
                                  " took_ms=" + std::to_string(connect_took))
                                     .c_str());
}

}  // namespace kiosk::network
