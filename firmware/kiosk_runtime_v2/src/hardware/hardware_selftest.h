#pragma once

namespace kiosk::hardware {

// Per-peripheral pass/fail used at boot (§5) and exposed to the renderer's
// diagnostics screen. "PASS" here means "init reported success", which for
// the display is weaker than a true readback verification — see Display's
// doc comment.
struct SelfTestResult {
  bool display_ok = false;
  bool scanner_ok = false;
  bool keypad_ok = false;  // false = DEGRADED (keypad not found), not fatal
};

}  // namespace kiosk::hardware
