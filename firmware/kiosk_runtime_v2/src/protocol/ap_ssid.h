#pragma once

#include <string>

namespace kiosk::protocol {

// Pure, host-testable computation of the Wi-Fi setup/recovery AP's SSID.
// Kept out of WifiSetupPortal (Arduino/WiFi.h) so the format itself --
// "MesflowKiosk-XXXX", a short stable per-device suffix -- can be verified
// by a host unit test without any Arduino framework or hardware, matching
// this project's established pure-core/thin-Arduino-adapter split (see
// event_journal_core.h vs storage/event_journal.h).
//
// The AP itself is deliberately OPEN (no password) as of the 2026-08-24
// provisioning-AP rework -- see docs/WIFI_RECOVERY.md's security note for
// why that's an accepted, documented trade-off rather than an oversight.
// This header only computes the SSID string; it says nothing about the
// password (there isn't one).

// `identity_name` is the device_id if provisioned, else hardware_id (the
// same fallback WifiSetupPortal::compute_ap_credentials() already used --
// recovery must work even on an UNPROVISIONED unit with no device_id yet).
// Returns "MesflowKiosk-XXXX" where XXXX is the last 4 characters of
// `identity_name`, uppercased for on-screen readability. If `identity_name`
// is shorter than 4 characters (shouldn't happen in practice -- both
// device_id and hardware_id are always longer -- but never assume), the
// whole string is used and left-padded with '0' so the SSID format is
// always exactly "MesflowKiosk-" + 4 characters.
std::string compute_setup_ap_ssid(const std::string& identity_name);

}  // namespace kiosk::protocol
