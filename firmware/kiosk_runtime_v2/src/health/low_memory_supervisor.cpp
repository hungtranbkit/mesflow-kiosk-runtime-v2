#include "low_memory_supervisor.h"

namespace kiosk::health {

LowMemoryLevel classify_low_memory(uint32_t largest_internal_free_block_bytes) {
  if (largest_internal_free_block_bytes <= kLowMemoryCriticalLargestBlockBytes) return LowMemoryLevel::CRITICAL;
  if (largest_internal_free_block_bytes <= kLowMemoryWarningLargestBlockBytes) return LowMemoryLevel::WARNING;
  return LowMemoryLevel::NORMAL;
}

const char* low_memory_level_to_string(LowMemoryLevel level) {
  switch (level) {
    case LowMemoryLevel::NORMAL: return "NORMAL";
    case LowMemoryLevel::WARNING: return "WARNING";
    case LowMemoryLevel::CRITICAL: return "CRITICAL";
  }
  return "UNKNOWN";
}

}  // namespace kiosk::health
