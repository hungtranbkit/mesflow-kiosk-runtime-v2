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
int perform_single_attempt(const String& url, const String& json_body, String* out_retry_after,
                           std::string* out_body) {
  HTTPClient http;
  http.setTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    kiosk::health::log_structured("ERROR", "API_ERR_BEGIN_FAILED", "api_client",
                                   "HTTPClient::begin failed (bad URL?)");
    return -1;  // treated as NET_CONNECT_REFUSED by classify_http_result
  }
  http.addHeader("Content-Type", "application/json");

  int status = http.POST(json_body);
  if (status == 429 && out_retry_after) {
    *out_retry_after = http.header("Retry-After");
  }
  if (status > 0 && out_body) {
    *out_body = std::string(http.getString().c_str());
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

constexpr int kMaxAttempts = 5;

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

    char log_msg[96];
    snprintf(log_msg, sizeof(log_msg), "attempt=%d status=%d error_code=%s retryable=%d", attempt,
             status, outcome.error_code.c_str(), outcome.retryable ? 1 : 0);
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
    result.outcome.error_code = "NET_WIFI_DOWN";
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
  BaseType_t created = xTaskCreate(send_task_entry, "event_send", 8192, job, 1, nullptr);
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
