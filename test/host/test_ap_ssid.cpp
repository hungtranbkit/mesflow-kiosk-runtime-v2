// Host test: plain C++, no Arduino. Covers the pure SSID-format computation
// for the Wi-Fi setup/recovery AP (2026-08-24 open-AP rework).
#include <cstdio>

#include "../../firmware/kiosk_runtime_v2/src/protocol/ap_ssid.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

int main() {
  using namespace kiosk::protocol;

  std::printf("test_ap_ssid\n");

  check(compute_setup_ap_ssid("KIOSK-LASER-01") == "MesflowKiosk-ER01",
        "device_id 'KIOSK-LASER-01' -> punctuation stripped first, then last 4 alnum chars, uppercased ('ER01')");

  check(compute_setup_ap_ssid("esp32s3-4c64cef61b44") == "MesflowKiosk-1B44",
        "hardware_id fallback -> last 4 chars uppercased ('1b44' -> '1B44')");

  check(compute_setup_ap_ssid("abcd") == "MesflowKiosk-ABCD",
        "exactly-4-char identity -> whole string used, uppercased");

  check(compute_setup_ap_ssid("ab") == "MesflowKiosk-00AB",
        "shorter-than-4 identity -> left-padded with '0' (never seen in practice, but must not crash "
        "or produce a variable-width SSID)");

  check(compute_setup_ap_ssid("") == "MesflowKiosk-0000",
        "empty identity (defensive case) -> all-zero suffix, not a crash or empty SSID");

  // Determinism: same input always produces the same SSID (a QA/regression
  // test needs to know the AP name in advance, and an operator relies on it
  // being stable across reboots -- see docs/WIFI_RECOVERY.md).
  check(compute_setup_ap_ssid("KIOSK-LASER-01") == compute_setup_ap_ssid("KIOSK-LASER-01"),
        "deterministic: same identity -> same SSID every time");

  // Different devices must not collide on the common case of differing
  // only in their final digits.
  check(compute_setup_ap_ssid("KIOSK-LASER-01") != compute_setup_ap_ssid("KIOSK-LASER-02"),
        "different device suffixes -> different SSIDs");

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
