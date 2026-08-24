// Plain C++, no Arduino.h — must stay host-testable (see test/host/).
#pragma once

#include <string>

#include "event_types.h"

namespace kiosk::protocol {

// Encodes a KioskEvent as the JSON wire format documented in
// docs/PROTOCOL.md. Hand-rolled (no ArduinoJson dependency) specifically so
// this file compiles with plain g++ for host tests, with zero toolchain
// setup. Escaping covers the minimal set this protocol actually needs
// (quotes, backslash, control chars) — it is not a general-purpose JSON
// encoder.
std::string encode_event_json(const KioskEvent& event);

// Escapes a string for embedding inside a JSON string literal (without the
// surrounding quotes). Exposed for testing.
std::string json_escape(const std::string& input);

}  // namespace kiosk::protocol
