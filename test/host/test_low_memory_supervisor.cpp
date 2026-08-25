// Host test: plain C++, no Arduino -- pure classification logic only (the
// actual heap_caps_get_*(MALLOC_CAP_INTERNAL) reads are ESP-IDF-only and
// live in kiosk_runtime_v2.ino, not tested here).
#include <cstdio>
#include <string>

#include "../../firmware/kiosk_runtime_v2/src/health/low_memory_supervisor.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

using kiosk::health::classify_low_memory;
using kiosk::health::kLowMemoryCriticalLargestBlockBytes;
using kiosk::health::kLowMemoryWarningLargestBlockBytes;
using kiosk::health::LowMemoryLevel;
using kiosk::health::low_memory_level_to_string;

int main() {
  std::printf("test_low_memory_supervisor\n");

  check(classify_low_memory(1000000) == LowMemoryLevel::NORMAL, "plenty of internal SRAM -> NORMAL");
  check(classify_low_memory(kLowMemoryWarningLargestBlockBytes + 1) == LowMemoryLevel::NORMAL,
        "just above the WARNING threshold -> still NORMAL");
  check(classify_low_memory(kLowMemoryWarningLargestBlockBytes) == LowMemoryLevel::WARNING,
        "exactly at the WARNING threshold -> WARNING");
  check(classify_low_memory(kLowMemoryCriticalLargestBlockBytes + 1) == LowMemoryLevel::WARNING,
        "just above the CRITICAL threshold -> still WARNING");
  check(classify_low_memory(kLowMemoryCriticalLargestBlockBytes) == LowMemoryLevel::CRITICAL,
        "exactly at the CRITICAL threshold -> CRITICAL");
  check(classify_low_memory(0) == LowMemoryLevel::CRITICAL, "zero free -> CRITICAL, not a crash/UB");

  check(std::string(low_memory_level_to_string(LowMemoryLevel::NORMAL)) == "NORMAL", "NORMAL stringifies");
  check(std::string(low_memory_level_to_string(LowMemoryLevel::WARNING)) == "WARNING", "WARNING stringifies");
  check(std::string(low_memory_level_to_string(LowMemoryLevel::CRITICAL)) == "CRITICAL", "CRITICAL stringifies");

  std::printf("\n%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
