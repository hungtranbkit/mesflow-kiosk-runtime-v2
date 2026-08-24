#include "heartbeat_client.h"

#include <HTTPClient.h>
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

  String url = derive_sibling_endpoint(backend_url, "heartbeat");
  std::string body =
      kiosk::runtime::build_status_json(identity_, time_sync_, runtime_, keypad_, selftest_,
                                        diagnostics_, bootstrap_);

  HTTPClient http;
  http.setTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) return;
  http.addHeader("Content-Type", "application/json");
  int status = http.POST(body.c_str());
  http.end();

  kiosk::health::log_structured(status > 0 && status < 300 ? "INFO" : "WARN", "HEARTBEAT_SENT",
                                 "heartbeat_client",
                                 (std::string("status=") + std::to_string(status)).c_str());
}

}  // namespace kiosk::network
