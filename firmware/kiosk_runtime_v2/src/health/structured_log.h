#pragma once

#include <Arduino.h>

namespace kiosk::health {

// §38: structured logging over free-text where practical. Phase 0 keeps this
// to a single line of hand-built JSON on Serial — no log buffering/shipping
// yet (that's part of telemetry/heartbeat, Phase 1+).
//
// `code` should be one of the documented error families (NET_/AUTH_/API_/
// STATE_/UI_/STORAGE_/SCANNER_/INPUT_/OTA_/CONFIG_/SYNC_/HW_/SEC_) followed
// by a specific suffix, e.g. "HW_KEYPAD_NOT_FOUND".
void log_structured(const char* level, const char* code, const char* module,
                     const char* message);

}  // namespace kiosk::health
