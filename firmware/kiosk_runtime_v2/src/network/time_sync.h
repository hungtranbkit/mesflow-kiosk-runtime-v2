#pragma once

#include <Arduino.h>

#include "../protocol/event_types.h"  // TimeSyncStatus

namespace kiosk::network {

// NTP/SNTP time sync (§15/§16/§17, docs/TIME_SYNC.md).
//
// §16: SERVER time is authoritative for business purposes, never the
// device's. This module exists only to give operators/logs a real
// human-readable timestamp and to compute sync_age -- device_seq remains
// the actual ordering authority regardless of sync state (docs/PROTOCOL.md).
//
// Never blocks the runtime: configTime() itself is non-blocking (starts
// the SNTP client in the background), and poll() just checks whether the
// system clock has become plausible yet. If NTP never succeeds, the kiosk
// keeps running indefinitely in UNSYNCED -- KIOSK-069.
class TimeSync {
 public:
  // Called once Wi-Fi is connected. Safe to call again (e.g. after a
  // reconnect) -- it just re-issues configTime().
  void begin();

  // Call every loop() iteration (cheap: just checks time(nullptr) and a
  // state machine, no I/O of its own beyond what configTime already kicked
  // off in the background).
  void poll();

  kiosk::protocol::TimeSyncStatus status() const { return status_; }

  // Seconds since the last successful sync observation. 0 if never synced.
  uint32_t sync_age_s() const;

  // ISO-8601 UTC string for the current time, or "" if not SYNCED/STALE
  // (§17: never fabricate a timestamp when we don't actually trust the
  // clock).
  String iso8601_now() const;

  // "HH:MM" in LOCAL time (TIME_SYNC_LOCAL_UTC_OFFSET_S) for the operator
  // clock in the status bar, or "" if the clock isn't trusted -- the same
  // §17 rule iso8601_now() follows, for the same reason: a wrong wall clock
  // on a factory-floor screen is worse than a visibly absent one, because
  // an operator will believe it. The caller draws "--:--" for "".
  String local_hhmm() const;

 private:
  kiosk::protocol::TimeSyncStatus status_ = kiosk::protocol::TimeSyncStatus::UNSYNCED;
  unsigned long last_sync_uptime_ms_ = 0;
  unsigned long last_attempt_uptime_ms_ = 0;
  bool begun_ = false;
};

}  // namespace kiosk::network
