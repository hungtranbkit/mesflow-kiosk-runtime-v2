// Host test: plain C++, no Arduino. ESP kiosk UX-hardening pass, §13.
#include <cstdio>

#include "../../firmware/kiosk_runtime_v2/src/runtime/ui_timeout_policy.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

int main() {
  using kiosk::protocol::BusinessState;
  using namespace kiosk::runtime;

  std::printf("test_ui_timeout_policy\n");

  // --- No-timeout states: 0 regardless of elapsed time ---
  check(ui_timeout_ms_for_state(BusinessState::WAIT_EMPLOYEE) == 0,
        "WAIT_EMPLOYEE has no timeout (nothing to abandon)");
  check(ui_timeout_ms_for_state(BusinessState::SESSION_ACTIVE) == 0,
        "SESSION_ACTIVE has no timeout");
  check(ui_timeout_ms_for_state(BusinessState::DEVICE_DISABLED) == 0,
        "DEVICE_DISABLED has no timeout (server-declared, device has no authority to time out of it)");
  check(ui_timeout_ms_for_state(BusinessState::MAINTENANCE) == 0, "MAINTENANCE has no timeout");
  check(ui_timeout_ms_for_state(BusinessState::UNSUPPORTED) == 0, "UNSUPPORTED has no timeout");
  check(!ui_state_should_timeout(BusinessState::WAIT_EMPLOYEE, 999999999u),
        "WAIT_EMPLOYEE never times out, even after a huge elapsed value");

  // --- WAIT_OPERATION: 20s ---
  check(ui_timeout_ms_for_state(BusinessState::WAIT_OPERATION) == 20000,
        "WAIT_OPERATION timeout is 20000ms");
  check(!ui_state_should_timeout(BusinessState::WAIT_OPERATION, 19999),
        "WAIT_OPERATION: 19999ms elapsed -> not yet timed out");
  check(ui_state_should_timeout(BusinessState::WAIT_OPERATION, 20000),
        "WAIT_OPERATION: exactly 20000ms elapsed -> timed out");
  check(ui_state_should_timeout(BusinessState::WAIT_OPERATION, 25000),
        "WAIT_OPERATION: 25000ms elapsed -> timed out");
  check(!ui_state_should_timeout(BusinessState::WAIT_OPERATION, 0),
        "WAIT_OPERATION: 0ms elapsed -> not timed out");

  // --- QUANTITY_INPUT: 60s ---
  check(ui_timeout_ms_for_state(BusinessState::QUANTITY_INPUT) == 60000,
        "QUANTITY_INPUT timeout is 60000ms");
  check(!ui_state_should_timeout(BusinessState::QUANTITY_INPUT, 59999),
        "QUANTITY_INPUT: 59999ms elapsed -> not yet timed out");
  check(ui_state_should_timeout(BusinessState::QUANTITY_INPUT, 60000),
        "QUANTITY_INPUT: exactly 60000ms elapsed -> timed out");

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
