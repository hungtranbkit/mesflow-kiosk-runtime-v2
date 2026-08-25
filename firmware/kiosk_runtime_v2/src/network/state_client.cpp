#include "state_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../config/runtime_config.h"
#include "../health/memory_diag.h"
#include "../health/structured_log.h"

namespace kiosk::network {

namespace {

struct FetchJob {
  AsyncStateFetcher* owner;
  String url;
};

void fetch_task_entry(void* arg) {
  FetchJob* job = static_cast<FetchJob*>(arg);

  StateFetchOutcome result;
  HTTPClient http;
  http.setTimeout(RUNTIME_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RUNTIME_HTTP_TIMEOUT_MS);
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("BEFORE_HTTP_REQUEST");
#endif
  if (http.begin(job->url)) {
    int status = http.GET();
    result.http_status = status;
    if (status > 0) {
      result.response_body = std::string(http.getString().c_str());
    }
    result.ok = status >= 200 && status < 300;
    http.end();
    kiosk::health::log_structured(result.ok ? "INFO" : "WARN", "STATE_SYNC_ATTEMPT", "state_client",
                                   (std::string("status=") + std::to_string(status)).c_str());
  } else {
    kiosk::health::log_structured("ERROR", "STATE_SYNC_FAIL", "state_client",
                                   "HTTPClient::begin failed (bad URL?)");
  }

#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_FRESH_TLS_CONNECT");
  char stack_msg[64];
  snprintf(stack_msg, sizeof(stack_msg), "task=state_fetch high_water_words=%u",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  kiosk::health::log_structured("INFO", "TASK_STACK_DIAG", "state_client", stack_msg);
#endif

  job->owner->internal_deposit_result(result);

  delete job;
  vTaskDelete(nullptr);
}

}  // namespace

AsyncStateFetcher::AsyncStateFetcher() {
  mutex_ = xSemaphoreCreateMutex();
}

bool AsyncStateFetcher::busy() const {
  return busy_;
}

bool AsyncStateFetcher::fetch(const String& state_url) {
  if (busy_) return false;

  if (WiFi.status() != WL_CONNECTED || state_url.length() == 0) {
    StateFetchOutcome result;
    result.ok = false;
    result.http_status = 0;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    pending_result_ = result;
    result_ready_ = true;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return true;
  }

  FetchJob* job = new FetchJob{this, state_url};
  // Stack shrunk from 8192 -> 6144 (2026-08-24, internal-SRAM fragmentation
  // investigation) -- same reasoning as api_client.cpp's event_send task
  // (see that comment for the measured high-water-mark data: min free ever
  // seen was 4640/8192 bytes there). This task's own high-water mark was
  // NOT independently measured this pass (it only fires on a RESYNC/
  // STATE_CONFLICT, harder to trigger on demand) -- extrapolated from
  // event_send's real measurement given near-identical shape (a single
  // HTTPClient call, GET here vs POST there, similar body size). Kept at
  // the SAME 6144 rather than shrinking further on unmeasured assumption.
  BaseType_t created = xTaskCreate(fetch_task_entry, "state_fetch", 6144, job, 1, nullptr);
  if (created != pdPASS) {
    delete job;
    kiosk::health::log_structured("ERROR", "STATE_SYNC_FAIL", "state_client",
                                   "could not spawn fetch task");
    return false;
  }

  busy_ = true;
  return true;
}

void AsyncStateFetcher::internal_deposit_result(const StateFetchOutcome& result) {
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  pending_result_ = result;
  result_ready_ = true;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

bool AsyncStateFetcher::poll(StateFetchOutcome& out) {
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
