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
//
// A SECOND real bug this fixes (found live, 2026-08-25 connectivity
// investigation): `events_url.lastIndexOf('/')` alone treats the "//" in
// "http://" itself as a path separator when `events_url` is a bare
// "http://host" with NO real path component (a malformed config value --
// the contract requires the full ".../api/kiosk/v2/events" URL, not just
// the host) -- e.g. `derive_sibling_endpoint("http://dev.mesflow.net",
// "bootstrap")` used to silently return the nonsense URL
// "http://bootstrap" instead of failing. Every caller already handles an
// empty return correctly (bootstrap_client.cpp logs a clear
// "could not derive .../bootstrap endpoint" error; heartbeat_client.cpp's
// `http.begin("")` returns false and is skipped; kiosk_runtime.cpp/
// ui_sync_controller.cpp both check `.length()==0`) -- so the real fix is
// just detecting this case and returning "" instead of a plausible-looking
// wrong URL, so a misconfigured endpoint fails loudly and diagnosably
// instead of producing a generic, hours-to-diagnose "no response from
// backend" that looks exactly like a real network fault.
inline String derive_sibling_endpoint(const String& events_url, const char* name) {
  int scheme_end = events_url.indexOf("://");
  // Index of the LAST byte of "scheme://" (the 2nd '/') -- a real path
  // segment's own '/' must come strictly after this, or there's no path
  // at all beyond "scheme://host" for this function to split on.
  int min_slash = scheme_end < 0 ? -1 : scheme_end + 2;
  int last_slash = events_url.lastIndexOf('/');
  if (last_slash < 0 || last_slash <= min_slash) return "";
  return events_url.substring(0, last_slash + 1) + name;
}

}  // namespace kiosk::network
