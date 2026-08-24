#pragma once

#include <Arduino.h>

#include <string>

namespace kiosk::network {

// Result of a GET /api/kiosk/v2/state attempt. `response_body` is parsed by
// the caller via kiosk::protocol::parse_state_snapshot_json -- this module
// only fetches bytes, it never interprets the state contract itself.
struct StateFetchOutcome {
  bool ok = false;         // true only for a 2xx response
  int http_status = 0;     // 0 if no response was ever received
  std::string response_body;
};

// §8/RESYNC: on STATE_CONFLICT the device must GET the authoritative
// snapshot itself rather than trust anything a conflicting event's response
// might contain. Also used right after a successful bootstrap when the
// bootstrap response itself didn't carry a usable snapshot (defensive --
// normally it does, see bootstrap_client.cpp).
//
// Same async-background-task shape as AsyncEventSender (§23: never block
// display/keypad/scanner) but single-attempt, no retry loop -- a resync GET
// is a rare, operator-visible event; if it fails, KioskRuntime just stays
// in RESYNCING and the caller can trigger fetch() again on the next poll.
class AsyncStateFetcher {
 public:
  AsyncStateFetcher();

  // Starts fetching in the background. Returns false (does NOT start a new
  // fetch) if a previous fetch is still in flight.
  bool fetch(const String& state_url);

  bool busy() const;

  // Call every loop() iteration. Returns true (and fills `out`) exactly
  // once per completed fetch. Never blocks.
  bool poll(StateFetchOutcome& out);

  // INTERNAL: called only by the background fetch task.
  void internal_deposit_result(const StateFetchOutcome& result);

 private:
  void* mutex_;  // SemaphoreHandle_t, opaque here (see api_client.h's own note)
  bool busy_ = false;
  bool result_ready_ = false;
  StateFetchOutcome pending_result_;
};

}  // namespace kiosk::network
