#pragma once

#include <Arduino.h>
#include <functional>
#include <vector>

namespace kiosk::runtime {

// Local event bus (§28, §27): drivers publish; the runtime and UI subscribe.
// No driver calls the network or storage layer directly — everything goes
// through here first. Phase 0 keeps this deliberately simple: synchronous,
// single-core dispatch from the main loop() — there is no cross-task queue
// yet. If a future phase needs drivers running on a separate FreeRTOS task,
// this is the seam where a real queue would replace the direct callback
// list, without touching publishers or subscribers.

enum class LocalEventKind {
  SCAN,                // scanner_gm65: a line was read
  KEYPAD_RAW,          // keypad_pcf8574: uncalibrated pair activity (diagnostics only)
  KEY_DOWN,            // keypad_pcf8574: a calibrated key was pressed
  KEY_UP,              // keypad_pcf8574: a calibrated key was released
  WIFI_STATE,          // wifi_manager: connection state changed
  WIFI_RECOVERY_STATE, // wifi_setup_portal: recovery-portal state changed
};

struct LocalEvent {
  LocalEventKind kind;
  String text;          // SCAN: raw scanner line. WIFI_STATE/WIFI_RECOVERY_STATE: human state name.
  uint8_t raw_byte = 0;  // KEYPAD_RAW: encoded electrical pair (0xFF = none)
  char key = '\0';       // KEY_DOWN/KEY_UP: resolved key character
  uint32_t timestamp_ms = 0;
};

class EventBus {
 public:
  using Subscriber = std::function<void(const LocalEvent&)>;

  void subscribe(Subscriber subscriber);
  void publish(const LocalEvent& event);

 private:
  std::vector<Subscriber> subscribers_;
};

}  // namespace kiosk::runtime
