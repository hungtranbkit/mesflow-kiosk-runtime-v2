#pragma once

#include <Arduino.h>

namespace kiosk::runtime {

// §5 / §59: what must be visible at boot, gathered once and kept around for
// the renderer's diagnostics screen and for structured log output.
struct BootDiagnostics {
  String chip_model;
  int chip_cores = 0;
  uint32_t flash_mb = 0;
  uint32_t psram_total_bytes = 0;
  uint32_t psram_free_bytes = 0;
  uint32_t free_heap_bytes = 0;
  uint32_t largest_free_block_bytes = 0;
  String reset_reason;
  String fw_version;
  String build_id;
  String boot_id;
};

BootDiagnostics collect_boot_diagnostics(const String& boot_id);

// Re-samples the live/mutable fields (heap, PSRAM, largest block) without
// re-deriving the boot-time-only fields (chip model, reset reason, etc).
// Used by the heartbeat-equivalent periodic print in Phase 0.
void refresh_memory_fields(BootDiagnostics& diagnostics);

// True if free PSRAM is below RUNTIME_TARGET_PSRAM_FREE_PCT_MIN of total.
// A soft signal for logs only in Phase 0 — nothing acts on it yet.
bool is_psram_headroom_low(const BootDiagnostics& diagnostics);

}  // namespace kiosk::runtime
