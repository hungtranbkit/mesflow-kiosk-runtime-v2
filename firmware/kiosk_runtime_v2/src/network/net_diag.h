#pragma once

#include <Arduino.h>

#include "../config/runtime_config.h"

#if MESFLOW_DEBUG_API

namespace kiosk::network {

// DEV-only low-level connectivity diagnostic, added 2026-08-25 during the
// "device reaches nothing, host reaches everything" investigation. Walks
// the network stack bottom-up (WiFi association -> gateway TCP -> DNS ->
// raw TCP -> plain HTTP GET) for a fixed set of targets, one structured
// JSON block per layer, so a stuck layer is visible directly instead of
// inferred from HTTPClient's single collapsed "no response" error.
//
// Deliberately PLAIN WiFiClient only -- no HTTPClient, no
// WiFiClientSecure/TLS in this diagnostic path (this project already has
// one documented WiFiClientSecure::connected() false-negative bug on this
// ESP32 core -- see api_client.cpp's own comment -- a diagnostic tool that
// itself might hit that bug would be worse than useless here).
//
// Targets are fixed (dev.mesflow.net, prod.mesflow.net) rather than
// parameterized -- those are the two real backends this investigation
// cares about; testing an arbitrary host is still possible via the
// existing `api-endpoint:` command + `debug-device-state`'s
// bootstrap/heartbeat fields, just without this diagnostic's per-layer
// breakdown.
void run_net_diag(Stream& out, uint32_t wifi_reconnect_count);

}  // namespace kiosk::network

#endif  // MESFLOW_DEBUG_API
