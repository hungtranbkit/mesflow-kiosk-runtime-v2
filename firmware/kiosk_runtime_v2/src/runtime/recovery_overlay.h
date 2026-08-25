// Plain header, trivial free functions (matches recovery_supervisor.h's
// static-state pattern) -- deliberately NOT a class passed by reference
// between KioskRuntime and WifiRecoveryController, since neither currently
// depends on the other and this is the one small, orthogonal thing they
// need to agree on.
#pragma once

namespace kiosk::runtime {

// True while a modal recovery overlay (the '*'-hold recovery menu, or the
// Wi-Fi setup portal) owns the screen and digit-key input. KioskRuntime
// must NOT treat a digit key as business input while this is true (§5 of
// the 2026-08-25 anti-stuck follow-up: "must never silently mutate server
// business state" -- a '1'..'5' press meant for the recovery menu must
// never fall through to e.g. QUANTITY_INPUT's local digit buffer).
//
// Set by WifiRecoveryController (the only thing that owns either overlay);
// read by KioskRuntime.
void set_recovery_overlay_active(bool active);
bool recovery_overlay_active();

}  // namespace kiosk::runtime
