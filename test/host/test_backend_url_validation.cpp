// Host test: plain C++, no Arduino. §41/§51.
#include <cstdio>
#include <string>

#include "../../firmware/kiosk_runtime_v2/src/protocol/backend_url_validation.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

int main() {
  using namespace kiosk::protocol;

  std::printf("test_backend_url_validation\n");

  check(validate_backend_url("", true, 160) == BackendUrlValidation::EMPTY,
        "empty string -> EMPTY (CONFIG_BACKEND_NOT_SET)");

  check(validate_backend_url("https://mesflow.example.com/api", true, 160) == BackendUrlValidation::OK,
        "valid https URL -> OK");
  check(validate_backend_url("https://mesflow.example.com/api", false, 160) == BackendUrlValidation::OK,
        "https URL -> OK even when http is disallowed (PROD profile)");

  check(validate_backend_url("http://192.168.1.50:8799/api/kiosk/v2/events", true, 160) ==
            BackendUrlValidation::OK,
        "http URL -> OK when allow_http=true (DEV profile)");
  check(validate_backend_url("http://192.168.1.50:8799/api/kiosk/v2/events", false, 160) ==
            BackendUrlValidation::BAD_SCHEME,
        "http URL -> BAD_SCHEME when allow_http=false (PROD profile)");

  check(validate_backend_url("ftp://example.com/", true, 160) == BackendUrlValidation::BAD_SCHEME,
        "unknown scheme -> BAD_SCHEME");
  check(validate_backend_url("mesflow.example.com", true, 160) == BackendUrlValidation::BAD_SCHEME,
        "missing scheme entirely -> BAD_SCHEME");

  check(validate_backend_url("https://", true, 160) == BackendUrlValidation::MISSING_HOST,
        "scheme with no host -> MISSING_HOST");
  check(validate_backend_url("https:///path", true, 160) == BackendUrlValidation::MISSING_HOST,
        "scheme with empty host before path -> MISSING_HOST");

  {
    std::string long_url = "https://";
    for (int i = 0; i < 200; ++i) long_url += "a";
    check(validate_backend_url(long_url, true, 160) == BackendUrlValidation::TOO_LONG,
          "over max_length -> TOO_LONG");
  }

  check(backend_url_validation_to_string(BackendUrlValidation::OK) == std::string("OK"),
        "OK stringifies to \"OK\"");
  check(backend_url_validation_to_string(BackendUrlValidation::EMPTY) ==
            std::string("CONFIG_BACKEND_NOT_SET"),
        "EMPTY stringifies to CONFIG_BACKEND_NOT_SET");
  check(backend_url_validation_to_string(BackendUrlValidation::BAD_SCHEME) ==
            std::string("CONFIG_BACKEND_INVALID"),
        "BAD_SCHEME stringifies to CONFIG_BACKEND_INVALID");

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
