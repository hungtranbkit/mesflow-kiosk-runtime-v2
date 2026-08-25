// Plain C++, no Arduino.h -- host-testable pure classification, matching
// event_journal_index.h's journal_pressure_for_usage() pattern. The actual
// heap_caps_get_*(MALLOC_CAP_INTERNAL) reads (ESP-IDF-only) are done by the
// caller (kiosk_runtime_v2.ino) and passed in here.
#pragma once

#include <cstdint>

namespace kiosk::health {

enum class LowMemoryLevel { NORMAL, WARNING, CRITICAL };

// Conservative thresholds against INTERNAL SRAM's largest contiguous free
// block -- deliberately NOT "free heap" blended with PSRAM, which looked
// deceptively abundant (~8MB) during the earlier fragmentation investigation
// while the real constraint lived entirely in internal SRAM (see
// memory_diag.h's own comment for that history). xTaskCreate() needs one
// contiguous block big enough for a task's whole stack (currently 6144
// bytes -- see api_client.cpp/state_client.cpp's measured-safe shrink) plus
// its TCB overhead; WARNING sits at roughly 3x that and CRITICAL at roughly
// 1.5x, so WARNING gives real lead time and CRITICAL still has *some*
// margin left before an actual xTaskCreate() failure like the one this
// self-recovery task exists to stop from being a dead end.
constexpr uint32_t kLowMemoryWarningLargestBlockBytes = 18000;
constexpr uint32_t kLowMemoryCriticalLargestBlockBytes = 9000;

LowMemoryLevel classify_low_memory(uint32_t largest_internal_free_block_bytes);
const char* low_memory_level_to_string(LowMemoryLevel level);

}  // namespace kiosk::health
