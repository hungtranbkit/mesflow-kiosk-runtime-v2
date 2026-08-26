// Plain C++, no Arduino.h — host-testable (see test/host/).
//
// ESP kiosk UX-hardening pass (2026-08-26), §2/§3/§25: the device's own
// short DEV/TEST/PROD/UNKNOWN label, and the pure comparison that decides a
// server mismatch. The backend (app/mesflow/web/kiosk_v2.py) returns its
// real, unambiguous settings.server_role value ("DEV"/"PRODUCTION_TEST"/
// "PRODUCTION"/"") -- this file owns mapping THAT to the operator-facing
// short label the spec asks for, so the backend never has to know or care
// about the device's presentation choice.
#pragma once

#include <string>

namespace kiosk::protocol {

enum class Environment {
  DEV,
  TEST,
  PROD,
  UNKNOWN,
};

const char* environment_to_string(Environment e);

// Maps a raw settings.server_role value (as returned by /bootstrap's
// "server_role" field) to the short label. Case-sensitive on purpose --
// the backend's own values are a fixed, known set (never user-typed), so a
// value that doesn't match one of the three exactly is exactly the
// "I don't recognize this" case UNKNOWN exists for, not a typo to guess
// through -- same philosophy as StateProjection's own UNSUPPORTED handling
// for a business state name it doesn't recognize (§53).
Environment environment_from_server_role(const std::string& server_role);

// Maps an OPERATOR-typed expected-environment string (config_store.h's
// expected_environment(), set via the serial/portal provisioning command)
// to the same enum -- case-INSENSITIVE here, since a human is typing this
// one, and "dev"/"Dev"/"DEV" should all mean the same thing. An empty or
// unrecognized string maps to UNKNOWN, which environment_matches() below
// treats as "never configured -- never claim a match".
Environment environment_from_config_string(const std::string& configured);

// §3: the actual mismatch decision. UNKNOWN on either side NEVER counts as
// a match -- an unconfigured expected_environment or an old backend that
// doesn't send server_role yet must not silently pass as "fine", since
// that would defeat the entire safety feature (a device with a forgotten
// expected_environment config would happily talk to any server).
bool environment_matches(Environment expected, Environment actual);

}  // namespace kiosk::protocol
