#include "state_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../config/runtime_config.h"
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
  BaseType_t created = xTaskCreate(fetch_task_entry, "state_fetch", 8192, job, 1, nullptr);
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
