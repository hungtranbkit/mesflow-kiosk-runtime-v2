#include "api_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../config/runtime_config.h"
#include "../health/structured_log.h"

namespace kiosk::network {

namespace {

// Single blocking HTTP attempt. Sets HTTPClient's own timeouts as the first
// line of defense (usually sufficient), but see docs/VISUAL_DEBUG.md-era
// finding: these are NOT a fully reliable bound on their own -- callers
// must not depend on this alone for "never blocks too long" guarantees.
// Phase 1 keeps a per-attempt cap via HTTPClient's timeouts; the OVERALL
// send (across retries) has no hard ceiling beyond max_attempts *
// (RUNTIME_HTTP_TIMEOUT_MS + max backoff) -- acceptable now that this runs
// on a background task the caller never blocks on (§23).
//
// Plain HTTP (2026-08-24, see docs/KIOSK_V2_PLAIN_HTTP.md): this used to
// take an explicit persistent WiFiClientSecure to get TLS connection reuse
// (Phase A latency task). That experiment is REMOVED, not just idle code --
// it never actually achieved reuse in practice (a real library-level false
// negative in this ESP32 core's WiFiClientSecure::connected(), confirmed by
// reading the installed source), and the SRAM fragmentation investigation
// separately confirmed the persistent client was not itself a measurable
// contributor either way, since the WiFi driver's own ~47KB one-time init
// already dominates. Removing HTTPS from the active path entirely (a
// product decision: kiosk v2 business/event data is not confidential)
// removes the whole mbedTLS memory question at once -- back to the
// simplest possible shape HTTPClient itself supports: `http.begin(url)`
// with a plain "http://" URL auto-selects a plain WiFiClient internally
// (confirmed by reading HTTPClient::begin(String)/beginInternal() --
// scheme "http" never touches WiFiClientSecure at all), fresh per attempt,
// nothing held across calls.
// Extracts the host portion of a "http://host[:port][/path]" URL. Plain
// string scanning (no full URL-parser dependency, matching this project's
// existing endpoint_utils.h convention).
String extract_host(const String& url) {
  int host_start = url.indexOf("://");
  host_start = host_start < 0 ? 0 : host_start + 3;
  int host_end = url.indexOf('/', host_start);
  String host_and_port = host_end < 0 ? url.substring(host_start) : url.substring(host_start, host_end);
  int colon = host_and_port.indexOf(':');
  return colon < 0 ? host_and_port : host_and_port.substring(0, colon);
}

int perform_single_attempt(const String& url, const String& json_body, String* out_retry_after,
                           std::string* out_body) {
  // Real, distinct DNS_FAIL classification (2026-08-24) -- a pre-flight
  // WiFi.hostByName() check BEFORE ever attempting the TCP connect, so a
  // DNS resolution failure is reported as DNS_FAIL rather than collapsing
  // into the same TCP_CONNECT_FAIL bucket HTTPClient's own internal
  // resolve-then-connect would otherwise produce (this exact ambiguity was
  // a real diagnostic dead-end earlier in this project's history -- see
  // the WiFi/network troubleshooting notes -- before it was traced to a
  // completely different cause; this closes that ambiguity going forward
  // instead of leaving it unresolved).
  IPAddress resolved;
  if (!WiFi.hostByName(extract_host(url).c_str(), resolved)) {
    kiosk::health::log_structured("WARN", "API_ERR_DNS_FAIL", "api_client",
                                   "WiFi.hostByName() failed -- no TCP attempt made");
    return kiosk::protocol::kDnsFailMarker;
  }

  HTTPClient http;
  http.setTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    kiosk::health::log_structured("ERROR", "API_ERR_BEGIN_FAILED", "api_client",
                                   "HTTPClient::begin failed (bad URL?)");
    return -1;  // treated as TCP_CONNECT_FAIL by classify_http_result
  }
  http.addHeader("Content-Type", "application/json");

  int status = http.POST(json_body);
  if (status == 429 && out_retry_after) {
    *out_retry_after = http.header("Retry-After");
  }
  if (status > 0) {
    // Response-size guard (2026-08-26): reject on Content-Length alone,
    // before ever calling getString() -- see RUNTIME_MAX_RESPONSE_BODY_BYTES'
    // own doc comment. getSize() < 0 means unknown/chunked length, treated
    // the same as oversized (this protocol's real responses always carry a
    // real Content-Length).
    int content_length = http.getSize();
    if (content_length < 0 || content_length > RUNTIME_MAX_RESPONSE_BODY_BYTES) {
      kiosk::health::log_structured(
          "ERROR", "API_ERR_RESPONSE_TOO_LARGE", "api_client",
          (std::string("content_length=") + std::to_string(content_length)).c_str());
      http.end();
      return kiosk::protocol::kResponseTooLargeMarker;
    }
    if (out_body) *out_body = std::string(http.getString().c_str());
  }
  http.end();
  return status;
}

struct SendJob {
  AsyncEventSender* owner;
  String url;
  String json_body;
  std::string event_id;
  uint64_t device_seq;
};

// Simplicity/memory pass (2026-08-26): lowered from 5. "Choose one coherent
// [timeout] policy... foreground retries: max 1 when transient" -- with
// RUNTIME_HTTP_TIMEOUT_MS=2500ms, 5 attempts meant a single fully-exhausted
// event could take up to ~28-30s wall-clock (confirmed live) before giving
// up, which is also exactly what let a stuck-network episode take a long
// time to even reach this codebase's OWN detection threshold for it
// (KioskRuntime::consecutive_tcp_connect_fail()). 2 attempts (1 initial +
// 1 retry) bounds a genuinely bad request to ~5-6s worst case instead,
// without giving up on the single most common real case this exists for --
// a merely transient blip that succeeds on its very next try.
constexpr int kMaxAttempts = 2;

void send_task_entry(void* arg) {
  SendJob* job = static_cast<SendJob*>(arg);
  unsigned long start_ms = millis();
  kiosk::protocol::HttpOutcome outcome;
  int attempt = 1;
  std::string last_body;

  for (;;) {
    String retry_after_header;
    int status = perform_single_attempt(job->url, job->json_body, &retry_after_header, &last_body);

    kiosk::protocol::RetryAfterResult retry_after = kiosk::protocol::parse_retry_after(
        std::string(retry_after_header.c_str()));
    outcome = kiosk::protocol::classify_http_result(
        status, retry_after.present ? static_cast<int>(retry_after.seconds) : -1);

    char log_msg[112];
    snprintf(log_msg, sizeof(log_msg), "attempt=%d status=%d error_code=%s retryable=%d",
             attempt, status, outcome.error_code.c_str(), outcome.retryable ? 1 : 0);
    kiosk::health::log_structured(outcome.ok ? "INFO" : "WARN", "EVENT_SEND_ATTEMPT", "api_client",
                                   log_msg);

    if (outcome.ok || !outcome.retryable || attempt >= kMaxAttempts) break;

    uint32_t backoff_ms;
    if (outcome.error_code == "API_RATE_LIMITED" && outcome.retry_after_s > 0) {
      backoff_ms = outcome.retry_after_s * 1000;  // §21: 429 obeys Retry-After over normal backoff
    } else {
      backoff_ms = kiosk::protocol::compute_backoff_ms(attempt, RETRY_BACKOFF_BASE_MS,
                                                       RETRY_BACKOFF_MAX_MS, esp_random() % 101,
                                                       RETRY_JITTER_PCT_MAX);
    }
    kiosk::health::log_structured("INFO", "EVENT_RETRY", "api_client",
                                   (std::string("backoff_ms=") + std::to_string(backoff_ms)).c_str());
    vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    ++attempt;
  }

  SendOutcome result;
  result.outcome = outcome;
  result.total_latency_ms = static_cast<uint32_t>(millis() - start_ms);
  result.attempts = attempt;
  result.event_id = job->event_id;
  result.device_seq = job->device_seq;
  result.response_body = last_body;

  job->owner->internal_deposit_result(result);

  delete job;
  vTaskDelete(nullptr);
}

}  // namespace

AsyncEventSender::AsyncEventSender() {
  mutex_ = xSemaphoreCreateMutex();
}

bool AsyncEventSender::busy() const {
  return busy_;
}

bool AsyncEventSender::send(const String& url, const String& json_body, const std::string& event_id,
                            uint64_t device_seq) {
  if (busy_) return false;

  if (WiFi.status() != WL_CONNECTED) {
    // No point spawning a task at all -- fail fast and synchronously so the
    // caller doesn't have to wait a poll cycle to learn what NO_WIFI already
    // tells it for free.
    SendOutcome result;
    result.outcome.ok = false;
    result.outcome.error_code = "WIFI_DOWN";
    result.outcome.retryable = true;
    result.event_id = event_id;
    result.device_seq = device_seq;
    result.attempts = 0;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    pending_result_ = result;
    result_ready_ = true;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return true;
  }

  SendJob* job = new SendJob{this, url, json_body, event_id, device_seq};
  // 6144 (kept from the SRAM investigation's measured-safe value -- see
  // that report for the real uxTaskGetStackHighWaterMark() data). A plain
  // HTTP POST needs meaningfully less stack than a TLS handshake did, so
  // this is if anything MORE conservative than necessary now, not tight.
  BaseType_t created;
#if MESFLOW_DEBUG_API
  if (force_next_task_create_failure_) {
    // §8 fault injection: behave EXACTLY like a genuine xTaskCreate()
    // failure (same job cleanup, same log line, same false return) without
    // actually needing real memory exhaustion to prove the retry/escalate
    // recovery path in kiosk_runtime.cpp works.
    force_next_task_create_failure_ = false;
    kiosk::health::log_structured("WARN", "API_FAULT_INJECTED", "api_client",
                                  "forced xTaskCreate failure (DEV test hook)");
    created = pdFAIL;
  } else {
    created = xTaskCreate(send_task_entry, "event_send", 6144, job, 1, nullptr);
  }
#else
  created = xTaskCreate(send_task_entry, "event_send", 6144, job, 1, nullptr);
#endif
  if (created != pdPASS) {
    delete job;
    kiosk::health::log_structured("ERROR", "API_ERR_TASK_CREATE_FAILED", "api_client",
                                   "could not spawn send task");
    return false;
  }

  busy_ = true;
  return true;
}

void AsyncEventSender::internal_deposit_result(const SendOutcome& result) {
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  pending_result_ = result;
  result_ready_ = true;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

bool AsyncEventSender::poll(SendOutcome& out) {
  bool got_result = false;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  if (result_ready_) {
    out = pending_result_;
    result_ready_ = false;
    busy_ = false;
    got_result = true;
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return got_result;
}

}  // namespace kiosk::network
