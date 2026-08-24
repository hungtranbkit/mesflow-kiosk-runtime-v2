#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "../runtime/event_bus.h"

namespace kiosk::hardware {

// PCF8574T keypad over I2C. Board wiring (confirmed by reading, not
// copying, mesflow/esp-kiosk's keypad code — see docs/HARDWARE.md): this is
// NOT a diode matrix scanned row-by-row in the usual sense. Each of the 12
// keys, when pressed, directly shorts together two of the 8 PCF8574 I/O
// lines. To detect a press, the driver must actively drive one line low at
// a time (the other 7 held high via the PCF8574's quasi-bidirectional
// output) and see which other line reads back low — that pair of pin
// numbers identifies the key. A passive single-byte read (Phase 0's first
// implementation) never detects any press at all, because nothing is ever
// driven low without this active scan. This is a real correctness fix, not
// a style change.
//
// Which physical (pinA,pinB) pair corresponds to which labeled key varies
// per unit (7 wires can land on P0..P7 in any order), so a one-time
// calibration is required — same reason legacy has a calibration wizard.
// v2 reimplements this concept clean-room; the electrical scan algorithm
// below is dictated by the board's wiring, not lifted from legacy source.
class KeypadPcf8574 {
 public:
  explicit KeypadPcf8574(kiosk::runtime::EventBus& bus) : bus_(bus) {}

  static constexpr uint8_t kKeyCount = 12;
  static constexpr char kKeyLayout[kKeyCount + 1] = "123456789*0#";

  // Finds the PCF8574 on the shared I2C bus (see hardware_pins.h) and loads
  // any saved calibration from NVS. Returns false (DEGRADED, not fatal) if
  // no PCF8574 responds — keypad absence must not block boot.
  bool init();

  // Call every loop() iteration. Publishes KEY_DOWN/KEY_UP LocalEvents once
  // calibrated; publishes KEYPAD_RAW (uncalibrated pair) otherwise, so the
  // runtime/log can at least see raw activity and prompt for calibration.
  void poll();

  bool found() const { return found_; }
  bool calibrated() const { return calibrated_; }

  // Runs the guided calibration flow: for each of the 12 keys in
  // kKeyLayout, waits for a single stable key press+release and records
  // its electrical pair, then validates the result forms a real 4-row x
  // 3-column matrix (same sanity check legacy uses — a real property of a
  // correctly wired 4x3 matrix, not an arbitrary choice) before saving to
  // NVS. Blocking by design (like legacy): production input must not mix
  // with calibration data. `on_prompt` is called once per key so the
  // caller can render "press key X (n/12)".
  bool run_calibration(void (*on_prompt)(char key, uint8_t index, uint8_t total));

 private:
  kiosk::runtime::EventBus& bus_;
  bool found_ = false;
  uint8_t address_ = 0;
  bool calibrated_ = false;
  uint8_t pairs_[kKeyCount];  // pairs_[i] = encoded (pinA<<4)|pinB for kKeyLayout[i]

  int candidate_pair_ = -1;
  unsigned long candidate_since_ms_ = 0;
  int stable_emitted_pair_ = -1;  // -1 = nothing currently held down
  unsigned long last_poll_ms_ = 0;

  bool i2c_write_read(uint8_t drive_value, uint8_t& sensed_value);
  void release_all();
  // Returns encoded pair (pinA<<4)|pinB for exactly one shorted pair, -1 for
  // none pressed, -2 for more than one pair shorted (ambiguous/multi-key),
  // -3 for an I2C communication error.
  int scan_pair();
  char pair_to_key(int pair) const;
  bool validate_matrix_shape() const;
  bool load_calibration();
  bool save_calibration();
};

}  // namespace kiosk::hardware
