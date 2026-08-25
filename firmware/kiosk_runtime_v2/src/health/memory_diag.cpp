#include "memory_diag.h"

#if MESFLOW_DEBUG_API

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "structured_log.h"

namespace kiosk::health {

MemorySnapshot capture_memory_snapshot() {
  MemorySnapshot s;
  s.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  s.internal_min_ever = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  s.all8bit_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  s.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s.psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  s.total_free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  s.total_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  return s;
}

void log_memory_snapshot(const char* stage) {
  MemorySnapshot s = capture_memory_snapshot();
  char msg[192];
  snprintf(msg, sizeof(msg),
           "stage=%s task=%s uptime_ms=%lu int_free=%u int_largest=%u int_min=%u "
           "psram_free=%u psram_largest=%u total_free=%u total_largest=%u",
           stage, pcTaskGetName(nullptr), millis(), s.internal_free, s.internal_largest,
           s.internal_min_ever, s.psram_free, s.psram_largest, s.total_free, s.total_largest);
  log_structured("INFO", "MEMORY_SNAPSHOT", "memory_diag", msg);
}

}  // namespace kiosk::health

#endif  // MESFLOW_DEBUG_API
