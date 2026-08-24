# Wi-Fi Recovery — local, backend-independent by design

**Status: implemented in Phase 0, verified on real hardware except live
keypad calibration (needs a human pressing keys — see "What's verified"
below).**

## Why this exists and why it can't depend on the backend

The entire point of this feature is recovering a device that can't reach
the backend (wrong Wi-Fi password, moved to a new network, wrong AP). If it
depended on the backend to work, it couldn't do its job. It is one of the
**ESP32 built-in** emergency features in `docs/ARCHITECTURE.md`'s boundary
— a backend-controlled UI bundle can never override or disable it.

## Trigger: hold `*` for 10 seconds

```text
normal kiosk -> hold '*' continuously for 10s -> local confirmation -> WIFI_SETUP
```

Not "press `*` several times" — a real held-duration timer
(`WifiRecoveryController`), because a few real presses false-triggering a
network reset would be worse than requiring genuine intent. Releasing
before 10s cancels cleanly.

Progressive feedback while held (`Renderer::draw_wifi_hold_progress`):
nothing before 3s, then "Tiếp tục giữ * để cài Wi-Fi", then a per-second
countdown from 7s. At 10s the portal starts and takes over the screen.

## What legacy does (read-only reference — `mesflow/esp-kiosk`, not copied)

Read before designing this, per the task's own instruction:

- Same trigger concept: hold `*` for `WIFI_SETUP_HOLD_MS` (10000ms), timer
  cancelled on release, gated on `keypadMappingReady` (calibration must
  exist first — see below).
- `WiFi.softAP("MESFlow-Setup-<chip-suffix>", "mesflow123")` +
  `DNSServer` wildcard capture (`captiveDns.start(53, "*", apIP)`) +
  `WebServer` on port 80 serving a phone-friendly setup page, plus the
  standard captive-portal detection redirects (`/generate_204`,
  `/hotspot-detect.html`, `/connecttest.txt`, catch-all).
- Legacy saves the submitted SSID/password to NVS and calls `ESP.restart()`
  **immediately** — no verification the new network actually works before
  committing.

## What v2 does differently (deliberate improvements, not accidental drift)

1. **Test before commit** (§24 of the task spec). `WifiSetupPortal` never
   writes to `ConfigStore` on submit — it calls `WiFi.begin()` with the
   candidate credentials while keeping the AP up (`WIFI_AP_STA`), waits up
   to `WIFI_RECOVERY_TEST_TIMEOUT_MS` (15s) for `WL_CONNECTED`, and only
   then persists + reboots. A wrong password never costs the
   already-working network — the old credentials in NVS are untouched
   until a real connection is proven.
2. **Per-device AP password**, not one fixed string shared across the whole
   fleet. `WifiSetupPortal::start()` derives it from `device_id`
   (`"mesflow-" + last 6 hex chars`, padded to meet WPA2's 8-char minimum).
   A single leaked/observed password shouldn't open every kiosk's recovery
   AP.
3. **Portal timeout** (§23): `WIFI_RECOVERY_PORTAL_TIMEOUT_MS` (10 minutes)
   with no HTTP request tears the AP down and returns to normal station
   mode automatically — recovery mode cannot stay open forever by accident.
4. **Explicit Cancel**, not a second keypad combo (§27's simpler option):
   the portal page has a Cancel button (`POST /cancel`) that tears down
   immediately.
5. Config save uses plain form-urlencoded POST + a hand-built JSON array
   for the scan-results list (`kiosk::protocol::json_escape` reused), not
   ArduinoJson — consistent with keeping `src/protocol/` dependency-light
   and host-testable.

## Prerequisite: keypad must be calibrated first

The `*` key can only be recognized once the keypad's electrical wiring has
been calibrated (see `docs/HARDWARE.md`'s "Keypad electrical scheme").
**This is a real bootstrapping gap, inherited from the hardware itself, not
introduced by v2**: legacy has the identical prerequisite
(`requestRuntimeKeypadCalibration` refuses unless `keypadAvailable`, and the
`*`-hold handler is gated on `keypadMappingReady`). A factory-fresh,
never-calibrated unit cannot use the physical recovery trigger until
someone runs calibration once (`keypad-calibrate` over serial in Phase 0;
a real factory-provisioning step in Phase 1/5).

## State machine (`WifiRecoveryController` + `WifiSetupPortal`)

```text
NORMAL
  --(KEY_DOWN '*')--> HOLD_COUNTDOWN
HOLD_COUNTDOWN
  --(KEY_UP '*' before 10s)--> NORMAL
  --(held 10s)--> AP_ACTIVE (portal starts)
AP_ACTIVE
  --(operator submits network)--> TESTING
  --(Cancel button / 10min timeout)--> NORMAL
TESTING
  --(WL_CONNECTED)--> commit credentials, ESP.restart()
  --(15s no connection)--> FAILED
FAILED
  --(operator retries)--> TESTING
  --(Cancel button / 10min timeout)--> NORMAL
```

`WifiRecoveryController` owns the hold-timer and renders the countdown;
`WifiSetupPortal` owns the AP/DNS/web server lifecycle and the test/commit/
rollback logic. They communicate only through `LocalEventKind::
WIFI_RECOVERY_STATE` on the shared event bus — `WifiRecoveryController`
never reaches into the portal's internals, and vice versa.

## Diagnostics shown during recovery (no secrets)

The portal/countdown screens show AP SSID, target 192.168.4.1, and (on
failure) a plain-language error — never the Wi-Fi password, and nothing is
ever logged with a password value (`structured_log` calls in
`wifi_setup_portal.cpp` only ever pass the SSID, never `pending_password_`).
A fuller local diagnostics screen (current SSID/IP/RSSI/backend-reachable/
last error, per §28 of the task spec) is not yet built — Phase 0 only
implements the recovery flow itself.

## What's verified vs. what still needs a human

Verified on real hardware this session:

- Electrical key-pair scan runs without I2C errors (`scan_pair()`
  correctly returns "no key pressed" continuously with nothing touching
  the keypad — confirms the active-scan mechanism works, not just that it
  compiles).
- Uncalibrated state is correctly detected and logged
  (`HW_KEYPAD_UNCALIBRATED`) rather than silently pretending a key was
  read.
- `wifi:<ssid>,<password>` serial provisioning still works end-to-end
  (unrelated to this feature, re-verified after the rebuild).

**Not verified this session** (needs a person physically pressing the 12
keys in sequence, which an automated tool-call session can't do on its
own): a live `keypad-calibrate` run, and therefore the full `*`-hold ->
countdown -> AP portal -> test -> commit flow end to end. The code path is
implemented and builds/flashes cleanly; ask to run it live (over
`scripts/monitor.sh`) whenever there's a person available at the device.

## Local Recovery Menu, emergency screen set beyond Wi-Fi (§30, §19)

Not implemented in Phase 0. `docs/ARCHITECTURE.md`'s emergency screen list
(`SAFE_MODE`, `NO_NETWORK`, `DEVICE_RECOVERY`, `FATAL_ERROR`,
`OTA_RECOVERY`) is reserved conceptually but only `BOOT` and the Wi-Fi
recovery screens actually exist as code today — the others depend on
functionality (safe-mode boot-loop detection, OTA A/B) that isn't built
yet (Phase 3/6). Do not assume a numbered "SYSTEM SETUP" menu exists.
