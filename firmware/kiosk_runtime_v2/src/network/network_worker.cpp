#include "network_worker.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

#include "../config/runtime_config.h"
#include "../health/structured_log.h"

namespace kiosk::network {

namespace {

// Same DNS pre-flight this project's api_client.cpp already used --
// distinguishes DNS_FAIL from TCP_CONNECT_FAIL rather than collapsing both
// into one ambiguous bucket (a real, previously-fixed diagnostic gap; see
// that history in retry_policy.h's own comment).
String extract_host(const char* url) {
  String u(url);
  int host_start = u.indexOf("://");
  host_start = host_start < 0 ? 0 : host_start + 3;
  int host_end = u.indexOf('/', host_start);
  String host_and_port = host_end < 0 ? u.substring(host_start) : u.substring(host_start, host_end);
  int colon = host_and_port.indexOf(':');
  return colon < 0 ? host_and_port : host_and_port.substring(0, colon);
}

// One HTTP attempt (GET if body==nullptr, POST otherwise). Applies the
// response-size guard (RUNTIME_MAX_RESPONSE_BODY_BYTES) before ever calling
// getString() -- same policy as this codebase's other HTTP call sites.
// Returns the raw status (or a negative marker) and fills out_body on a
// real 2xx-or-not response actually received.
int perform_http_attempt(const char* url, const char* body, std::string* out_body,
                         String* out_retry_after) {
  IPAddress resolved;
  if (!WiFi.hostByName(extract_host(url).c_str(), resolved)) {
    kiosk::health::log_structured("WARN", "API_ERR_DNS_FAIL", "network_worker",
                                  "WiFi.hostByName() failed -- no TCP attempt made");
    return kiosk::protocol::kDnsFailMarker;
  }

  HTTPClient http;
  http.setTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    kiosk::health::log_structured("ERROR", "API_ERR_BEGIN_FAILED", "network_worker",
                                  "HTTPClient::begin failed (bad URL?)");
    return -1;  // treated as TCP_CONNECT_FAIL by classify_http_result
  }

  int status;
  if (body != nullptr) {
    http.addHeader("Content-Type", "application/json");
    status = http.POST(body);
  } else {
    status = http.GET();
  }
  if (status == 429 && out_retry_after) {
    *out_retry_after = http.header("Retry-After");
  }

  if (status > 0) {
    int content_length = http.getSize();
    if (content_length < 0 || content_length > RUNTIME_MAX_RESPONSE_BODY_BYTES) {
      kiosk::health::log_structured(
          "ERROR", "API_ERR_RESPONSE_TOO_LARGE", "network_worker",
          (std::string("content_length=") + std::to_string(content_length)).c_str());
      http.end();
      return kiosk::protocol::kResponseTooLargeMarker;
    }
    if (out_body) *out_body = std::string(http.getString().c_str());
  }
  http.end();
  return status;
}

}  // namespace

void NetworkWorker::begin() {
  // Small, bounded depths -- see kMaxQueuedBodyBytes' own doc comment for
  // why the per-slot cost is what it is. HIGH holds foreground business
  // events + state fetch + bootstrap; in practice at most one or two of
  // these are ever pending at once (a human can't scan faster than the
  // worker drains this queue), so depth 3 is generous headroom, not a
  // guess. LOW (replay + heartbeat) similarly never needs more than a
  // couple of slots -- replay walks its own backlog one item at a time
  // already (kiosk_runtime.cpp), never bursting more than one enqueue at a
  // time, and heartbeat is paced 20s apart.
  high_queue_ = xQueueCreate(3, sizeof(NetworkRequest));
  low_queue_ = xQueueCreate(2, sizeof(NetworkRequest));
  result_mutex_ = xSemaphoreCreateMutex();

  // 6144: same stack size this codebase's own per-call tasks already
  // proved sufficient for an equivalent single HTTPClient POST (see
  // api_client.cpp's own history) -- this task does the SAME work, just
  // never exits. Priority 1 (same as the main loopTask) -- matches what
  // the per-call tasks already ran at.
  xTaskCreate(task_entry, "network_worker", 6144, this, 1, nullptr);
}

bool NetworkWorker::enqueue(NetworkPriorityTier tier, const NetworkRequest& req) {
  QueueHandle_t q = static_cast<QueueHandle_t>(tier == NetworkPriorityTier::PRIORITY_HIGH ? high_queue_ : low_queue_);
  // Non-blocking (0 ticks) -- §4 "queue full behavior must be explicit":
  // the caller gets a clean false back immediately, never a stall.
  return xQueueSend(q, &req, 0) == pdTRUE;
}

namespace {
void fill_body(NetworkRequest& req, const std::string& body) {
  size_t n = std::min(body.size(), kMaxQueuedBodyBytes - 1);
  memcpy(req.body, body.data(), n);
  req.body_len = static_cast<uint16_t>(n);
}
}  // namespace

bool NetworkWorker::enqueue_business_event(const std::string& event_id, uint64_t device_seq, const String& url,
                                           const std::string& payload) {
  NetworkRequest req;
  req.kind = NetworkRequestKind::BUSINESS_EVENT;
  strncpy(req.url, url.c_str(), kMaxQueuedUrlBytes - 1);
  strncpy(req.event_id, event_id.c_str(), kMaxQueuedEventIdBytes - 1);
  req.device_seq = device_seq;
  fill_body(req, payload);
  return enqueue(NetworkPriorityTier::PRIORITY_HIGH, req);
}

bool NetworkWorker::enqueue_offline_replay(const std::string& event_id, uint64_t device_seq, const String& url,
                                           const std::string& payload) {
  NetworkRequest req;
  req.kind = NetworkRequestKind::OFFLINE_REPLAY;
  strncpy(req.url, url.c_str(), kMaxQueuedUrlBytes - 1);
  strncpy(req.event_id, event_id.c_str(), kMaxQueuedEventIdBytes - 1);
  req.device_seq = device_seq;
  fill_body(req, payload);
  return enqueue(NetworkPriorityTier::PRIORITY_LOW, req);
}

bool NetworkWorker::enqueue_state_fetch(const String& url) {
  NetworkRequest req;
  req.kind = NetworkRequestKind::STATE_FETCH;
  strncpy(req.url, url.c_str(), kMaxQueuedUrlBytes - 1);
  return enqueue(NetworkPriorityTier::PRIORITY_HIGH, req);
}

bool NetworkWorker::enqueue_ui_bundle_fetch(const String& url) {
  NetworkRequest req;
  req.kind = NetworkRequestKind::UI_BUNDLE_FETCH;
  strncpy(req.url, url.c_str(), kMaxQueuedUrlBytes - 1);
  return enqueue(NetworkPriorityTier::PRIORITY_LOW, req);
}

bool NetworkWorker::enqueue_bootstrap(const String& url, const std::string& body) {
  NetworkRequest req;
  req.kind = NetworkRequestKind::BOOTSTRAP;
  strncpy(req.url, url.c_str(), kMaxQueuedUrlBytes - 1);
  fill_body(req, body);
  return enqueue(NetworkPriorityTier::PRIORITY_HIGH, req);
}

bool NetworkWorker::enqueue_heartbeat(const String& url, const std::string& body) {
  NetworkRequest req;
  req.kind = NetworkRequestKind::HEARTBEAT;
  strncpy(req.url, url.c_str(), kMaxQueuedUrlBytes - 1);
  fill_body(req, body);
  return enqueue(NetworkPriorityTier::PRIORITY_LOW, req);
}

bool NetworkWorker::high_priority_busy() const {
  if (executing_) return true;  // conservative: covers a LOW item currently executing too, which is fine --
                                 // callers using this to decide "should I defer" are safe deferring either way
  return uxQueueMessagesWaiting(static_cast<QueueHandle_t>(high_queue_)) > 0;
}

bool NetworkWorker::busy() const {
  if (executing_) return true;
  return uxQueueMessagesWaiting(static_cast<QueueHandle_t>(high_queue_)) > 0 ||
        uxQueueMessagesWaiting(static_cast<QueueHandle_t>(low_queue_)) > 0;
}

void NetworkWorker::deposit_result(const NetworkResult& result) {
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(result_mutex_), portMAX_DELAY);
  pending_result_ = result;
  result_ready_ = true;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(result_mutex_));
}

bool NetworkWorker::poll(NetworkResult& out) {
  bool got = false;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(result_mutex_), portMAX_DELAY);
  if (result_ready_) {
    out = pending_result_;
    result_ready_ = false;
    got = true;
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(result_mutex_));
  return got;
}

// §5 "one HTTP transaction scope": everything this function allocates
// (the local HTTPClient, the retry-loop's own locals) is destroyed when it
// returns -- nothing survives into the next request. Runs entirely on the
// one persistent worker task, never on the caller's thread.
NetworkResult NetworkWorker::execute(const NetworkRequest& req) {
  NetworkResult result;
  result.kind = req.kind;
  unsigned long start_ms = millis();

  // §8/§9 of the 2026-08-27 "Final Runtime Closure" pass: real gap found
  // during that work -- the OLD per-call AsyncEventSender::send() used to
  // fail FAST with error_code="WIFI_DOWN" (0 attempts, no HTTPClient/DNS
  // I/O at all) whenever WiFi.status() != WL_CONNECTED, so a caller never
  // had to wait out a full connect/DNS timeout just to learn what
  // WiFi.status() already said for free. That check was never carried over
  // when this worker replaced AsyncEventSender -- every request kind here
  // silently fell through to a real (slow, doomed) HTTPClient attempt,
  // which then misclassified as DNS_FAIL/TCP_CONNECT_FAIL instead of the
  // correct, fast, unambiguous WIFI_DOWN. Restored here, once, for every
  // request kind (the old code only had it in the one BUSINESS_EVENT-
  // equivalent path).
  if (WiFi.status() != WL_CONNECTED) {
    result.outcome.ok = false;
    result.outcome.error_code = "WIFI_DOWN";
    result.outcome.retryable = true;
    result.event_id = req.event_id;
    result.device_seq = req.device_seq;
    result.attempts = 0;
    result.total_latency_ms = static_cast<uint32_t>(millis() - start_ms);
    return result;
  }

  if (req.kind == NetworkRequestKind::BUSINESS_EVENT || req.kind == NetworkRequestKind::OFFLINE_REPLAY) {
    // The payload was already copied into req.body by the enqueuing side
    // (on the main thread, where journal access is safe) -- see this
    // file's header comment for the cross-thread-safety reasoning. No
    // journal access happens on this worker thread at all.
    std::string payload(req.body, req.body_len);
    result.event_id = req.event_id;
    result.device_seq = req.device_seq;

    // Same retry taxonomy this codebase always used (docs/RETRY_POLICY.md):
    // classify -> backoff+jitter -> retry, up to kMaxAttempts total.
    // kMaxAttempts=2 (simplicity pass, 2026-08-26): 1 initial + 1 retry --
    // "foreground retries: max 1 when transient."
    constexpr int kMaxAttempts = 2;
    kiosk::protocol::HttpOutcome outcome;
    int attempt = 1;
    std::string last_body;
    for (;;) {
      String retry_after_header;
      int status = perform_http_attempt(req.url, payload.c_str(), &last_body, &retry_after_header);
      kiosk::protocol::RetryAfterResult retry_after =
          kiosk::protocol::parse_retry_after(std::string(retry_after_header.c_str()));
      outcome = kiosk::protocol::classify_http_result(
          status, retry_after.present ? static_cast<int>(retry_after.seconds) : -1);

      char log_msg[112];
      snprintf(log_msg, sizeof(log_msg), "attempt=%d status=%d error_code=%s retryable=%d", attempt, status,
               outcome.error_code.c_str(), outcome.retryable ? 1 : 0);
      kiosk::health::log_structured(outcome.ok ? "INFO" : "WARN", "EVENT_SEND_ATTEMPT", "network_worker", log_msg);

      if (outcome.ok || !outcome.retryable || attempt >= kMaxAttempts) break;

      uint32_t backoff_ms;
      if (outcome.error_code == "API_RATE_LIMITED" && outcome.retry_after_s > 0) {
        backoff_ms = outcome.retry_after_s * 1000;
      } else {
        backoff_ms = kiosk::protocol::compute_backoff_ms(attempt, RETRY_BACKOFF_BASE_MS, RETRY_BACKOFF_MAX_MS,
                                                         esp_random() % 101, RETRY_JITTER_PCT_MAX);
      }
      vTaskDelay(pdMS_TO_TICKS(backoff_ms));
      ++attempt;
    }
    result.outcome = outcome;
    result.attempts = attempt;
    result.response_body = last_body;
    result.total_latency_ms = static_cast<uint32_t>(millis() - start_ms);
    return result;
  }

  // STATE_FETCH/UI_BUNDLE_FETCH/BOOTSTRAP/HEARTBEAT: single attempt, no
  // retry loop -- same as this codebase's own pre-existing behavior for all
  // of these (a resync/UI-check/bootstrap/heartbeat failing just tries
  // again on its own next natural cycle, never worth a blocking
  // retry-with-backoff here).
  std::string body_str = req.body_len > 0 ? std::string(req.body, req.body_len) : std::string();
  bool is_get = req.kind == NetworkRequestKind::STATE_FETCH || req.kind == NetworkRequestKind::UI_BUNDLE_FETCH;
  const char* body_ptr = is_get ? nullptr : body_str.c_str();
  std::string response_body;
  int status = perform_http_attempt(req.url, body_ptr, &response_body, nullptr);
  result.outcome = kiosk::protocol::classify_http_result(status);
  result.attempts = 1;
  result.response_body = response_body;
  result.total_latency_ms = static_cast<uint32_t>(millis() - start_ms);

  // HEARTBEAT's result is never consumed by any caller's poll() dispatch
  // (§43/§44: best-effort telemetry, nothing acts on the outcome) -- log it
  // HERE, unconditionally, so migrating HeartbeatClient off its own per-call
  // task onto this worker didn't silently drop the HEARTBEAT_SENT evidence
  // heartbeat_client.cpp used to log itself from inside its task.
  if (req.kind == NetworkRequestKind::HEARTBEAT) {
    kiosk::health::log_structured(
        result.outcome.ok ? "INFO" : "WARN", "HEARTBEAT_SENT", "network_worker",
        (std::string("status=") + std::to_string(result.outcome.http_status) +
         " took_ms=" + std::to_string(result.total_latency_ms))
            .c_str());
  }
  return result;
}

void NetworkWorker::task_entry(void* arg) {
  NetworkWorker* self = static_cast<NetworkWorker*>(arg);
  QueueHandle_t high = static_cast<QueueHandle_t>(self->high_queue_);
  QueueHandle_t low = static_cast<QueueHandle_t>(self->low_queue_);

  for (;;) {
    NetworkRequest req;
    bool got = false;
    // §3: HIGH always drained completely before LOW is even looked at.
    if (xQueueReceive(high, &req, 0) == pdTRUE) {
      got = true;
    } else if (xQueueReceive(low, &req, pdMS_TO_TICKS(50)) == pdTRUE) {
      // The 50ms wait here is this task's idle wait (avoids busy-spinning)
      // -- re-checks HIGH again immediately on the next loop iteration
      // regardless of whether this branch fired.
      got = true;
    }
    if (!got) continue;

    self->executing_ = true;
    NetworkResult result = self->execute(req);
    self->executing_ = false;
    self->deposit_result(result);
  }
}

}  // namespace kiosk::network
