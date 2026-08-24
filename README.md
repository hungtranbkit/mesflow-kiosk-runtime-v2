# MESFlow Kiosk Runtime v2

> **DO NOT DEPLOY TO PRODUCTION.** This is Phase 1 of a from-scratch
> rebuild. No mTLS/secure boot/flash encryption, no offline durability, no
> backend-controlled UI exist yet. See "Known limitations" below before
> considering it for anything beyond bring-up on a development board.

## Purpose

Rebuild the ESP32 kiosk as a thin hardware runtime — renderer, local cache,
offline journal, secure transport — with the MESFlow backend owning
business state, workflow, policy, and UI. See `docs/ARCHITECTURE.md` for the
full rationale and the invariants this project must never violate.

## Why this project is separate from the legacy kiosk

`mesflow/esp-kiosk` (currently v5.5.7) has business/workflow logic baked
into a single ~6300-line firmware file, making every policy change a
build+flash and making the device — not the backend — the source of truth.
This project is a clean-room rebuild: the legacy firmware was only ever
*read* for hardware facts (pinout, UART settings — see `docs/HARDWARE.md`),
never copied, refactored, or modified. **The legacy firmware keeps running
unmodified**; nothing here touches it. Migration between them is staged and
deliberate — see `docs/DEVICE_LIFECYCLE.md`.

## Supported hardware

ESP32-S3, 16MB flash, 8MB PSRAM (confirmed on real hardware), ILI9341
240x320 SPI display, GM65 UART barcode scanner, PCF8574T I2C keypad
(electrical pair-scan + one-time calibration, see `docs/HARDWARE.md`).
Touch (FT6336G) is present on the reference board but not driven yet. No
speaker/buzzer exists on the reference board.

## Framework: Arduino (not ESP-IDF) — why

ESP-IDF is not installed in this environment. `arduino-cli` with the
`esp32:esp32` core (3.3.11) has a proven build for this exact chip, with
every library needed already present. Business/protocol logic
(`src/protocol/`) is kept framework-agnostic (plain C++, no `Arduino.h`)
specifically so a later ESP-IDF port only has to redo the hardware/network
adapter layer. Reconsider this choice before Phase 6 (OTA A/B, secure boot).

## Build — DEV vs PRODUCTION profile

```bash
scripts/build-dev.sh    # -> firmware/kiosk_runtime_v2/build-dev/
scripts/build-prod.sh   # -> firmware/kiosk_runtime_v2/build-prod/
scripts/build.sh        # alias for build-dev.sh
```

The profile is baked in at compile time (`MESFLOW_PROFILE_DEV`/
`MESFLOW_PROFILE_PROD`), not a runtime flag. PRODUCTION compiles the entire
Remote Visual Debug module out — verified via `strings` on the compiled
`.elf`: 4 occurrences of `/debug/*` route strings in DEV, **0** in PROD.
See `docs/SECURITY.md`. PROD has not been flashed to any board — build +
this static check is the evidence bar for it right now.

## Flash

```bash
scripts/flash.sh [/dev/ttyACM0]
```

Always flashes the DEV profile (builds it first). Requires typing `YES` to
confirm — this overwrites whatever is currently on the target device. **Do
not run this against a board that runs firmware you depend on.**

## Monitor

```bash
scripts/monitor.sh [/dev/ttyACM0]
```

Serial log lines are structured JSON (`{"level":...,"code":...}`, one
`Serial.print()` call per line, mutex-guarded — see "Known gaps fixed this
phase" below) — see `docs/PROTOCOL.md` / `src/health/structured_log.*`.

## Test

Host-side (no ESP toolchain, runs anywhere):

```bash
scripts/run_host_tests.sh
```

Covers the protocol codec's Envelope v1 wire format, `device_seq`
reservation-block persistence, retry classification/backoff/jitter, and
backend URL validation — see `docs/TEST_PLAN.md` for full scenario
coverage/status (KIOSK-001 through KIOSK-086).

## Mock backend

```bash
python3 tools/mock_backend/mock_backend.py --port 8799
```

Implements `/bootstrap`, `/events`, `/heartbeat`, plus a test-only
`/_test/drop-next` chaos control for simulating a lost ACK. Point the
device at it over serial: `api-endpoint:http://<host-ip>:8799/api/kiosk/v2/events`
(there is **no default backend URL baked in** — §3: a placeholder-that-
looks-valid was Phase 0's own mistake; `ConfigStore::api_endpoint()` now
returns `""`, reported as `CONFIG_BACKEND_NOT_SET`, until explicitly
configured).

**Environment note**: in this dev sandbox, the kiosk's Wi-Fi segment could
not route back to this host's own addresses (likely an isolated guest/IoT
VLAN policy) even though the device could reach the public internet fine.
Live protocol verification this session used `httpbin.org`'s public echo/
status endpoints instead — see `docs/PROTOCOL.md`'s note on this. Point the
device at your own reachable mock backend if your network doesn't have
this restriction.

## Remote Visual Debug

```bash
scripts/capture-screen.sh <device-ip>
```

Grabs a real screenshot (decoded to PNG) + UI state + device state
(now including `protocol`/`time`/`security` blocks, §28/§39) from a running
kiosk over HTTP (port 8081, DEV builds only) into
`artifacts/debug/<timestamp>/`. See `docs/VISUAL_DEBUG.md` — already caught
and fixed a real text-overflow rendering bug this way. `POST /debug/input`
injects `SCAN`/`KEY_DOWN`/`KEY_UP` through the real EventBus.

## Security profile (current)

DEV/PRODUCTION build-profile split implemented. mTLS, certificate storage,
secure boot, and flash encryption are **designed, not implemented** — see
`docs/SECURITY.md` for exactly what exists vs. what's still a document.
Plain HTTP transport, plaintext Wi-Fi credentials in NVS.

## Device identity & provisioning

```
provision:<device_id>   assign device_id, move to ACTIVE, reboot
suspend / revoke        DEV-only: force those provisioning states
```

`hardware_id` (from the chip's eFuse MAC) and `device_id` (business
identity, empty until provisioned) are two different things — invariant 12,
no shared fleet default. A factory-fresh unit shows an identity screen
("THIET BI CHUA CAU HINH") instead of the normal waiting screen and refuses
to send business events. See `docs/PROVISIONING.md`.

## Wi-Fi recovery: hold `*` for 10 seconds

Works in **every** provisioning state, including UNPROVISIONED (§40) — see
`docs/WIFI_RECOVERY.md`. Requires the keypad to be calibrated first:

```
keypad-calibrate    guided 12-key calibration (press each key shown on-screen)
wifi:<ssid>,<password>    DEV-only stopgap, saves + reboots
```

## Current phase

**Phase 1 — Protocol + Security + Device Identity.** Implemented and
verified live on real hardware this phase:

- **Device identity**: `hardware_id`/`device_id`/`provisioning_state`
  split (`security/device_identity.*`), gating scan handling and rendering
  a dedicated identity screen when not ACTIVE
- **Persistent, wear-aware `device_seq`**: reservation-block technique
  (`protocol/sequence_reservation.*`, portable + host-tested), confirmed
  monotonic across 5+ real reboots (`0 → 1 → 1000 → 2000 → 3001 → 4001`)
- **Protocol Envelope v1**: nested `device`/`event`/`time`/`context`/
  `payload`, `timestamp_device` is JSON `null` (never fabricated) when
  unsynced
- **NTP time sync** (`network/time_sync.*`): non-blocking, `UNSYNCED → SYNCING
  → SYNCED/STALE/FAILED`, verified live
- **Real retry policy**: exponential backoff + jitter, HTTP-status
  classification, 429/Retry-After — verified live (same `event_id`/
  `device_seq` across 5 retries against a real 503 endpoint)
- **Non-blocking event sends** (§23): `AsyncEventSender` runs retries on
  its own FreeRTOS task; the main loop/UI/heartbeat kept running normally
  throughout a live ~29s multi-retry sequence
- **Bootstrap + heartbeat clients** (`network/bootstrap_client.*`,
  `heartbeat_client.*`), heartbeat sharing a single status-JSON builder
  with `/debug/device-state` (`runtime/status_snapshot.*`)
- **Backend URL validation** (`protocol/backend_url_validation.*`, host-
  tested): no more placeholder-that-looks-valid default; explicit
  `CONFIG_BACKEND_NOT_SET`/`CONFIG_BACKEND_INVALID`
- **DEV/PRODUCTION build profiles**: PRODUCTION compiles the debug module
  out entirely (build-artifact-verified, not just runtime-gated)
- Everything from Phase 0 (Remote Visual Debug, Wi-Fi recovery, keypad
  matrix decode, scanner, event bus) — regression-tested and still passing

## Known gaps fixed this phase (found via real hardware, not just review)

- `device_seq` used to reset to 1 every boot: `DeviceSequence`'s NVS access
  ran inside a globally-constructed object's C++ constructor, before
  Arduino's `nvs_flash_init()`. Fixed by moving it to an explicit `init()`
  called from `setup()`.
- `structured_log`'s multi-part `Serial.print()` calls could interleave
  when called from two different FreeRTOS tasks (the background send task
  and the main loop) at once, corrupting a log line. Fixed: one
  mutex-guarded `Serial.print()` per line.

## Known limitations (do not assume otherwise)

- **No durable offline journal.** An event is genuinely lost if it can't
  be sent (no queue, no retry beyond the current attempt's max) — the
  runtime says so on-screen. Phase 3.
- **mTLS/secure boot/flash encryption**: designed only, zero code. Plain
  HTTP. See `docs/SECURITY.md`.
- **No authoritative backend state.** Placeholder `READY_LOCAL` runtime
  state. Phase 2.
- **No backend-controlled UI.** Screens are hardcoded. Phase 4.
- **No Vietnamese glyph support at all** — stock ASCII-only font.
- **Touch is not driven.**
- **Keypad must be calibrated before any key resolves**, including the
  `*`-hold Wi-Fi recovery trigger — inherent to the hardware wiring.
- **No PIN/auth on DEV serial provisioning commands.**
- **No live-tested unsupported-protocol-version / real-bootstrap-accepted
  round trip** this session — `mock_backend.py` (which returns real
  `accepted:true`) wasn't reachable from the device on this network; see
  the mock-backend section above.
- **No local recovery menu beyond Wi-Fi**: `SAFE_MODE`/`NO_NETWORK`/
  `DEVICE_RECOVERY`/`FATAL_ERROR`/`OTA_RECOVERY` are reserved names, not
  working screens.

## Next phase

Phase 2 — Server Authoritative State + Workflow Contract: real
stable/transient state machine driven by backend responses, `state_version`
tracking and RESYNC, replacing the `READY_LOCAL` placeholder.
