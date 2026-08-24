#include "ids.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>

#include "../config/runtime_config.h"
#include "../health/structured_log.h"

namespace kiosk::protocol {

namespace {
const char* kNvsNamespace = "kiosk_seq";
const char* kNvsKey = "hwm";
}  // namespace

std::string generate_random_hex_id(size_t byte_length) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(byte_length * 2);
  for (size_t i = 0; i < byte_length; ++i) {
    uint8_t b = static_cast<uint8_t>(esp_random() & 0xFF);
    out += hex[(b >> 4) & 0x0F];
    out += hex[b & 0x0F];
  }
  return out;
}

void DeviceSequence::init() {
  if (reservation_) return;  // already initialized, don't reserve a second block
  reservation_ = std::make_unique<SequenceReservation>(
      []() -> uint64_t {
        Preferences prefs;
        prefs.begin(kNvsNamespace, true);
        uint64_t v = prefs.getULong64(kNvsKey, 0);
        prefs.end();
        return v;
      },
      [](uint64_t value) {
        Preferences prefs;
        prefs.begin(kNvsNamespace, false);
        prefs.putULong64(kNvsKey, value);
        prefs.end();
      },
      DEVICE_SEQ_RESERVE_BLOCK);
}

uint64_t DeviceSequence::next() {
  if (!reservation_) {
    kiosk::health::log_structured("ERROR", "SEQ_PERSIST_FAIL", "device_sequence",
                                   "next() called before init() -- returning 0, NOT a real sequence value");
    return 0;
  }
  return reservation_->next();
}

uint64_t DeviceSequence::current() const {
  if (!reservation_) return 0;
  return reservation_->current();
}

}  // namespace kiosk::protocol
