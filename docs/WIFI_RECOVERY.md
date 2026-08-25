# Wi-Fi Recovery — local, backend-independent by design

**Status: implemented in Phase 0, verified on real hardware except live
keypad calibration (needs a human pressing keys — see "What's verified"
below). Updated 2026-08-24: the setup AP is now OPEN (no password) — see
"Open AP rework" below for the full before/after and the security
reasoning.**

## Open AP rework (2026-08-24)

The setup AP used to require its own per-device WPA2 password
(`"mesflow-" + last 6 hex chars`). That made the very first step of fixing
a broken Wi-Fi connection depend on already knowing a secret — real friction
for no real benefit, since the AP is only up for a short, narrow window to
begin with (see "Not permanent" below). It is now **open, no password**:

- **SSID**: `MesflowKiosk-XXXX` (`src/protocol/ap_ssid.h`, pure/host-tested
  in `test/host/test_ap_ssid.cpp`) — `XXXX` is the last 4 alphanumeric
  characters of `device_id` (or `hardware_id` if unprovisioned), uppercased.
  Stable across reboots, deterministic, no punctuation (device_id values
  like `KIOSK-LASER-01` are dash-heavy — those are stripped first so the
  suffix reads as a clean code, not `...-R-01`).
- **Password**: none. `WiFi.softAP(ap_ssid_.c_str())` with no passphrase
  argument. `ap_password()` still exists as an accessor (kept rather than
  removed, so any caller that asks gets an honest `""` instead of a
  compile error) but is always empty now.
- **Not permanent** (§3 of the task): the AP only runs when (a) there are no
  usable stored Wi-Fi credentials (new — see below), (b) the operator holds
  `*` for 10s, or (c) an explicit recovery mode is entered. Normal operating
  state is AP OFF. (a) is a genuinely new trigger: previously a
  fresh/never-provisioned board just sat `DISCONNECTED` forever with no
  automatic way back into setup (only the physical `*` hold, itself gated on
  keypad calibration having already run once). `kiosk_runtime_v2.ino`'s
  `setup()` now calls `g_wifi_portal.start("no_credentials")` right after
  `WifiManager::begin()` if `config_.wifi_ssid()` is empty.
- **Auto-timeout**: `WIFI_RECOVERY_PORTAL_TIMEOUT_MS` bumped from 10 to 12
  minutes — still squarely inside "approximately 10-15 minutes", with a
  little more real-world margin for an operator fumbling with their phone's
  Wi-Fi settings UI.
- **No internet bridge**: nothing in this codebase configures IP
  forwarding/NAT between the AP and STA interfaces (`grep -r
  "ip_napt\|NAPT\|forward"` under `src/` returns nothing) — `WiFi.softAP()`
  never bridges AP clients to whatever the STA side can reach. A client
  joined to `MesflowKiosk-XXXX` can only reach the setup portal (port 80)
  and, while paused (see next point), nothing else.
- **Debug HTTP API paused for the duration**: the DEV-only debug API
  (`DebugServer`, port 8081 — full device-state read AND business-event
  injection, unauthenticated by design) used to deliberately stay up through
  recovery, back when the recovery AP still required its own password. An
  *open* AP changes that calculus completely: anyone in radio range could
  otherwise join with no credentials at all and immediately reach it. It is
  now closed (`DebugServer::set_paused(true)`, socket actually released via
  `web_.close()`, not just skipped) for as long as the setup AP is up, and
  reopened when it goes back to `INACTIVE`. The **serial** debug fallback
  (`debug-device-state`/`debug-input`/... over USB, what
  `tools/kiosk_test_runner.py` actually uses) is untouched — it never went
  through `web_` in the first place.
- **Scan-vs-AP stability fix (v0.5.1)**: reproduced live (laptop joined the
  open AP, `nmcli` scan found it at 100% signal, then failed to associate
  moments later, repeatedly) that `handle_scan()`'s old
  `WiFi.scanNetworks(false, true)` -- synchronous, 300ms/channel default --
  makes the ESP32's single 2.4GHz radio leave the AP's channel for up to
  ~300ms per channel across up to 13 channels (~4s total), during which the
  AP genuinely cannot send beacons; an already-joined client can read that
  gap as the AP disappearing. This is a real single-radio hardware limit,
  not fixable outright, but shrunk substantially: 120ms/channel (more than
  halves the total window) plus an async scan polled via `scanComplete()`
  (instead of the library's own fully-blocking wait) so
  `dns_.processNextRequest()` keeps running during the scan rather than the
  whole portal freezing on top of the radio-level gap. A negative
  `scanComplete()` result (`WIFI_SCAN_FAILED`/timeout) is now logged
  (`NET_WIFI_SCAN_FAILED`) rather than silently treated the same as "found
  zero networks" -- the previous code's loop bound made those
  indistinguishable, which is part of why this was hard to diagnose. The
  setup page also now shows a plain-language note that scanning can cause a
  brief disconnect and to rejoin and retry if it happens -- honest about the
  remaining limitation rather than claiming it's fully eliminated.
- **Captive portal, broadened**: added `/gen_204` (older Android),
  `/library/test/success.html` (older iOS), and `/ncsi.txt` (legacy Windows
  NCSI) alongside the existing `/generate_204`, `/hotspot-detect.html`,
  `/connecttest.txt` — more phones should pop the setup page automatically
  on join rather than needing the operator to know to open
  `192.168.4.1` themselves.
- **On-screen text**: the portal-active screen now reads "WIFI SETUP /
  Ket noi: MesflowKiosk-XXXX / Khong mat khau / Mo: 192.168.4.1" — explicit
  "no password" line so an operator isn't left hunting for one that doesn't
  exist. Normal-operation screens are untouched (still the 4-character SSID
  prefix near the Wi-Fi icon from the earlier session's work — this rework
  does not restore "WiFi: OK" or otherwise touch that).

### Security note (§12 of the task) — why an open AP is an accepted trade-off, not an oversight

An open Wi-Fi AP provides **no cryptographic confidentiality or
authentication** — anyone within radio range can join it, and (while
joined) see any unencrypted traffic on that AP's own segment. This is
intentional here, not a claim that the AP is secure, because:

1. **Temporary.** It is only up for a short, bounded window (12-minute idle
   timeout, or immediate teardown on explicit Cancel / successful commit) —
   never a permanent access point.
2. **Local proximity required.** Wi-Fi range only, not routable from the
   internet — an attacker must already be physically near the device.
3. **Scoped functionality.** While open, it exposes only the Wi-Fi setup
   portal itself (SSID scan + candidate-network test/commit) — the
   unauthenticated debug HTTP API is explicitly paused for the AP's entire
   lifetime (see above), and nothing else is bound to it. No business-event
   APIs, no OTA controls, no stored business data.
4. **Shuts down automatically.** Idle timeout, explicit Cancel, or a
   successful commit (which reboots into the new STA network) all tear the
   AP down — it cannot be left exposed indefinitely by accident.

If a real deployment's threat model requires stronger protection during
this window (e.g. a kiosk in a genuinely public space rather than a
supervised factory floor), the right fix is a *separate*, explicitly-scoped
hardening pass — not silently re-adding a password back into this flow,
which would just reintroduce the original friction this rework removes.

## Why this exists and why it can't depend on the backend

The entire point of this feature is recovering a device that can't reach
the backend (wrong Wi-Fi password, moved to a new network, wrong AP). If it
depended on the backend to work, it couldn't do its job. It is one of the
**ESP32 built-in** emergency features in `docs/ARCHITECTURE.md`'s boundary
— a backend-controlled UI bundle can never override or disable it.

## Triggers (three, as of the 2026-08-24 open-AP rework)

```text
normal kiosk -> hold '*' continuously for 10s -> local confirmation -> WIFI_SETUP
boot with no usable stored Wi-Fi credentials -> WIFI_SETUP automatically
explicit local recovery mode (same code path, different `reason` string)
```

The first is the original, most common path. The second is new: a
fresh/never-provisioned board (or one whose credentials were cleared) used
to just sit `DISCONNECTED` forever with no automatic way back into setup;
`kiosk_runtime_v2.ino`'s `setup()` now starts the portal itself
(`g_wifi_portal.start("no_credentials")`) when `config_.wifi_ssid()` is
empty. All three converge on the exact same `WifiSetupPortal` state machine
below — there is no separate "no-credentials" code path to keep in sync.

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
2. **Per-device AP SSID** (originally also a per-device AP *password* —
   removed in the 2026-08-24 open-AP rework, see that section above for why;
   the SSID-per-device part remains). `WifiSetupPortal::compute_ap_credentials()`
   derives `MesflowKiosk-XXXX` from `device_id` (`src/protocol/ap_ssid.h`) so
   an operator can tell kiosks apart in a Wi-Fi picker when more than one is
   in setup mode nearby.
3. **Portal timeout** (§23): `WIFI_RECOVERY_PORTAL_TIMEOUT_MS` (12 minutes as
   of the open-AP rework, previously 10) with no HTTP request tears the AP
   down and returns to normal station mode automatically — recovery mode
   cannot stay open forever by accident.
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
