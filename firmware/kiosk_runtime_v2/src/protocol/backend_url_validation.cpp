#include "backend_url_validation.h"

namespace kiosk::protocol {

namespace {
bool starts_with(const std::string& s, const char* prefix) {
  size_t len = 0;
  while (prefix[len] != '\0') ++len;
  return s.size() >= len && s.compare(0, len, prefix) == 0;
}
}  // namespace

BackendUrlValidation validate_backend_url(const std::string& url, bool allow_http, int max_length) {
  if (url.empty()) return BackendUrlValidation::EMPTY;
  if (static_cast<int>(url.length()) > max_length) return BackendUrlValidation::TOO_LONG;

  bool is_https = starts_with(url, "https://");
  bool is_http = starts_with(url, "http://");
  if (!is_https && !(is_http && allow_http)) {
    return BackendUrlValidation::BAD_SCHEME;
  }

  size_t scheme_len = is_https ? 8 : 7;
  size_t slash = url.find('/', scheme_len);
  size_t host_end = (slash == std::string::npos) ? url.length() : slash;
  if (host_end <= scheme_len) return BackendUrlValidation::MISSING_HOST;

  return BackendUrlValidation::OK;
}

const char* backend_url_validation_to_string(BackendUrlValidation v) {
  switch (v) {
    case BackendUrlValidation::OK: return "OK";
    case BackendUrlValidation::EMPTY: return "CONFIG_BACKEND_NOT_SET";
    case BackendUrlValidation::TOO_LONG: return "CONFIG_BACKEND_INVALID";
    case BackendUrlValidation::BAD_SCHEME: return "CONFIG_BACKEND_INVALID";
    case BackendUrlValidation::MISSING_HOST: return "CONFIG_BACKEND_INVALID";
  }
  return "UNKNOWN";
}

}  // namespace kiosk::protocol
