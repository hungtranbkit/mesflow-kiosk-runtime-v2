// Plain C++, no Arduino.h — host-testable (see test/host/).
//
// §8 of the 2026-08-27 "Final Runtime Closure" pass: a small, explicit
// runtime network state, replacing the previous "collapse everything into
// one generic connection-error message" pattern with a coarse
// classification an operator/diagnostics screen/field log can actually act
// on -- "WIFI OK / SERVER OFFLINE" reads very differently from
// "WIFI OFFLINE" even though both used to render as the same generic
// error. Deliberately NOT a state machine with its own transition table or
// timers: it is a pure function of facts the runtime already tracks
// (WiFi.status(), the last request's outcome, a small failure-streak
// counter) -- nothing here polls, sleeps, or owns any new timing.
#pragma once

#include <cstdint>
#include <string>

namespace kiosk::network {

enum class NetworkState : uint8_t {
  ONLINE,          // last request succeeded, WiFi associated
  DEGRADED,        // WiFi associated, a request just failed but not (yet) a sustained streak -- still usable
  OFFLINE_WIFI,    // WiFi itself is not associated -- nothing server-related can be inferred yet
  OFFLINE_SERVER,  // WiFi associated, but requests have been failing repeatedly (server/network path down)
  AUTH_BLOCKED,    // last request came back 401/403 -- a credentials/authorization problem, not connectivity
};

const char* network_state_to_string(NetworkState state);

// How many CONSECUTIVE non-OK results (any request kind) before a failing-
// but-WiFi-connected device is classified OFFLINE_SERVER instead of the
// more forgiving DEGRADED ("intermittent retries but still usable", per
// the task's own example). 2 means "the same problem persisted across a
// second, independent request" -- a single blip (one dropped packet, one
// slow response) stays DEGRADED and is never alarmed as a full outage.
constexpr uint32_t kOfflineServerStreakThreshold = 2;

struct NetworkStateInputs {
  bool wifi_connected = true;
  bool last_request_ok = true;    // true if there has been no request yet this boot (nothing to be wrong about)
  int last_http_status = 0;       // 0 if no response was ever received for the last request
  uint32_t consecutive_failures = 0;  // consecutive non-OK results across any request kind, any error code
};

// Pure classification -- see each NetworkState value's own comment for the
// exact rule. Priority order (first match wins), matching the task's own
// worked examples:
//   1) WiFi not associated                       -> OFFLINE_WIFI
//   2) last response was 401/403                 -> AUTH_BLOCKED
//   3) last request failed, streak >= threshold   -> OFFLINE_SERVER
//   4) last request failed, streak < threshold    -> DEGRADED
//   5) otherwise (last request ok, or none yet)   -> ONLINE
NetworkState classify_network_state(const NetworkStateInputs& in);

}  // namespace kiosk::network
