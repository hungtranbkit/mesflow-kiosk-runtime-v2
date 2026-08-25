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

- ~~No durable offline journal~~ -- fixed 2026-08-24, see "Anti-Stuck
  Runtime Policy" below: a real crash-safe event journal
  (PENDING/INFLIGHT/ACKED/REJECTED, incremental compaction, verified
  against all 4 crash-mid-compaction windows on real hardware) exists now.
  This line predated that work and was never updated. Remaining real gap:
  the journal isn't cleared/isolated when `api-endpoint:` changes to a
  different backend -- old PENDING events from a prior backend get
  replayed against whatever's configured now (harmless -- they just get
  rejected -- but noisy; see the bug backlog).
- **mTLS/secure boot/flash encryption**: designed only, zero code. Plain
  HTTP. See `docs/SECURITY.md`.
- ~~No authoritative backend state~~ -- fixed. `READY_LOCAL` is still real
  but is only the transient state between boot and the first successful
  bootstrap; every business screen (`WAIT_EMPLOYEE`/`WAIT_OPERATION`/
  `SESSION_ACTIVE`/`QUANTITY_INPUT`) is server-driven now (`state.source:
  "SERVER"`), verified extensively on real hardware this session. This
  line predated that work.
- ~~No backend-controlled UI~~ -- partially fixed: the backend-managed UI
  bundle system (`kiosk_v2_ui_bundles`/`kiosk_v2_ui_desired`, download-on-
  change, A/B slot swap) is real and working (verified live: pushed a
  fixed bundle, watched the device detect the hash mismatch and re-sync).
  What's still hardcoded: the quantity-entry flow's screens
  (`draw_quantity_defect_screen()` etc. in `renderer.cpp`) -- only the
  simpler business-state screens go through the bundle system so far.
- ~~No Vietnamese glyph support at all~~ -- fixed 2026-08-25, see
  `docs/VIETNAMESE_FONT.md`: three native-size bitmap fonts (12/16/24px,
  porting v1's proven strategy), full precomposed Vietnamese coverage,
  verified on real hardware.
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

## Anti-Stuck Runtime Policy (Self-Recovery)

**Invariant (2026-08-24): NO RECOVERABLE ERROR MAY LEAVE THE KIOSK
PERMANENTLY STUCK.** Every transient/error state must have a timeout OR a
progress condition, a next state, an automatic recovery path, and a
structured error code — no dead-end state is allowed. This was written
after a REAL incident: the durable event journal's in-memory index
(`EventJournalIndex`) never shrank, grew to 211+ records / 96%+ of its
256KB capacity over a day of testing, and the resulting internal-SRAM
pressure made `xTaskCreate()` itself start failing — the device got stuck
on "Lỗi gửi sự kiện - CHƯA được lưu" with no way off the screen short of a
manual reboot.

What's implemented:

- **Journal compaction, now INCREMENTAL** (`EventJournalIndex::select_records_to_keep()` /
  `rebuild_after_compaction()`, `EventJournal::begin_compaction()`/`compact_tick()`):
  PENDING/IN_FLIGHT/CONFLICT/HUMAN_REVIEW records are always kept in full
  (never silently drop an unsynced business event); ACKED/REJECTED are
  bounded to the most-recently-created N of each (`CompactionPolicy`,
  default 20/20). The routine 30s/70%-usage auto-trigger now writes
  `kCompactionRecordsPerTick` (10) records per `loop()` iteration instead
  of the whole rewrite in one blocking call — scanner/keypad/Wi-Fi polling
  gets control back between chunks. `compaction_active`/
  `compaction_records_processed`/`compaction_records_total`/
  `compaction_last_progress_ms` are exposed in `/debug/device-state`. The
  original one-shot `compact()` is kept for the rare CRITICAL low-memory /
  task-creation-failure emergency paths, where finishing in one bounded
  call is the safer tradeoff. Crash-safe atomic swap (temp file → validate
  by re-scan → rename swap → cleanup) either way, with boot-time
  orphan-file recovery in `EventJournal::init()` for every interruption
  window — **all 4 windows verified on real hardware** via the DEV-only
  `simulate-compaction-crash:<1-4>` hook (stray incomplete tmp, valid tmp
  not yet swapped, mid-swap/.old-only, swap-done/.old-not-cleaned-up), each
  recovering its full, un-truncated record count with zero loss. Compaction
  itself was also verified live: a device at 214 records / internal-SRAM
  largest-block 3828 bytes (CRITICAL) recovered to 94 records / 26612 bytes
  on its first scheduled run, no reboot needed.
- **Task-creation-failure recovery, PROVEN via fault injection**: a
  `send()` failure (xTaskCreate failing, the actual root cause above) gets
  exactly one retry after a bounded delay (triggering an immediate
  compaction attempt first); a second consecutive failure escalates to a
  controlled reboot (`RECOVERY_TASK_CREATE_FAILED`). DEV-only
  `force-task-create-failure` (one-shot) makes this exercisable without
  real memory exhaustion — **verified live twice**: a single forced
  failure recovered via the retry (event sent, ACKED, state applied); two
  forced failures in a row triggered a real `ESP.restart()`
  (`reset_reason:"SW"`), and the device came back up cleanly with the
  event still durably PENDING in the journal.
- **Low-memory supervisor** (`src/health/low_memory_supervisor.h`):
  classifies internal SRAM's largest contiguous free block as NORMAL/
  WARNING/CRITICAL every 30s (PROD+DEV both). CRITICAL triggers an
  immediate journal compaction; if that doesn't recover it, a controlled
  reboot (`RECOVERY_LOW_MEMORY`).
- **Error-view auto-timeout**: `draw_error_view()` takes over the whole
  screen and previously was only dismissed by the next unrelated render
  call — exactly the incident above. `KioskRuntime` now tracks how long an
  error view has been showing and forces a return to the current
  authoritative state after 20s if nothing else happened
  (`RECOVERY_UI_STALL`) — presentation-only, never touches business state.
- **Controlled reboot + reboot-loop protection + minimal SAFE_MODE, all
  PROVEN on real hardware** (`src/health/recovery_supervisor.h`): every
  automatic reboot logs a structured `RECOVERY_*` code, persists the
  reason + a same-fault streak in NVS, and clears that streak once uptime
  has been stable for a full periodic-check cycle (`recovery_supervisor_mark_stable()`).
  A streak of 3 consecutive same-cause reboots sets `safe_mode`. DEV-only
  `force-safe-mode` forces this without waiting for 3 genuine faults —
  **verified live**: SAFE_MODE boot skips the debug HTTP server entirely
  (still keeps display/input/Wi-Fi/bootstrap-resync/journal-recovery/the
  recovery UI running) and renders a dedicated `safe_mode` screen
  (confirmed via a real serial-captured screenshot) instead of any
  business flow; the recovery menu and Wi-Fi setup gesture both work from
  inside it; and — without even needing a further reboot — `is_safe_mode()`
  flips back to `false` (screen returns to normal business rendering,
  `screen_id` confirmed via screenshot) once 30s of stable uptime clears
  the persisted streak. Only the debug HTTP server itself stays off until
  an actual reboot, since that decision is made once at `setup()` time.
- **Universal escape gesture + local recovery menu, PROVEN on real hardware**
  (`WifiRecoveryController`): hold `*` ~5s opens a menu (RETRY NETWORK /
  RESYNC / WIFI SETUP / RETURN / REBOOT, no unsafe "clear state" option);
  keep holding to ~10s for the existing Wi-Fi setup portal instead. Works
  from every screen, including SAFE_MODE, independent of business/
  provisioning state. A digit key while the menu (or the portal) is open
  is captured by the overlay, never falls through to business input
  (`recovery_overlay_active()`) — verified live via `debug-input`,
  including a REAL bug found and fixed in the process: selecting a menu
  option didn't clear the `'*'`-held timer, so a still-held `'*'` could
  silently trigger the Wi-Fi portal well after the menu interaction was
  over; selecting any option now resets the hold timer, requiring a fresh
  press-and-hold to reach Wi-Fi setup afterward.
- **Lightweight UI stall detection**: reuses `Display::frame_id()` (bumped
  once per render) as "render_generation" and `Renderer::current_screen_id()`
  as "last_screen_id" — no render progress for 5 minutes forces one
  redraw, then one display reinit, then a controlled reboot
  (`RECOVERY_UI_STALL`) if neither helps. Honest limitation: a genuinely
  hung `loop()` would also freeze this check, since it runs on the same
  task — this catches "nothing left to trigger a redraw", not a dead loop.
- **Keypad self-healing (real signal, real threshold)**: `scan_pair()`'s
  existing I2C-error return value now feeds a counter; 50 consecutive
  errors re-initializes the I2C bus (`RECOVERY_KEYPAD_REINIT`) without a
  full re-discovery/recalibration. **Scanner intentionally does NOT get an
  automatic reinit** — a one-way RX UART has no error signal distinct from
  normal idle silence, so an automatic trigger would risk exactly the
  "aggressive periodic reset" this task said not to add; a manual
  `reinit-scanner` DEV command exists instead.
- **Structured recovery codes + bounded history**: `RecoveryCode`
  (LOW_MEMORY/JOURNAL_PRESSURE/TASK_CREATE_FAILED/NETWORK_TIMEOUT/
  STATE_DESYNC/UI_STALL/WATCHDOG/REBOOT_LOOP/SCANNER_REINIT/KEYPAD_REINIT)
  and the last 8 recovery events (code/detail/uptime/journal
  pressure/memory) are exposed in `/debug/device-state`'s `recovery{}`
  block for every profile, not just DEV.

Known gaps in this pass (honestly deferred, not silently skipped):

- **Compaction's final validate+swap+rebuild step is still one synchronous
  call** (fast — reads/renames, not per-record flash writes — but not
  chunked). Only the dominant per-record encode+write cost was made
  incremental.
- **UI stall detection can't catch a truly hung main loop** (see above) —
  only a "nothing renders, but loop() is still running" class of stall.
- **No independent display-init retry redundancy beyond the one reinit
  attempt** UI stall recovery already does, and no input-stall detection
  beyond the keypad I2C-error counter (the scanner has none, by design —
  see above).
- **Fault-injection test matrix**: only the targeted scenarios that map to
  real, previously-observed failure modes were built and exercised
  (compaction crash windows, task-create failure/escalation, SAFE_MODE
  entry/exit) — a full generic A-J synthetic fault-injection suite beyond
  those was not built.

## Next phase

Phase 2 — Server Authoritative State + Workflow Contract: real
stable/transient state machine driven by backend responses, `state_version`
tracking and RESYNC, replacing the `READY_LOCAL` placeholder.
