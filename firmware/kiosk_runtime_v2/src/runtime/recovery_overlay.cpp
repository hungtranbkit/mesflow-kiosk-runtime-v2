#include "recovery_overlay.h"

namespace kiosk::runtime {

namespace {
bool g_active = false;
}

void set_recovery_overlay_active(bool active) { g_active = active; }
bool recovery_overlay_active() { return g_active; }

}  // namespace kiosk::runtime
