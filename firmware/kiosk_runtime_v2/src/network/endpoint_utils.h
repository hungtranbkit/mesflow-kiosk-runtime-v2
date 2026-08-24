#pragma once

#include <Arduino.h>

namespace kiosk::network {

// ConfigStore only stores ONE backend URL (the /events endpoint); every
// other endpoint (bootstrap/heartbeat/state) is a sibling under the same
// base path (docs/PROTOCOL.md: ".../api/kiosk/v2/<name>"). Simple
// last-path-segment swap, not a general URL library -- the shape is fixed
// by this project's own contract.
//
// A real bug this fixes (found live, Phase 2): BootstrapClient used to POST
// directly to the stored /events URL instead of deriving /bootstrap, so
// every bootstrap request silently landed on the events handler instead --
// which correctly rejected it (no protocol_version field at the bootstrap
// body's top level), producing a misleading but plausible-looking
// "accepted:false" that had nothing to do with real bootstrap logic at all.
inline String derive_sibling_endpoint(const String& events_url, const char* name) {
  int last_slash = events_url.lastIndexOf('/');
  if (last_slash < 0) return "";
  return events_url.substring(0, last_slash + 1) + name;
}

}  // namespace kiosk::network
