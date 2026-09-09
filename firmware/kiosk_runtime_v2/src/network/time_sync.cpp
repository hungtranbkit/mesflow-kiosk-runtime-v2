#include "time_sync.h"

#include <time.h>

#include "../config/runtime_config.h"
#include "../health/structured_log.h"

namespace kiosk::network {

namespace {
// A epoch value below this is obviously not a real synced clock (this
// constant itself will go stale eventually -- "year 2024" as a floor is
// just "clearly not 1970", not a precise boundary).
constexpr time_t kPlausibleEpochFloor = 1704067200;  // 2024-01-01T00:00:00Z
}  // namespace

void TimeSync::begin() {
  begun_ = true;
  status_ = kiosk::protocol::TimeSyncStatus::SYNCING;
  last_attempt_uptime_ms_ = millis();
  configTime(0, 0, TIME_SYNC_NTP_SERVER);  // UTC only -- device timestamps are diagnostics-only (§16)
  kiosk::health::log_structured("INFO", "TIME_SYNC_STARTED", "time_sync", TIME_SYNC_NTP_SERVER);
}

void TimeSync::poll() {
  if (!begun_) return;

  unsigned long now = millis();
  time_t now_epoch = time(nullptr);

  if (now_epoch >= kPlausibleEpochFloor) {
    if (status_ != kiosk::protocol::TimeSyncStatus::SYNCED &&
        status_ != kiosk::protocol::TimeSyncStatus::STALE) {
      kiosk::health::log_structured("INFO", "TIME_SYNC_OK", "time_sync", "");
      last_sync_uptime_ms_ = now;  // only reset the "confirmed fresh" clock on a NEW sync
    }
    // §15: a sync older than TIME_SYNC_STALE_AFTER_S is reported STALE, not
    // SYNCED, even though the underlying clock is presumably still ticking
    // correctly -- staleness is about "haven't reconfirmed with the server
    // recently", a distinct signal worth surfacing on its own.
    status_ = (sync_age_s() > TIME_SYNC_STALE_AFTER_S) ? kiosk::protocol::TimeSyncStatus::STALE
                                                        : kiosk::protocol::TimeSyncStatus::SYNCED;
    // Even though the clock is plausible, periodically re-issue configTime()
    // while STALE so a device that's been up a long time re-confirms with
    // the server instead of just aging in place forever.
    if (status_ == kiosk::protocol::TimeSyncStatus::STALE &&
        now - last_attempt_uptime_ms_ >= TIME_SYNC_RETRY_INTERVAL_MS) {
      last_attempt_uptime_ms_ = now;
      configTime(0, 0, TIME_SYNC_NTP_SERVER);
    }
    return;
  }

  // Not yet plausible. Recompute status from how long we've been trying.
  if (status_ == kiosk::protocol::TimeSyncStatus::SYNCED) {
    // Was synced before but the clock reads implausible now -- shouldn't
    // normally happen (the RTC keeps ticking once set), but treat it
    // honestly as no longer trustworthy rather than silently keeping SYNCED.
    status_ = kiosk::protocol::TimeSyncStatus::FAILED;
    kiosk::health::log_structured("WARN", "TIME_SYNC_FAIL", "time_sync",
                                   "clock became implausible after being synced");
  }

  if (now - last_attempt_uptime_ms_ >= TIME_SYNC_RETRY_INTERVAL_MS) {
    last_attempt_uptime_ms_ = now;
    status_ = kiosk::protocol::TimeSyncStatus::SYNCING;
    configTime(0, 0, TIME_SYNC_NTP_SERVER);
  }
}

uint32_t TimeSync::sync_age_s() const {
  if (last_sync_uptime_ms_ == 0) return 0;
  return static_cast<uint32_t>((millis() - last_sync_uptime_ms_) / 1000);
}

String TimeSync::iso8601_now() const {
  if (status_ != kiosk::protocol::TimeSyncStatus::SYNCED &&
      status_ != kiosk::protocol::TimeSyncStatus::STALE) {
    return "";  // §17: never fabricate a timestamp when not trusted
  }
  time_t now_epoch = time(nullptr);
  struct tm tm_utc;
  gmtime_r(&now_epoch, &tm_utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return String(buf);
}

String TimeSync::local_hhmm() const {
  if (status_ != kiosk::protocol::TimeSyncStatus::SYNCED &&
      status_ != kiosk::protocol::TimeSyncStatus::STALE) {
    return "";  // §17 again -- same trust gate as iso8601_now()
  }
  // Offset applied HERE, not via configTime(), so the underlying clock (and
  // therefore every event timestamp and log line) stays UTC.
  time_t local_epoch = time(nullptr) + TIME_SYNC_LOCAL_UTC_OFFSET_S;
  struct tm tm_local;
  gmtime_r(&local_epoch, &tm_local);
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &tm_local);
  return String(buf);
}

}  // namespace kiosk::network
