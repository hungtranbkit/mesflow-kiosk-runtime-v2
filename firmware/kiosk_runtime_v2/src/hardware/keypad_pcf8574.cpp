#include "keypad_pcf8574.h"

#include <Preferences.h>

#include <cstring>

#include "../config/hardware_pins.h"
#include "../health/structured_log.h"

namespace kiosk::hardware {

namespace {
constexpr unsigned long kPollIntervalMs = 12;    // matches legacy's observed scan cadence
constexpr unsigned long kStableMs = 40;          // debounce before treating a pair as "held"
constexpr unsigned long kReleaseStableMs = 100;   // debounce before treating "no pair" as released
const char* kNvsNamespace = "kiosk_keypad";
constexpr uint32_t kMappingMagic = 0x4B505031;  // "KPP1"
}  // namespace

bool KeypadPcf8574::i2c_write_read(uint8_t drive_value, uint8_t& sensed_value) {
  Wire.beginTransmission(address_);
  Wire.write(drive_value);
  if (Wire.endTransmission() != 0) return false;
  delayMicroseconds(250);  // let the PCF8574's open-drain outputs settle
  if (Wire.requestFrom(static_cast<int>(address_), 1) != 1 || !Wire.available()) {
    return false;
  }
  sensed_value = Wire.read();
  return true;
}

void KeypadPcf8574::release_all() {
  uint8_t ignored = 0xFF;
  i2c_write_read(0xFF, ignored);
}

int KeypadPcf8574::scan_pair() {
  int found_pair = -1;
  uint8_t pair_count = 0;
  for (uint8_t first = 0; first < 8; ++first) {
    const uint8_t drive = static_cast<uint8_t>(0xFFu & ~(1u << first));
    uint8_t sample = 0xFF;
    if (!i2c_write_read(drive, sample)) {
      release_all();
      return -3;
    }
    for (uint8_t second = first + 1; second < 8; ++second) {
      if ((sample & (1u << second)) == 0) {
        found_pair = (first << 4) | second;
        ++pair_count;
      }
    }
  }
  release_all();
  if (pair_count == 0) return -1;
  if (pair_count == 1) return found_pair;
  return -2;  // more than one line shorted: multi-key or wiring issue
}

char KeypadPcf8574::pair_to_key(int pair) const {
  if (pair < 0) return '\0';
  for (uint8_t i = 0; i < kKeyCount; ++i) {
    if (pairs_[i] == static_cast<uint8_t>(pair)) return kKeyLayout[i];
  }
  return '\0';
}

bool KeypadPcf8574::validate_matrix_shape() const {
  // A correctly wired 4-row x 3-column direct-connect matrix has exactly 4
  // pins that each appear in 3 key-pairs (rows) and 3 pins that each appear
  // in 4 key-pairs (columns), with 1 pin unused -- this is a structural
  // property of the physical wiring, so a calibration whose pairs don't
  // form this shape means the operator likely pressed keys out of order or
  // there's a wiring fault, not a valid mapping.
  uint8_t degree[8] = {0};
  for (uint8_t i = 0; i < kKeyCount; ++i) {
    const uint8_t a = pairs_[i] >> 4;
    const uint8_t b = pairs_[i] & 0x0F;
    if (a >= 8 || b >= 8 || a == b) return false;
    for (uint8_t j = 0; j < i; ++j) {
      if (pairs_[j] == pairs_[i]) return false;  // two keys can't share a pair
    }
    ++degree[a];
    ++degree[b];
  }
  uint8_t rows = 0, columns = 0, unused = 0;
  for (uint8_t pin = 0; pin < 8; ++pin) {
    if (degree[pin] == 3) ++rows;
    else if (degree[pin] == 4) ++columns;
    else if (degree[pin] == 0) ++unused;
    else return false;
  }
  return rows == 4 && columns == 3 && unused == 1;
}

bool KeypadPcf8574::load_calibration() {
  Preferences prefs;
  prefs.begin(kNvsNamespace, true);
  bool present = prefs.getUInt("magic", 0) == kMappingMagic &&
                 prefs.getBytesLength("pairs") == sizeof(pairs_);
  if (present) prefs.getBytes("pairs", pairs_, sizeof(pairs_));
  prefs.end();
  calibrated_ = present && validate_matrix_shape();
  return calibrated_;
}

bool KeypadPcf8574::save_calibration() {
  if (!validate_matrix_shape()) return false;
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) return false;
  size_t pair_bytes = prefs.putBytes("pairs", pairs_, sizeof(pairs_));
  size_t magic_bytes = prefs.putUInt("magic", kMappingMagic);
  prefs.end();
  calibrated_ = pair_bytes == sizeof(pairs_) && magic_bytes == sizeof(uint32_t);
  return calibrated_;
}

bool KeypadPcf8574::init() {
  Wire.begin(PIN_KEYPAD_SDA, PIN_KEYPAD_SCL, 100000);

  for (uint8_t addr = KEYPAD_I2C_ADDR_MIN; addr <= KEYPAD_I2C_ADDR_MAX; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      address_ = addr;
      found_ = true;
      break;
    }
  }

  if (!found_) {
    kiosk::health::log_structured("WARN", "HW_KEYPAD_NOT_FOUND", "keypad_pcf8574",
                                   "no PCF8574 responded in 0x20..0x27");
    return false;
  }

  release_all();
  load_calibration();
  kiosk::health::log_structured(calibrated_ ? "INFO" : "WARN",
                                 calibrated_ ? "HW_KEYPAD_CALIBRATED" : "HW_KEYPAD_UNCALIBRATED",
                                 "keypad_pcf8574",
                                 calibrated_ ? "loaded saved key mapping from NVS"
                                             : "no valid calibration in NVS; keys will not resolve "
                                               "until calibrated (see run_calibration / serial "
                                               "command 'keypad-calibrate')");
  return true;
}

void KeypadPcf8574::poll() {
  if (!found_) return;

  unsigned long now = millis();
  if (now - last_poll_ms_ < kPollIntervalMs) return;
  last_poll_ms_ = now;

  int pair = scan_pair();
  if (pair == -3) {
    kiosk::health::log_structured("WARN", "HW_KEYPAD_I2C_ERROR", "keypad_pcf8574",
                                   "I2C communication error during scan");
    return;
  }

  if (pair != candidate_pair_) {
    candidate_pair_ = pair;
    candidate_since_ms_ = now;
    return;
  }

  unsigned long debounce = (pair == -1) ? kReleaseStableMs : kStableMs;
  if (now - candidate_since_ms_ < debounce) return;

  if (!calibrated_) {
    // Uncalibrated: surface raw activity for diagnostics/calibration UX,
    // but do not claim a resolved key identity we don't actually have.
    if (pair >= 0 && pair != stable_emitted_pair_) {
      kiosk::runtime::LocalEvent event;
      event.kind = kiosk::runtime::LocalEventKind::KEYPAD_RAW;
      event.raw_byte = static_cast<uint8_t>(pair);
      event.timestamp_ms = now;
      bus_.publish(event);
    }
    stable_emitted_pair_ = pair;
    return;
  }

  // Calibrated: emit resolved KEY_DOWN/KEY_UP transitions.
  if (pair >= 0 && stable_emitted_pair_ != pair) {
    char key = pair_to_key(pair);
    if (key != '\0') {
      stable_emitted_pair_ = pair;
      kiosk::runtime::LocalEvent event;
      event.kind = kiosk::runtime::LocalEventKind::KEY_DOWN;
      event.key = key;
      event.timestamp_ms = now;
      bus_.publish(event);
    }
  } else if (pair == -1 && stable_emitted_pair_ != -1) {
    char key = pair_to_key(stable_emitted_pair_);
    stable_emitted_pair_ = -1;
    if (key != '\0') {
      kiosk::runtime::LocalEvent event;
      event.kind = kiosk::runtime::LocalEventKind::KEY_UP;
      event.key = key;
      event.timestamp_ms = now;
      bus_.publish(event);
    }
  }
}

bool KeypadPcf8574::run_calibration(void (*on_prompt)(char key, uint8_t index, uint8_t total)) {
  if (!found_) return false;

  kiosk::health::log_structured("INFO", "HW_KEYPAD_CAL_START", "keypad_pcf8574",
                                 "starting guided calibration; press and release each key shown");

  uint8_t new_pairs[kKeyCount];
  memset(new_pairs, 0xFF, sizeof(new_pairs));

  // Wait for release before starting so a key held from before calibration
  // doesn't get mistaken for the first prompt.
  while (scan_pair() != -1) delay(10);

  for (uint8_t index = 0; index < kKeyCount; ++index) {
    if (on_prompt) on_prompt(kKeyLayout[index], index, kKeyCount);

    // Wait for a stable single-pair press.
    int candidate = -1;
    unsigned long stable_since = 0;
    for (;;) {
      int pair = scan_pair();
      if (pair >= 0) {
        if (pair != candidate) {
          candidate = pair;
          stable_since = millis();
        } else if (millis() - stable_since >= 60) {
          break;
        }
      } else {
        candidate = -1;
      }
      delay(10);
    }

    bool duplicate = false;
    for (uint8_t prev = 0; prev < index; ++prev) {
      if (new_pairs[prev] == static_cast<uint8_t>(candidate)) duplicate = true;
    }
    if (duplicate) {
      kiosk::health::log_structured("WARN", "HW_KEYPAD_CAL_DUPLICATE", "keypad_pcf8574",
                                     "same electrical pair seen twice; press the correct key");
      while (scan_pair() != -1) delay(10);
      --index;  // retry this slot
      continue;
    }

    new_pairs[index] = static_cast<uint8_t>(candidate);
    while (scan_pair() != -1) delay(10);  // wait for release before the next key
  }

  memcpy(pairs_, new_pairs, sizeof(pairs_));
  if (!validate_matrix_shape()) {
    kiosk::health::log_structured("ERROR", "HW_KEYPAD_CAL_BAD_SHAPE", "keypad_pcf8574",
                                   "calibration result is not a valid 4x3 matrix; not saved");
    return false;
  }
  if (!save_calibration()) {
    kiosk::health::log_structured("ERROR", "STORAGE_ERR_KEYPAD_CAL_SAVE", "keypad_pcf8574",
                                   "failed to persist calibration to NVS");
    return false;
  }

  kiosk::health::log_structured("INFO", "HW_KEYPAD_CAL_DONE", "keypad_pcf8574",
                                 "calibration saved");
  return true;
}

}  // namespace kiosk::hardware
