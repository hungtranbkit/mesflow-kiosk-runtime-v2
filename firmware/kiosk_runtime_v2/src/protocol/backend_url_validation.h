// Plain C++, no Arduino.h — host-testable (see test/host/). §41/§42.
#pragma once

#include <string>

namespace kiosk::protocol {

enum class BackendUrlValidation {
  OK,
  EMPTY,         // CONFIG_BACKEND_NOT_SET
  TOO_LONG,      // CONFIG_BACKEND_INVALID
  BAD_SCHEME,    // CONFIG_BACKEND_INVALID (scheme not allowed in this profile, or unknown)
  MISSING_HOST,  // CONFIG_BACKEND_INVALID
};

// allow_http: DEV profile allows http:// for bring-up against a local mock;
// PROD requires https:// only (BACKEND_URL_ALLOW_HTTP in runtime_config.h).
BackendUrlValidation validate_backend_url(const std::string& url, bool allow_http, int max_length);

const char* backend_url_validation_to_string(BackendUrlValidation v);

}  // namespace kiosk::protocol
