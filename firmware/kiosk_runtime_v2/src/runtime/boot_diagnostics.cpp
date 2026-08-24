#include "boot_diagnostics.h"

#include <esp_heap_caps.h>
#include <esp_system.h>

#include "../config/build_info.h"
#include "../config/runtime_config.h"

namespace kiosk::runtime {

namespace {

String reset_reason_to_string(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN: return "UNKNOWN";
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "OTHER";
  }
}

}  // namespace

void refresh_memory_fields(BootDiagnostics& d) {
  d.free_heap_bytes = static_cast<uint32_t>(esp_get_free_heap_size());
  d.largest_free_block_bytes = static_cast<uint32_t>(
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  d.psram_free_bytes = static_cast<uint32_t>(
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

BootDiagnostics collect_boot_diagnostics(const String& boot_id) {
  BootDiagnostics d;
  d.chip_model = ESP.getChipModel();
  d.chip_cores = ESP.getChipCores();
  d.flash_mb = ESP.getFlashChipSize() / (1024 * 1024);
  d.psram_total_bytes = static_cast<uint32_t>(
      heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  d.reset_reason = reset_reason_to_string(esp_reset_reason());
  d.fw_version = KIOSK_RUNTIME_VERSION;
  d.build_id = KIOSK_BUILD_ID;
  d.boot_id = boot_id;
  refresh_memory_fields(d);
  return d;
}

bool is_psram_headroom_low(const BootDiagnostics& d) {
  if (d.psram_total_bytes == 0) {
    return false;  // no PSRAM detected at all is a different, louder problem
  }
  uint32_t free_pct = (d.psram_free_bytes * 100) / d.psram_total_bytes;
  return free_pct < RUNTIME_TARGET_PSRAM_FREE_PCT_MIN;
}

}  // namespace kiosk::runtime
