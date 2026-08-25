#include "scanner_gm65.h"

#include "../config/runtime_config.h"
#include "../health/structured_log.h"

namespace kiosk::hardware {

bool ScannerGm65::init() {
  pinMode(PIN_SCANNER_RX, INPUT_PULLUP);
  serial_.begin(SCANNER_BAUD, SERIAL_8N1, PIN_SCANNER_RX, PIN_SCANNER_TX);
  buffer_.reserve(kMaxLineLength);
  return true;  // UART begin() has no failure signal to check on this core
}

void ScannerGm65::reinit() {
  buffer_ = "";  // drop whatever partial line was mid-flight -- it's stale after a UART re-attach
  serial_.end();
  init();
  kiosk::health::log_structured("INFO", "HW_SCANNER_REINIT", "scanner_gm65",
                                "manual re-init (DEV serial command)");
}

void ScannerGm65::poll() {
  unsigned long now = millis();

  // Line timeout: if bytes stopped arriving mid-line, flush what we have
  // rather than let a stalled scanner wedge the buffer forever.
  if (buffer_.length() > 0 && (now - last_byte_ms_) > kLineTimeoutMs) {
    emit_if_new(buffer_);
    buffer_ = "";
  }

  while (serial_.available() > 0) {
    char c = static_cast<char>(serial_.read());
    last_byte_ms_ = now;

    if (c == '\r' || c == '\n') {
      if (buffer_.length() > 0) {
        emit_if_new(buffer_);
        buffer_ = "";
      }
      continue;
    }

    if (buffer_.length() >= kMaxLineLength) {
      // Max length exceeded: treat as a framing problem, drop the partial
      // line rather than silently truncate-and-emit as if it were valid.
      buffer_ = "";
      continue;
    }

    buffer_ += c;
  }
}

void ScannerGm65::emit_if_new(const String& line) {
  unsigned long now = millis();

  // Physical duplicate-scan suppression: the same code re-read within the
  // debounce window from a jittery scan is suppressed here. This is NOT
  // business-level dedupe (that's event_id/backend, docs/PROTOCOL.md) — a
  // second genuinely separate scan of the same code after the window
  // elapses is passed through.
  if (line == last_emitted_line_ &&
      (now - last_emitted_ms_) < SCANNER_DUPLICATE_SUPPRESS_MS) {
    return;
  }

  last_emitted_line_ = line;
  last_emitted_ms_ = now;

  kiosk::runtime::LocalEvent event;
  event.kind = kiosk::runtime::LocalEventKind::SCAN;
  event.text = line;
  event.timestamp_ms = now;
  bus_.publish(event);
}

}  // namespace kiosk::hardware
