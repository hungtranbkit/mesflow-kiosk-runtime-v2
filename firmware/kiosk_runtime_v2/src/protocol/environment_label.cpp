#include "environment_label.h"

namespace kiosk::protocol {

const char* environment_to_string(Environment e) {
  switch (e) {
    case Environment::DEV: return "DEV";
    case Environment::TEST: return "TEST";
    case Environment::PROD: return "PROD";
    case Environment::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}

Environment environment_from_server_role(const std::string& server_role) {
  if (server_role == "DEV") return Environment::DEV;
  if (server_role == "PRODUCTION_TEST") return Environment::TEST;
  if (server_role == "PRODUCTION") return Environment::PROD;
  return Environment::UNKNOWN;
}

Environment environment_from_config_string(const std::string& configured) {
  std::string upper;
  upper.reserve(configured.size());
  for (char c : configured) {
    upper += static_cast<char>((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
  }
  if (upper == "DEV") return Environment::DEV;
  if (upper == "TEST") return Environment::TEST;
  if (upper == "PROD") return Environment::PROD;
  return Environment::UNKNOWN;
}

bool environment_matches(Environment expected, Environment actual) {
  if (expected == Environment::UNKNOWN || actual == Environment::UNKNOWN) return false;
  return expected == actual;
}

}  // namespace kiosk::protocol
