// Plain C++, no Arduino.h — host-testable (see test/host/).
//
// Central per-business-state inactivity timeout table (ESP kiosk UX-
// hardening pass, 2026-08-26, §13 "Global UI Timeout Policy"). Before this,
// timeouts were scattered/absent: kiosk_runtime.cpp only had two unrelated
// millis()-based timers (the 20s error-view auto-dismiss and the 800ms
// send-retry delay) -- WAIT_OPERATION/QUANTITY_INPUT had NO inactivity
// timer at all and could sit forever if an operator walked away mid-scan.
// This module is the single source of truth for "how long can this state
// sit idle before something happens" -- kiosk_runtime.cpp owns deciding
// WHAT happens (see its own comment on why QUANTITY_INPUT's timeout does
// NOT blindly send CANCEL_REQUESTED the way WAIT_OPERATION's does: a real,
// already-open work_session must never be silently abandoned just because
// the backend correctly refuses CANCEL_REQUESTED whenever
// work_session_id is set -- app/mesflow/web/kiosk_v2.py's own
// CANCEL_NOT_SUPPORTED refusal is the authority on this, not a guess made
// here).
#pragma once

#include <cstdint>

#include "../protocol/state_projection.h"

namespace kiosk::runtime {

// 0 = no timeout (never fires) -- e.g. WAIT_EMPLOYEE, which has nothing to
// time out FROM (there's no scan/employee context to abandon).
uint32_t ui_timeout_ms_for_state(kiosk::protocol::BusinessState state);

// Pure check: true once `elapsed_ms` (the caller's own `millis() -
// entered_at_ms`, computed once by the caller so this function never touches
// millis()/wall-clock itself and stays fully host-testable) has reached or
// passed the timeout for `state`. Always false when
// ui_timeout_ms_for_state(state) == 0.
bool ui_state_should_timeout(kiosk::protocol::BusinessState state, uint32_t elapsed_ms);

}  // namespace kiosk::runtime
