#include "retry_policy.h"

#include <cctype>
#include <cstdlib>

namespace kiosk::protocol {

HttpOutcome classify_http_result(int raw_status, int retry_after_header_s) {
  HttpOutcome out;
  out.http_status = raw_status > 0 ? raw_status : 0;

  if (raw_status >= 200 && raw_status < 300) {
    out.ok = true;
    return out;
  }

  if (raw_status == kResponseTooLargeMarker) {
    // Response-size guard (2026-08-26): rejected on Content-Length alone,
    // before ever reading the body. Deliberately NOT retryable -- a
    // pathologically large response from a given endpoint is a server-side
    // condition (bug/misconfiguration/compromise), not a transient network
    // blip that a retry would ever fix.
    out.error_code = "RESPONSE_TOO_LARGE";
    out.retryable = false;
    return out;
  }

  if (raw_status <= 0) {
    // Transport-level failure (no HTTP status ever received). Map known
    // ESP32 HTTPClient negative codes to the simplified taxonomy; anything
    // else buckets to the generic TCP_CONNECT_FAIL/HTTP_TIMEOUT as
    // appropriate.
    switch (raw_status) {
      case kDnsFailMarker:  // pre-flight WiFi.hostByName() failed -- no TCP attempt was even made
        out.error_code = "DNS_FAIL";
        break;
      case -11:  // HTTPC_ERROR_READ_TIMEOUT
        out.error_code = "HTTP_TIMEOUT";
        break;
      case -1:   // HTTPC_ERROR_CONNECTION_REFUSED
      case -4:   // HTTPC_ERROR_NOT_CONNECTED
      case -5:   // HTTPC_ERROR_CONNECTION_LOST
        out.error_code = "TCP_CONNECT_FAIL";
        break;
      default:
        out.error_code = raw_status == 0 ? "HTTP_TIMEOUT" : "TCP_CONNECT_FAIL";
        break;
    }
    out.retryable = true;
    return out;
  }

  // Real HTTP status, not 2xx.
  if (raw_status == 429) {
    out.error_code = "API_RATE_LIMITED";
    out.retryable = true;
    if (retry_after_header_s >= 0) out.retry_after_s = static_cast<uint32_t>(retry_after_header_s);
    return out;
  }
  if (raw_status >= 500 && raw_status < 600) {
    out.error_code = "HTTP_5XX";
    out.retryable = true;
    return out;
  }
  if (raw_status >= 400 && raw_status < 500) {
    // §21: 400 (and other 4xx) -> no blind retry. 401/403 are also 4xx here
    // -- Phase 1 doesn't yet have a real auth handshake to react to them
    // differently; that's a later integration point, not invented here.
    out.error_code = "HTTP_4XX";
    out.retryable = false;
    return out;
  }

  // Anything else (1xx/3xx or out-of-range) is unexpected from this
  // protocol -- not a case worth guessing about.
  out.error_code = "PROTOCOL_INVALID_RESPONSE";
  out.retryable = false;
  return out;
}

uint32_t compute_backoff_ms(int attempt, uint32_t base_ms, uint32_t max_ms,
                            uint32_t jitter_pct_0_100, uint32_t jitter_pct_max) {
  if (attempt < 1) attempt = 1;

  // Exponential: base * 2^(attempt-1), capped before jitter is applied.
  uint64_t exp_ms = base_ms;
  for (int i = 1; i < attempt && exp_ms < max_ms; ++i) {
    exp_ms *= 2;
  }
  if (exp_ms > max_ms) exp_ms = max_ms;

  uint32_t jitter_pct = jitter_pct_0_100 % (jitter_pct_max + 1);  // clamp into [0, jitter_pct_max]
  uint64_t jitter_ms = (exp_ms * jitter_pct) / 100;

  uint64_t total = exp_ms + jitter_ms;
  if (total > max_ms + (max_ms * jitter_pct_max) / 100) {
    total = max_ms + (max_ms * jitter_pct_max) / 100;  // never exceed max_ms by more than max jitter
  }
  return static_cast<uint32_t>(total);
}

RetryAfterResult parse_retry_after(const std::string& header_value) {
  RetryAfterResult result;
  if (header_value.empty()) return result;

  // Numeric-seconds form only: every character must be a digit.
  for (char c : header_value) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return result;  // HTTP-date form, not supported
  }

  long value = std::strtol(header_value.c_str(), nullptr, 10);
  if (value < 0) return result;

  result.present = true;
  result.seconds = static_cast<uint32_t>(value);
  return result;
}

}  // namespace kiosk::protocol
