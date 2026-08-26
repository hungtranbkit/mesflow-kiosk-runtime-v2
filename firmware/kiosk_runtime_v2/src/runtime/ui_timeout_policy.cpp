#include "ui_timeout_policy.h"

namespace kiosk::runtime {

namespace {
// §13: starting values, adjustable after real-floor UX observation (the
// spec itself says so). WAIT_EMPLOYEE/SESSION_ACTIVE/DEVICE_DISABLED/
// MAINTENANCE/UNSUPPORTED all get 0 (no timeout) -- WAIT_EMPLOYEE has
// nothing to abandon, SESSION_ACTIVE's only key action is the optional '#'
// finish shortcut (no scan-in-progress context to lose), and
// DEVICE_DISABLED/MAINTENANCE/UNSUPPORTED are server-declared states the
// device has no authority to time out of on its own.
constexpr uint32_t kWaitOperationTimeoutMs = 20000;   // §7 of the spec: 15-30s window, picked 20s
constexpr uint32_t kQuantityInputTimeoutMs = 60000;   // §13: 60s -- see kiosk_runtime.cpp for what firing this actually does
}  // namespace

uint32_t ui_timeout_ms_for_state(kiosk::protocol::BusinessState state) {
  using kiosk::protocol::BusinessState;
  switch (state) {
    case BusinessState::WAIT_OPERATION:
      return kWaitOperationTimeoutMs;
    case BusinessState::QUANTITY_INPUT:
      return kQuantityInputTimeoutMs;
    case BusinessState::WAIT_EMPLOYEE:
    case BusinessState::SESSION_ACTIVE:
    case BusinessState::DEVICE_DISABLED:
    case BusinessState::MAINTENANCE:
    case BusinessState::UNSUPPORTED:
      return 0;
  }
  return 0;
}

bool ui_state_should_timeout(kiosk::protocol::BusinessState state, uint32_t elapsed_ms) {
  uint32_t limit = ui_timeout_ms_for_state(state);
  if (limit == 0) return false;
  return elapsed_ms >= limit;
}

}  // namespace kiosk::runtime
