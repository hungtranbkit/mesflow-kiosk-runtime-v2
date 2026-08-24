# Architecture — MESFlow Kiosk Runtime v2

## Why this project exists, and why it is separate

The legacy kiosk firmware (`mesflow/esp-kiosk`) grew business logic directly
into the device: it decides workflow transitions, validates employees and
operations, and owns UI layout, all inside a single ~6300-line `.cpp` file.
That makes every policy change a firmware build+flash, and it means the
device — not the backend — is the source of truth for what is "true" about
a session.

This project starts over, clean-room, with the opposite split:

```text
ESP32 = hardware runtime + renderer + local cache + offline journal + secure transport
Backend = source of truth + workflow + business state + UI + policy + device management
```

The legacy firmware is **read-only reference material** for hardware facts
(pinout, part numbers, UART settings) — see `docs/HARDWARE.md`. Nothing here
imports, copies, or refactors its source. It keeps running unmodified in
production/dev until a deliberate, staged migration (`docs/DEVICE_LIFECYCLE.md`).

## Target dataflow

```text
Scanner / Keypad / Touch
        |
        v
   ESP32 Runtime  (local event, immediate presentation feedback only)
        |
        v
  Event Protocol   (device_id, boot_id, device_seq, event_id, payload)
        |
        v
  MESFlow Backend  (validates, applies business rules, owns state)
        |
        v
 Authoritative State  (state_version)
        |
        v
  View Model / UI State  (what to render, not how to decide)
        |
        v
   ESP32 Renderer
```

Once this is in place, the operational split becomes:

```text
Change text/layout/workflow/policy  -> update backend/bundle, no firmware build
Change driver/security/runtime      -> firmware OTA
Business error                      -> backend timeline
Device error                        -> telemetry/diagnostics
Network loss                        -> journal + reconcile
Power loss                          -> recover from durable state
Duplicate request                   -> harmless (idempotent)
Bad UI release                      -> rollback to last-known-good bundle
Bad firmware                        -> OTA rollback (A/B)
```

## Invariants (do not violate)

1. **SERVER OWNS BUSINESS STATE.** The device never decides whether an
   employee/operation/PO is valid, or whether a session may start — it only
   asks and renders the answer.
2. **DEVICE MAY REPEAT AN EVENT; BUSINESS EFFECT MUST NOT REPEAT.** Transport
   is at-least-once; the backend's dedupe on `event_id` makes the effect
   effectively-once. See `docs/PROTOCOL.md`.
3. **DEVICE MUST ALWAYS HAVE A LAST-KNOWN-GOOD EXECUTABLE STATE.** Firmware,
   UI bundle, and stable-state snapshot each keep a previous-good copy; a
   failed update never leaves the device unable to boot into something
   working.
4. **NO REMOTE DATA MAY BECOME EXECUTABLE CODE.** UI bundles, config, and
   commands are data interpreted by a fixed, whitelisted runtime — never
   script, bytecode, or eval'd expressions.
5. **LOSS OF NETWORK OR POWER MUST NOT SILENTLY LOSE A COMMITTED OPERATOR
   ACTION.** A durable journal (`docs/OFFLINE.md`) exists for exactly this;
   if durability can't be guaranteed, the device says so instead of
   pretending success.
6. **BUSINESS EVENTS AND TELEMETRY ARE DIFFERENT DATA CLASSES.** Events are
   durable, ordered, idempotent, business-relevant. Telemetry is best-effort
   and droppable. They are never queued in the same structure.
7. **A DISPLAY/UI CHANGE IS NOT VERIFIED UNTIL THE RENDERED OUTPUT CAN BE
   OBSERVED.** "It compiles and the serial log looks right" is not evidence
   about what the screen actually shows.
8. **SERIAL SUCCESS DOES NOT IMPLY VISUAL SUCCESS.** A real bug (text
   overflow corrupting an adjacent row) was found this way during this
   project's own development — nothing in the serial logs revealed it; only
   an actual screenshot did. See `docs/VISUAL_DEBUG.md`.
9. **DEVELOPMENT BUILDS MUST PROVIDE MACHINE-READABLE VISUAL AND UI STATE
   EVIDENCE.** `docs/VISUAL_DEBUG.md`'s `/debug/screenshot`, `/debug/ui-state`,
   `/debug/device-state` — DEV/LAB profile only, never assume they exist in
   a production build.

Any change touching display/font/typography/geometry/screen/keypad-visual-
feedback/Wi-Fi-setup-UI must finish with an actual capture-and-inspect step
(`docs/VISUAL_DEBUG.md`), not just "builds and boots clean."

10. **DEVICE DELIVERY IS AT-LEAST-ONCE; BUSINESS EFFECT MUST BE
    EFFECTIVELY-ONCE.** Same substance as invariant 2, restated for Phase 1's
    protocol work: the device may retry an event under the same `event_id`
    (verified live — same `event_id`/`device_seq` across 5 retry attempts,
    docs/RETRY_POLICY.md); it is the backend's dedupe, not the device's, that
    keeps the effect from repeating.
11. **DEVICE CLOCK IS NOT AUTHORITATIVE.** `timestamp_device` is diagnostics
    only and is JSON `null` (never a fabricated `1970-01-01`) whenever the
    device isn't confident in its own clock. `device_seq` is the real
    ordering authority. See docs/TIME_SYNC.md.
12. **DEVICE IDENTITY MUST NOT DEPEND ON A SHARED FLEET SECRET.** No default/
    auto-derived `device_id` that quietly becomes the real business identity
    (Phase 0 did exactly this — auto-generating `"KIOSK-DEV-XXXX"` from the
    MAC — and Phase 1 fixes it: `hardware_id` and `device_id` are two
    different things, and `device_id` is empty until explicitly provisioned.
    See docs/PROVISIONING.md.
13. **THE DEVICE NEVER COMMITS A BUSINESS-STATE TRANSITION WITHOUT SERVER
    AUTHORITY.** No `if employee scanned: state = WAIT_OPERATION` on-device.
    Every canonical business state (`kiosk::protocol::BusinessState`) only
    ever changes because `StateProjection::apply()` accepted a snapshot the
    server sent — verified live on real hardware (KIOSK-097-100): the
    device's own key/scan handlers pick which generic `EventType` to send,
    never which state to become.
14. **DEVICE STATE IS A PROJECTION, NOT A SOURCE OF TRUTH.** `StateProjection`
    holds a read-only mirror of whatever the server last told it — it is
    reset to empty on every reboot (invariant 15) and rebuilt only from
    server responses (`/bootstrap`, `/events`, `/state`).
15. **A REBOOT NEVER RESTORES A LOCALLY-REMEMBERED BUSINESS STATE.**
    `StateProjection` starts with `has_snapshot()==false` every boot; the
    first snapshot it ever holds this boot comes from the SAME bootstrap
    response that gates business-event sending, never from NVS/flash.
    Verified live: rebooting mid-session did not "resume" the last screen —
    it re-fetched and rendered whatever the server's current state actually
    was at that moment (see the Phase 2 report's reboot-restore scenario,
    including the honest stuck-RESYNCING edge case found when a DEV-only
    admin `reset` rewound the server's version backward).
16. **A SERVER `state_version` MUST NEVER MOVE BACKWARD ON THE DEVICE.**
    `StateProjection::apply()` rejects (`REJECTED_STALE`) any incoming
    snapshot whose version is lower than the currently-held one, regardless
    of source (`/events` success, business rejection, or a `/state` RESYNC
    fetch) — confirmed live to the point of exposing a real edge case: a
    DEV-only mock-backend `reset` that rewinds the counter leaves the device
    correctly, permanently refusing to reconcile until its next reboot. That
    is invariant 16 working as designed, not a bug to route around.

## Backend-controlled UI vs. ESP32 built-in — a hard boundary

```text
BACKEND-CONTROLLED                    ESP32 BUILT-IN
─────────────────────                 ─────────────────────
normal screens                        hardware drivers
text, font selection, font size       renderer, font engine
position, layout, colors, theme       emergency font
workflow, visibility, view models     boot screen
                                       Wi-Fi setup (docs/WIFI_RECOVERY.md)
                                       safe mode
                                       diagnostics
                                       fatal recovery
                                       OTA recovery
                                       '*' 10s physical trigger
```

Backend controls the **normal operating** interface (Phase 4,
`docs/UI_SCHEMA.md`, not implemented yet). The device always keeps a
**minimal rescue interface** that works even when the bundle is corrupt,
the backend is unreachable, or the certificate is invalid — because that is
exactly when a device needs to be fixable.

### Emergency UI priority

When more than one of these wants the screen, higher wins:

```text
1. FATAL / SAFE MODE
2. WIFI RECOVERY
3. OTA RECOVERY
4. LOCAL DIAGNOSTICS
5. BACKEND BUNDLE UI
```

A backend-controlled bundle can never suppress or override an emergency
screen. As of this writing, only tier 2 (Wi-Fi recovery,
`docs/WIFI_RECOVERY.md`) and the `BOOT` screen are actually implemented —
`SAFE_MODE`/`OTA_RECOVERY`/`FATAL_ERROR`/`DEVICE_RECOVERY` are reserved
names, not working code, pending the Phase 3/6 functionality (boot-loop
detection, OTA A/B) they depend on.

## Phase status

This repository currently implements through **Phase 2 — Server
Authoritative State + Workflow Contract** (see `README.md` for exact scope
and the Phase 2 report for full PASS/FAIL evidence). Phases 3-6 (offline
reliability/durable journal, backend-controlled UI, fleet integration,
release safety) are designed in the other `docs/` files but not yet
implemented in code. Do not skip ahead of the reliability foundation to
build UI polish — see project instructions §66.

## Module layout (Arduino `src/` subfolder convention)

```text
firmware/kiosk_runtime_v2/
├── kiosk_runtime_v2.ino      entry point (setup/loop wiring only)
└── src/
    ├── config/     compile-time pins, timeouts, build metadata
    ├── runtime/    event bus, boot diagnostics, kiosk state runtime
    ├── hardware/   display, scanner, keypad drivers (no business logic)
    ├── network/    wifi manager, api client (mock backend today)
    ├── protocol/   event envelope + codec — plain C++, no Arduino.h,
    │               host-testable (test/host/) and framework-portable.
    │               Phase 2 additions here: state_projection (StateProjection/
    │               BusinessState/ApplyResult), event_response (/events
    │               response parsing), json_extract (whitespace-tolerant
    │               field extraction, shared by bootstrap/event/state parsing)
    ├── storage/    config_store (NVS-backed). durable_journal etc. is
    │               Phase 3 and intentionally not stubbed out yet
    ├── ui/         renderer — minimal component drawing, no bundle format
    │               yet (Phase 4)
    └── health/     structured_log
```

`src/security/` is intentionally empty in Phase 0 (that's Phase 1: identity,
mTLS, provisioning). See `NOT IMPLEMENTED YET` in the top-level README before
assuming any security property holds.
