#include "heartbeat_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../config/runtime_config.h"
#include "../health/memory_diag.h"
#include "../health/structured_log.h"
#include "../runtime/status_snapshot.h"
#include "endpoint_utils.h"

namespace kiosk::network {

namespace {
bool g_logged_first_heartbeat = false;

// See heartbeat_client.h's sending_ comment for the full root-cause writeup:
// this used to be a single blocking HTTPClient POST performed directly on
// the caller's (main loop()) thread. Same shape as api_client.cpp's own
// SendJob/send_task_entry -- the job owns its own copies of url/body, so the
// background task never touches any shared runtime state, only its own
// local HTTPClient.
struct HeartbeatJob {
  HeartbeatClient* owner;
  String url;
  std::string body;
};

void heartbeat_task_entry(void* arg) {
  HeartbeatJob* job = static_cast<HeartbeatJob*>(arg);
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("BEFORE_HTTP_REQUEST");
#endif
  unsigned long connect_t0 = millis();
  int status = -1;
  // Plain HTTP (2026-08-24, see docs/KIOSK_V2_PLAIN_HTTP.md) -- no explicit
  // client object at all needed: http.begin(url) with a "http://" URL
  // auto-selects a plain WiFiClient internally.
  HTTPClient http;
  http.setTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  if (http.begin(job->url)) {
    http.addHeader("Content-Type", "application/json");
    status = http.POST(job->body.c_str());
    http.end();
  }
  unsigned long connect_took = millis() - connect_t0;
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

  job->owner->mark_send_done();
  delete job;
  vTaskDelete(nullptr);
}

}  // namespace

void HeartbeatClient::poll(const String& backend_url) {
  unsigned long now = millis();
  if (now - last_attempt_ms_ < HEARTBEAT_INTERVAL_MS) return;
  last_attempt_ms_ = now;

  if (WiFi.status() != WL_CONNECTED || backend_url.length() == 0) {
    return;  // best-effort telemetry -- just skip this cycle, no error UX (§43)
  }

  if (sending_) {
    // A previous heartbeat attempt is still in flight (slow network) --
    // best-effort telemetry tolerates simply skipping this cycle rather
    // than piling up overlapping requests; the next cycle tries again.
    kiosk::health::log_structured("INFO", "HEARTBEAT_SKIPPED_BUSY", "heartbeat_client",
                                  "previous heartbeat send still in flight");
    return;
  }

  String url = derive_sibling_endpoint(backend_url, "heartbeat");
  // Cheap, in-memory, no I/O -- safe to build on the calling thread exactly
  // as before. Only the actual network POST below moves to a background
  // task now.
  std::string body =
      kiosk::runtime::build_status_json(identity_, time_sync_, runtime_, keypad_, selftest_,
                                        diagnostics_, bootstrap_);

  auto* job = new HeartbeatJob{this, url, body};
  sending_ = true;
  // Same stack size as api_client.cpp's event-send task (6144, proven safe
  // there for an equivalent single HTTPClient POST) -- deliberately not
  // trimmed further without its own uxTaskGetStackHighWaterMark() evidence.
  BaseType_t created = xTaskCreate(heartbeat_task_entry, "heartbeat_send", 6144, job, 1, nullptr);
  if (created != pdPASS) {
    delete job;
    sending_ = false;
    kiosk::health::log_structured("ERROR", "API_ERR_TASK_CREATE_FAILED", "heartbeat_client",
                                  "could not spawn heartbeat task -- skipping this cycle");
  }
}

}  // namespace kiosk::network
