// Plain C++, no Arduino.h — host-testable (see test/host/). §21-§26.
#pragma once

#include <cstdint>
#include <string>

namespace kiosk::protocol {

// §25: never expose a magic "http = -1" as the primary error representation.
// This is the structured result every network attempt should be reduced to.
struct HttpOutcome {
  bool ok = false;              // true only for a 2xx response
  int http_status = 0;          // 0 if no response was ever received
  std::string error_code;       // one of the docs/PROTOCOL.md error taxonomy codes; empty if ok
  bool retryable = false;
  uint32_t retry_after_s = 0;   // only meaningful if error_code == API_RATE_LIMITED
};

// `raw_status` is whatever the underlying HTTP client returned: a real HTTP
// status code (100-599), or a library-specific negative "transport error"
// code (e.g. ESP32 HTTPClient: -1 connection refused, -11 read timeout), or
// this codebase's own -999 hard-deadline-exceeded marker (api_client.cpp).
// `retry_after_header_s`: parsed Retry-After value if a 429 response
// carried one, else -1.
HttpOutcome classify_http_result(int raw_status, int retry_after_header_s = -1);

// §22: exponential backoff + jitter. `attempt` is 1-based (first retry = 1).
// `jitter_pct_0_100` is a caller-supplied random value in [0,100) -- kept as
// an explicit parameter (not called internally) so this stays
// deterministic and host-testable; the real caller supplies esp_random()
// on-device.
uint32_t compute_backoff_ms(int attempt, uint32_t base_ms, uint32_t max_ms,
                            uint32_t jitter_pct_0_100, uint32_t jitter_pct_max = 25);

// §21/Retry-After: numeric-seconds form only (e.g. "Retry-After: 30"). The
// HTTP-date form ("Retry-After: Wed, 21 Oct 2026 07:28:00 GMT") is NOT
// supported -- honestly out of scope for Phase 1, not silently mishandled;
// returns not-present for that form rather than guessing.
struct RetryAfterResult {
  bool present = false;
  uint32_t seconds = 0;
};
RetryAfterResult parse_retry_after(const std::string& header_value);

}  // namespace kiosk::protocol
