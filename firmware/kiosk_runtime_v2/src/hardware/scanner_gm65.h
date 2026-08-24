#pragma once

#include <Arduino.h>

#include "../config/hardware_pins.h"
#include "../runtime/event_bus.h"

namespace kiosk::hardware {

// GM65 barcode scanner over UART, one-way (RX only). §26: this layer only
// does UART framing/timeout/max-length/input sanitation and physical
// duplicate-scan suppression — it does not interpret the payload as a
// business entity. It publishes a SCAN LocalEvent with the raw line; the
// runtime decides what (if anything) to do with it.
class ScannerGm65 {
 public:
  explicit ScannerGm65(kiosk::runtime::EventBus& bus) : bus_(bus) {}

  bool init();

  // Call every loop() iteration. Non-blocking: reads whatever bytes are
  // available, accumulates a line, and publishes on line-terminator or
  // max-length.
  void poll();

 private:
  static constexpr size_t kMaxLineLength = 128;
  static constexpr unsigned long kLineTimeoutMs = 200;

  kiosk::runtime::EventBus& bus_;
  HardwareSerial serial_{1};
  String buffer_;
  unsigned long last_byte_ms_ = 0;

  String last_emitted_line_;
  unsigned long last_emitted_ms_ = 0;

  void emit_if_new(const String& line);
};

}  // namespace kiosk::hardware
