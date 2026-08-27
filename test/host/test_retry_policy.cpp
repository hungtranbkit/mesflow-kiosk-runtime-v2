// Host test: plain C++, no Arduino. §21-§26/§51.
#include <cstdio>

#include "../../firmware/kiosk_runtime_v2/src/protocol/retry_policy.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

int main() {
  using namespace kiosk::protocol;

  std::printf("test_retry_policy\n");

  // --- Classification ---
  {
    auto ok = classify_http_result(200);
    check(ok.ok && ok.error_code.empty(), "200 -> ok, no error_code");

    auto timeout = classify_http_result(-11);
    check(!timeout.ok && timeout.error_code == "HTTP_TIMEOUT" && timeout.retryable,
          "-11 (read timeout) -> HTTP_TIMEOUT, retryable");

    auto refused = classify_http_result(-1);
    check(refused.error_code == "TCP_CONNECT_FAIL" && refused.retryable,
          "-1 (connection refused) -> TCP_CONNECT_FAIL, retryable");

    auto dns_fail = classify_http_result(kDnsFailMarker);
    check(dns_fail.error_code == "DNS_FAIL" && dns_fail.retryable,
          "kDnsFailMarker (pre-flight WiFi.hostByName() failure) -> DNS_FAIL, retryable");

    // Response-size guard (2026-08-26 memory-simplification pass): rejected
    // on Content-Length alone, before ever reading the body -- deliberately
    // NOT retryable (a pathologically large response is a server-side
    // condition a retry can't fix).
    auto too_large = classify_http_result(kResponseTooLargeMarker);
    check(!too_large.ok && too_large.error_code == "RESPONSE_TOO_LARGE" && !too_large.retryable,
          "kResponseTooLargeMarker -> RESPONSE_TOO_LARGE, NOT retryable");

    auto bad_request = classify_http_result(400);
    check(!bad_request.retryable && bad_request.error_code == "HTTP_4XX",
          "400 -> HTTP_4XX, NOT retryable (no blind retry)");

    auto unauthorized = classify_http_result(401);
    check(!unauthorized.retryable, "401 -> not retryable");

    auto forbidden = classify_http_result(403);
    check(!forbidden.retryable, "403 -> not retryable");

    // §2 of the 2026-08-27 "Final Reliability Standardization" pass: 409
    // (idempotency payload-mismatch, app/mesflow/web/kiosk_v2.py's
    // IDEMPOTENCY_KEY_REUSE_MISMATCH) must never be retried blindly --
    // confirmed here explicitly rather than relying on the generic 4xx
    // bucket test above to imply it.
    auto conflict = classify_http_result(409);
    check(!conflict.retryable && conflict.error_code == "HTTP_4XX",
          "409 -> HTTP_4XX, NOT retryable (idempotency payload mismatch must not retry forever)");

    auto server_error = classify_http_result(500);
    check(server_error.retryable && server_error.error_code == "HTTP_5XX",
          "500 -> HTTP_5XX, retryable");
    auto unavailable = classify_http_result(503);
    check(unavailable.retryable && unavailable.error_code == "HTTP_5XX", "503 -> HTTP_5XX, retryable");

    auto rate_limited = classify_http_result(429, 30);
    check(rate_limited.retryable && rate_limited.error_code == "API_RATE_LIMITED" &&
              rate_limited.retry_after_s == 30,
          "429 with Retry-After: 30 -> API_RATE_LIMITED, retryable, retry_after_s=30");

    auto rate_limited_no_header = classify_http_result(429);
    check(rate_limited_no_header.retryable && rate_limited_no_header.retry_after_s == 0,
          "429 without Retry-After -> still retryable, retry_after_s=0 (caller falls back to normal backoff)");
  }

  // --- Backoff bounds ---
  {
    uint32_t b1 = compute_backoff_ms(1, 1000, 30000, 0);
    uint32_t b2 = compute_backoff_ms(2, 1000, 30000, 0);
    uint32_t b3 = compute_backoff_ms(3, 1000, 30000, 0);
    check(b1 == 1000, "attempt 1, no jitter -> base (1000ms)");
    check(b2 == 2000, "attempt 2, no jitter -> 2x base (2000ms)");
    check(b3 == 4000, "attempt 3, no jitter -> 4x base (4000ms)");

    uint32_t b_capped = compute_backoff_ms(10, 1000, 30000, 0);
    check(b_capped == 30000, "high attempt count caps at max_ms (30000ms) with no jitter");

    uint32_t b_jitter_max = compute_backoff_ms(1, 1000, 30000, 25, 25);
    check(b_jitter_max == 1250, "attempt 1, jitter=25(of max 25) -> base + 25% = 1250ms");

    uint32_t b_capped_jitter = compute_backoff_ms(10, 1000, 30000, 25, 25);
    check(b_capped_jitter <= 30000 + 30000 / 4,
          "capped backoff + max jitter never exceeds max_ms + jitter_pct_max%%");

    bool bounds_ok = true;
    for (int attempt = 1; attempt <= 15; ++attempt) {
      for (uint32_t jitter = 0; jitter <= 100; jitter += 10) {
        uint32_t v = compute_backoff_ms(attempt, 1000, 30000, jitter, 25);
        if (v < 1000 || v > 30000 + 30000 / 4) bounds_ok = false;
      }
    }
    check(bounds_ok, "backoff stays within [base, max+25%%] across a sweep of attempts/jitter values");
  }

  // --- Retry-After parsing ---
  {
    auto r1 = parse_retry_after("30");
    check(r1.present && r1.seconds == 30, "numeric Retry-After parses correctly");

    auto r2 = parse_retry_after("");
    check(!r2.present, "empty Retry-After -> not present");

    auto r3 = parse_retry_after("Wed, 21 Oct 2026 07:28:00 GMT");
    check(!r3.present, "HTTP-date Retry-After form -> not present (honestly unsupported, not guessed)");

    auto r4 = parse_retry_after("0");
    check(r4.present && r4.seconds == 0, "Retry-After: 0 parses as present, 0 seconds");
  }

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
