// Host test: plain C++, no Arduino. ESP kiosk UX-hardening pass, §2/§3.
#include <cstdio>

#include "../../firmware/kiosk_runtime_v2/src/protocol/environment_label.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

int main() {
  using namespace kiosk::protocol;

  std::printf("test_environment_label\n");

  // --- server_role mapping (exact, case-sensitive -- the backend's own
  // fixed value set, never user-typed) ---
  check(environment_from_server_role("DEV") == Environment::DEV, "server_role DEV -> Environment::DEV");
  check(environment_from_server_role("PRODUCTION_TEST") == Environment::TEST,
        "server_role PRODUCTION_TEST -> Environment::TEST");
  check(environment_from_server_role("PRODUCTION") == Environment::PROD,
        "server_role PRODUCTION -> Environment::PROD");
  check(environment_from_server_role("") == Environment::UNKNOWN, "server_role '' -> Environment::UNKNOWN");
  check(environment_from_server_role("dev") == Environment::UNKNOWN,
        "server_role lowercase 'dev' -> UNKNOWN (case-sensitive: backend's own values are fixed, not a typo to guess)");
  check(environment_from_server_role("SOMETHING_NEW") == Environment::UNKNOWN,
        "an unrecognized server_role (future backend value) -> UNKNOWN, not guessed");

  // --- operator-typed expected-environment mapping (case-insensitive) ---
  check(environment_from_config_string("DEV") == Environment::DEV, "config 'DEV' -> Environment::DEV");
  check(environment_from_config_string("dev") == Environment::DEV, "config 'dev' -> Environment::DEV (case-insensitive)");
  check(environment_from_config_string("Test") == Environment::TEST, "config 'Test' -> Environment::TEST");
  check(environment_from_config_string("prod") == Environment::PROD, "config 'prod' -> Environment::PROD");
  check(environment_from_config_string("") == Environment::UNKNOWN, "config '' (never configured) -> UNKNOWN");
  check(environment_from_config_string("staging") == Environment::UNKNOWN,
        "config 'staging' (not one of the 3 known values) -> UNKNOWN");

  // --- stringify ---
  check(std::string(environment_to_string(Environment::DEV)) == "DEV", "DEV stringifies");
  check(std::string(environment_to_string(Environment::TEST)) == "TEST", "TEST stringifies");
  check(std::string(environment_to_string(Environment::PROD)) == "PROD", "PROD stringifies");
  check(std::string(environment_to_string(Environment::UNKNOWN)) == "UNKNOWN", "UNKNOWN stringifies");

  // --- matches: the actual safety decision ---
  check(environment_matches(Environment::TEST, Environment::TEST), "TEST expected, TEST actual -> match");
  check(!environment_matches(Environment::TEST, Environment::PROD),
        "TEST expected, PROD actual -> MISMATCH (the real §3 safety case)");
  check(!environment_matches(Environment::DEV, Environment::TEST), "DEV expected, TEST actual -> mismatch");
  check(!environment_matches(Environment::UNKNOWN, Environment::TEST),
        "expected UNKNOWN (never configured) -> never a match, even against a real TEST server");
  check(!environment_matches(Environment::TEST, Environment::UNKNOWN),
        "actual UNKNOWN (old backend with no server_role field yet) -> never a match, even if expected is configured");
  check(!environment_matches(Environment::UNKNOWN, Environment::UNKNOWN),
        "both UNKNOWN -> still never a match (fail closed, not fail open)");

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
