#pragma once

#include <Arduino.h>

#include "../config/runtime_config.h"  // defines MESFLOW_DEBUG_API

// Internal-SRAM fragmentation root-cause investigation (2026-08-24,
// LOCAL_TEST only). DEV-only (gated by MESFLOW_DEBUG_API, matching the
// rest of this project's debug-instrumentation convention) -- this is
// diagnostic tooling, not a production feature, and should compile out of
// PROD entirely like every other DEV-only surface.
//
// Why INTERNAL specifically, not just "free heap": esp_get_free_heap_size()
// (what device-state's "heap_free" already reports) blends PSRAM into the
// total, which looked deceptively huge (~8MB) while the real constraint --
// a contiguous block big enough for mbedTLS's TLS buffers -- lives entirely
// in internal SRAM. A live "SSL - Memory allocation failed" was traced to
// internal_free=52964 bytes but internal_largest_block=31732 bytes, just
// under what a TLS handshake needs contiguously, even though total (blended)
// free heap looked abundant.
#if MESFLOW_DEBUG_API

namespace kiosk::health {

struct MemorySnapshot {
  uint32_t internal_free = 0;
  uint32_t internal_largest = 0;
  uint32_t internal_min_ever = 0;  // heap_caps_get_minimum_free_size -- lowest internal-free point ever seen
  uint32_t all8bit_free = 0;       // MALLOC_CAP_8BIT: internal + PSRAM, byte-addressable
  uint32_t psram_free = 0;
  uint32_t psram_largest = 0;
  uint32_t total_free = 0;      // MALLOC_CAP_DEFAULT
  uint32_t total_largest = 0;
};

MemorySnapshot capture_memory_snapshot();

// Logs one MEMORY_SNAPSHOT structured-log line tagged with `stage` (one of
// the named lifecycle points in the investigation task, e.g. "BOOT_EARLY",
// "AFTER_WIFI_INIT") plus the calling task's name and current uptime, so a
// serial capture across a full boot produces a stage-by-stage table without
// needing separate bookkeeping to correlate lines back to lifecycle points.
void log_memory_snapshot(const char* stage);

}  // namespace kiosk::health

#endif  // MESFLOW_DEBUG_API
