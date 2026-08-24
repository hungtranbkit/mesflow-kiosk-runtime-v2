#pragma once

#include <string>

#include "../hardware/hardware_selftest.h"
#include "../hardware/keypad_pcf8574.h"
#include "../network/bootstrap_client.h"
#include "../network/time_sync.h"
#include "../security/device_identity.h"
#include "boot_diagnostics.h"
#include "kiosk_runtime.h"

namespace kiosk::runtime {

// Single source of truth for the device/protocol/time/security/memory/
// hardware/input status JSON -- used by BOTH /debug/device-state (DEV only,
// debug_server.cpp) AND the heartbeat POST body (heartbeat_client.cpp,
// always compiled, including PROD). Refreshes `diagnostics`'s live memory
// fields as a side effect (matches debug_server's prior behavior).
//
// §27/§34/§39: never includes Wi-Fi password, private keys, or certificate
// material -- only SSID/RSSI/IP and coarse security-profile flags.
std::string build_status_json(kiosk::security::DeviceIdentity& identity,
                              kiosk::network::TimeSync& time_sync, KioskRuntime& runtime,
                              kiosk::hardware::KeypadPcf8574& keypad,
                              const kiosk::hardware::SelfTestResult& selftest,
                              BootDiagnostics& diagnostics,
                              const kiosk::network::BootstrapClient& bootstrap);

}  // namespace kiosk::runtime
