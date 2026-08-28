# Protocol

**Status: Envelope v1 implemented and verified end-to-end on real hardware**
(boot → identity → event → retry → ack, see docs/RETRY_POLICY.md and
docs/TIME_SYNC.md for the live evidence). **Phase 2: server-authoritative
state contract (bootstrap seeds StateProjection, /events response
success/business-rejected/conflict shapes, GET /state + RESYNC) also
implemented and verified end-to-end on real hardware — see the Phase 2
report for the full scenario-by-scenario evidence.**

## Protocol versioning

```text
protocol_version = 1
```

An unsupported version must be rejected explicitly (`PROTOCOL_UNSUPPORTED_VERSION`),
never parsed "best effort". `BootstrapClient` checks
`protocol.accepted_version` against `{0, 1}` (0 = backend didn't echo one)
and reports `REJECTED` otherwise.

## Delivery semantics

```text
Transport semantics:  AT-LEAST-ONCE
Business effect:      EFFECTIVELY-ONCE
```
(Invariant 10.) The device may retry an event under the same `event_id`
(timeout, lost response, reboot mid-flight). The backend's idempotency
store — not the device — is what makes the *business effect* happen at most
once. Kiosk v2 does not attempt distributed exactly-once delivery.

## Data classes

Four classes, never mixed in the same queue/structure:

| Class | Direction | Durable? | Ordered? | Notes |
|---|---|---|---|---|
| EVENT | device -> backend | yes (journal, Phase 3) | per-device (`device_seq`) | idempotent, business relevant |
| STATE | backend -> device | no (replaceable) | versioned (`state_version`), latest wins | authoritative — Phase 2 |
| TELEMETRY | device -> backend | no, best-effort | no | droppable, never gates business logic. Heartbeat is TELEMETRY. |
| COMMAND | backend -> device | tracked by backend | has lifecycle | contract only, Phase 5 |

## Event Envelope v1 (`src/protocol/event_types.h`, host-tested via `test/host/test_protocol_codec.cpp`)

```json
{
  "protocol_version": 1,
  "device": {
    "device_id": "KIOSK-LASER-01",
    "hardware_id": "esp32s3-4c64cef61b44",
    "boot_id": "0d6bd30f358ed181"
  },
  "event": {
    "event_id": "4d20ecf89678c50072f3a29193e380f2",
    "device_seq": 2001,
    "type": "SCAN"
  },
  "time": {
    "timestamp_device": "2026-08-23T01:52:16Z",
    "uptime_ms": 8234112,
    "sync_status": "SYNCED",
    "sync_age_s": 62
  },
  "context": {
    "expected_state_version": 0,
    "workflow_version": 0,
    "ui_bundle_version": 0
  },
  "payload": {
    "source": "GM65",
    "raw": "WF|EMP|00152"
  }
}
```

`timestamp_device` is JSON `null`, not an omitted field or a fabricated
epoch, whenever `sync_status` is `UNSYNCED`/`SYNCING`/`FAILED` (§17,
invariant 11 — the device clock is never authoritative, and a `1970-01-01`
placeholder would be worse than admitting "we don't know"). Verified by
host test (`KIOSK-070`).

### payload{} — quantity fields on QUANTITY_SUBMITTED (GOOD/DEFECT/REWORK)

`quantity_good`/`quantity_defect`/`quantity_rework` are each independently
OPTIONAL — present-with-a-value or fully absent, never present-as-null. 0 and
absent are NOT the same thing (KIOSK-006): a field the device never sends is
omitted from the JSON object entirely, not encoded as `0` or `null`. Every
other event type (SCAN/FINISH_REQUESTED/CANCEL_REQUESTED) omits all three.
QUANTITY_SUBMITTED sends all three together, once, on the single final
submit that closes the GOOD→DEFECT→(repairable?REWORK:skip) local input
flow (docs/ARCHITECTURE.md's quantity flow) — never a partial/early commit
of just GOOD. The device does not derive `quantity_rework` from
`quantity_defect`; the operator's actual entered value is sent as-is, and
the server independently validates `0 <= quantity_rework <= quantity_defect`
(see REWORK_EXCEEDS_DEFECT below) rather than trusting the device.

### device{} — three different identifiers, never conflated

```text
device_id     logical/business identity MESFlow assigns. "" until explicitly
              provisioned (docs/PROVISIONING.md). Invariant 12: no shared
              fleet default.
hardware_id   immutable-ish, derived from the chip's eFuse MAC
              ("esp32s3-<12 hex>"). Always available. Never business-meaningful
              on its own.
boot_id       random, regenerated every boot. Not durable across reboots.
```

### event{} — identity/ordering

```text
event_id     generated ONCE per business event, stable across every retry
             attempt of that event. Never regenerated mid-retry (verified
             live, docs/RETRY_POLICY.md).
device_seq   monotonic, durable (docs/PROTOCOL.md "device_seq persistence"
             below), allocated ONCE per event, before the first send attempt.
type         SCAN | FINISH_REQUESTED | QUANTITY_SUBMITTED | CANCEL_REQUESTED
             (§79: deliberately generic — SCAN's raw payload is parsed by
             the SERVER, never by the device; CANCEL_REQUESTED is defined
             but not yet wired to a keypad key, Phase 2 scope cut).
```

### context{} — optimistic concurrency (§7, Phase 2)

```text
expected_state_version   the device's claim: "this is the state_version I
                          last saw". 0 if StateProjection has no snapshot
                          yet. The backend rejects with STATE_CONFLICT if
                          its actual state_version has since moved on
                          (see "RESYNC" below) -- renamed from Phase 1's
                          bare `state_version` field to make the semantics
                          ("expected", not authoritative) explicit in the
                          wire format itself.
```

### time{} — device clock is diagnostics-only (invariant 11)

```text
UNSYNCED   never synced this boot
SYNCING    sync attempt in progress
SYNCED     synced and fresh
STALE      was synced, but older than TIME_SYNC_STALE_AFTER_S (24h)
FAILED     an explicit sync attempt failed after previously being synced
```
See docs/TIME_SYNC.md. Backend adds `timestamp_received`/`timestamp_committed`
on its side (Phase 2+) — server time is authoritative for business purposes,
never the device's. Ordering inside the device is `device_seq`, never wall
clock.

## `device_seq` persistence — wear-aware reservation blocks

`device_seq` must be monotonic, durable, and NOT written to NVS on every
single event (flash wear). `SequenceReservation`
(`src/protocol/sequence_reservation.h`, portable, host-tested) implements
the reservation-block technique:

```text
persist high-water mark = 2000
RAM sequence usable = 1001..2000 without another NVS write
```

On construction, it loads the last persisted high-water mark and
IMMEDIATELY reserves (persists) a fresh ceiling above it — before handing
out a single value. This means even a crash after the very first `next()`
call of a boot can never cause a repeat on the next boot: that boot loads
the already-bumped ceiling. Documented cost: up to `block_size - 1`
(`DEVICE_SEQ_RESERVE_BLOCK` = 1000) values are skipped per reboot, not
reused. Verified live across three consecutive reboots on real hardware:
`device_seq` climbed `0 → 1 → 1000 → 2000`, boot_id changing every time,
never resetting.

**A real bug was found and fixed here**: `DeviceSequence` is a member of
`KioskRuntime`, which is a globally-constructed object. Its constructor
used to build the `SequenceReservation` (and touch NVS) directly — but
C++ global static initializers run *before* Arduino has called
`nvs_flash_init()`, so every `Preferences` call silently no-op'd
(`begin()` returns false, reads return their default, writes do nothing —
no crash, no error surfaced). This reset `device_seq` to 1 on every single
boot despite the algorithm itself being correct (proven by
`test/host/test_sequence_reservation.cpp` passing throughout). Fixed by
moving NVS access into an explicit `DeviceSequence::init()`, called from
`KioskRuntime::begin()` — which runs from `setup()`, safely after Arduino's
own NVS init. See `ids.h`'s doc comment; this is exactly the kind of gap
"builds and boots clean" would never have caught — only a real reboot test did.

## Sequence anomaly detection (protocol/error definitions only — Phase 2/3 decides resync)

```text
SEQ_DUPLICATE      backend already processed this device_seq
SEQ_GAP            backend expected a lower device_seq than this one
SEQ_OUT_OF_ORDER   device_seq lower than the last one seen for this device
SEQ_ROLLBACK       device_seq lower than backend's persisted high-water mark
                   for this device (e.g. NVS was cleared/replaced)
```
The device never self-corrects business state based on these — that's a
backend/Phase 2 decision (RESYNC).

## Backend endpoint contract

```text
POST /api/kiosk/v2/bootstrap
POST /api/kiosk/v2/events
POST /api/kiosk/v2/heartbeat
GET  /api/kiosk/v2/state        (Phase 2 — implemented, verified live)
POST /api/kiosk/v2/reconcile    (Phase 3 — not implemented)
GET  /api/kiosk/v2/desired      (Phase 2 — not implemented, superseded by state{}/view{} below)
POST /api/kiosk/v2/reported     (Phase 2 — not implemented)
GET  /api/kiosk/v2/commands     (Phase 5 — not implemented)
GET  /api/kiosk/v2/bundles/{version}   (Phase 4 — not implemented)
GET  /api/kiosk/v2/firmware-policy     (Phase 6 — not implemented)
```
`tools/mock_backend/mock_backend.py` implements bootstrap/events/heartbeat/
state as a full STATEFUL per-device backend (real employee/operation
reference data, a real transition table, a real idempotency store, global
`server_seq`), plus DEV-only inspection/admin endpoints (`GET
/mock/state/<id>`, `POST /mock/admin/<id>` for disable/maintenance/reset)
and a test-only `POST /_test/drop-next` chaos control (delays the next N
event responses past the device's hard deadline, to simulate a lost ACK —
see docs/RETRY_POLICY.md; none of `/mock/*`/`/_test/*` are part of the real
protocol).

### Device authorization — `X-Kiosk-Token` (added 2026-08-28, P0 fix)

`POST /events` and `GET /state` now require a real, ACTIVE `kiosk_identities`
row for the claimed `device_id`, PLUS an `X-Kiosk-Token` header whose value
hashes to that row's `token_hash` (see mesflow's
`app/mesflow/web/kiosk_v2.py::_authorize_kiosk_v2_device`). Before this fix
neither endpoint checked device identity at all — a real P0: any caller
that knew a device_id (a public, non-secret string sent on every request)
could drive real START/FINISH/quantity business mutations, and an
admin-DISABLED device kept working forever.

Responses on failure: `403 {"error":{"code":"DEVICE_NOT_ALLOWED"}}` for an
unknown/PENDING/SUSPENDED/DISABLED device or a wrong/mismatched token;
`401 {"error":{"code":"AUTH_REQUIRED"}}` for an ACTIVE device presenting no
token at all. `retry_policy.cpp::classify_http_result()` already classified
any 4xx (including 401/403) as `HTTP_4XX`/non-retryable *before* this fix
landed (its own comment: "Phase 1 doesn't yet have a real auth handshake to
react to them differently; that's a later integration point, not invented
here") — this IS that integration point; no retry-taxonomy change was
needed, a device just correctly stops retrying and surfaces the failure.

**Deliberately NOT required on** `/bootstrap` or `/heartbeat` — those stay
on the existing identity+status-only check (no token), since a brand-new
or not-yet-provisioned device must still be able to reach them to discover
config and learn its own real status; requiring a token there would break
onboarding, and neither call can be tricked into a business *effect* the
way `/events` can. **Also not required on** `GET /ui-bundles/<version>` —
pure static render-instruction content, no business/PII data, deliberately
public (see that route's own comment).

**Provisioning the token onto a device**: the plaintext only ever exists in
the response of MESFlow's `POST /api/kiosk-identities/<id>/approve`
(admin-session-authenticated) or `POST /api/kiosk/bind` (requires proof of
the CURRENT token to rotate an already-ACTIVE identity, or
`MESFLOW_ALLOW_LEGACY_KIOSK_AUTOBIND` for a brand-new one) — both pre-exist
this fix and are already secure. `bootstrap()` deliberately never hands the
token back on any call (it has no auth of its own, so doing so would let
anyone who merely knows the device_id harvest it and defeat this whole
check). Getting it onto the physical device is therefore an out-of-band
step, the same shape as the existing `wifi:`/`api-endpoint:`/
`expected-env:` serial commands: type `kiosk-token:<token>` at the serial
console (`DeviceIdentity::set_kiosk_token`, NVS-persisted, no reboot
needed — `KioskRuntime` reads it fresh on every send/state-fetch).

**Compatibility note**: a device that was auto-bound (or otherwise reached
ACTIVE) before this fix has a real `token_hash` in `kiosk_identities`
already (set at bind time) but was never HANDED that plaintext, since
kiosk_v2's bootstrap always discarded it. Such a device will get a clean
401 on every `/events`/`/state` call — by design, not a bug — until an
admin re-approves/rebinds it and the resulting token is provisioned via
`kiosk-token:` above.

### GET /api/kiosk/v2/state?device_id=... (Phase 2, `network/state_client.*`)

Returns the SAME `state{}`/`workflow{}`/`view{}` shape as a bootstrap/events
response, just without the envelope wrapper. Used only for RESYNC (below) —
`AsyncStateFetcher` is a single-attempt, no-retry background fetch (a resync
is rare and operator-visible; a failed fetch just leaves the device in
RESYNCING until the next attempt, rather than retrying blindly).

### Bootstrap request (`BootstrapClient`, `network/bootstrap_client.*`)

```json
{
  "device_id": "KIOSK-LASER-01", "hardware_id": "esp32s3-...", "boot_id": "...",
  "runtime": { "version": "0.2.0", "protocol_version": 1 },
  "hardware": { "chip": "ESP32-S3", "flash_mb": 16, "psram_mb": 8,
                "display": "ILI9341", "scanner": "GM65", "keypad": "PCF8574" },
  "current": { "ui_bundle": 0, "workflow": 0, "state_version": 0, "last_device_seq": 2000 }
}
```

Response — Phase 2: the SAME `state{}`/`workflow{}`/`view{}` shape as
`/events` and `/state`, and it IS acted upon now (`KioskRuntime::
on_bootstrap_result`): this is the snapshot `StateProjection` seeds itself
with as the very first thing it ever holds each boot (invariant 15).

```json
{
  "accepted": true, "device_status": "ACTIVE",
  "server_time": "2026-08-23T08:29:45Z",
  "protocol": { "accepted_version": 1 },
  "desired": { "config_version": 1, "workflow_version": 1, "ui_bundle_version": 0 },
  "state": { "name": "WAIT_EMPLOYEE", "version": 1 },
  "workflow": { "version": 1 },
  "view": {}
}
```

One shot at boot, blocking (acceptable — it's not on the hot scan path §23
cares about). `BootstrapStatus`: `NOT_ATTEMPTED` / `OK` / `REJECTED`
(accepted:false, or unsupported version) / `FAILED` (no response at all).
Exposed via `/debug/device-state`'s `protocol.bootstrap`.

**A real bug found and fixed this phase**: `BootstrapClient::attempt()` used
to POST directly to the device's configured `/events` URL instead of
deriving the `/bootstrap` sibling endpoint (`network/endpoint_utils.h`'s
`derive_sibling_endpoint`, the same helper `heartbeat_client.cpp` already
used correctly). Every bootstrap request silently landed on the mock
backend's `/events` handler instead, which correctly rejected it (a
bootstrap body has no top-level `protocol_version` field) and replied with
a genuine HTTP 200 body containing `"accepted": false` — indistinguishable,
from the client's point of view, from a real bootstrap rejection. Found by
noticing the mock backend's own request log never showed a matching
`BOOTSTRAP` line for the device's attempts. Fixed by deriving `/bootstrap`
before use; confirmed live afterward (`BOOTSTRAP_OK` + `STATE_APPLY
source=BOOTSTRAP apply=APPLIED`).

## /events response contract (Phase 2, `event_response.h`, three shapes)

```json
// SUCCESS
{"accepted": true, "event_id": "...", "server_seq": 7,
 "state": {"name": "WAIT_OPERATION", "version": 2},
 "workflow": {"version": 1}, "view": {"employee_name": "Nguyen Van A"}}

// BUSINESS_REJECTED -- still carries the (unchanged) current snapshot
{"accepted": false, "event_id": "...", "server_seq": 8,
 "error": {"code": "EMPLOYEE_NOT_FOUND", "message": "Nhan vien khong hop le"},
 "state": {"name": "WAIT_EMPLOYEE", "version": 1},
 "workflow": {"version": 1}, "view": {}}

// CONFLICT_RESYNC -- deliberately NO state{} here; see RESYNC below
{"accepted": false, "event_id": "...",
 "error": {"code": "STATE_CONFLICT"}, "action": "RESYNC",
 "current_state_version": 9, "server_seq": 10}
```

All three are HTTP 200 (§83: every BUSINESS outcome — accept, reject,
conflict — is a successful transport round trip; only
`IDEMPOTENCY_KEY_REUSE_MISMATCH` uses HTTP 409, a genuine transport-level
conflict). A response matching none of these three shapes is `MALFORMED` —
the device applies nothing from it (invariant 14: a bad parse is never
license to guess).

## RESYNC (Phase 2, `KioskRuntime::start_resync`/`handle_resync_result`)

```text
/events responds CONFLICT_RESYNC (or StateProjection::apply() returns
REJECTED_STALE/REJECTED_INCONSISTENT for any other reason)
  -> device enters local, transient RESYNCING (draw_resyncing_screen)
  -> GET /api/kiosk/v2/state?device_id=... (AsyncStateFetcher, single
     attempt, no self-merge -- §8)
  -> StateProjection::apply(fetched snapshot)
       APPLIED / APPLIED_IDENTICAL -> RESYNCING clears, render the result
       REJECTED_STALE/INCONSISTENT/UNSUPPORTED -> stays RESYNCING; a real
         edge case confirmed live: if the fetched snapshot's version is
         itself LOWER than what the device already holds (e.g. a DEV-only
         mock-backend `reset` rewound state_version backward), the device
         correctly refuses forever (invariant 16) until its next reboot,
         which re-seeds StateProjection from empty and accepts unconditionally
```

### Heartbeat (TELEMETRY — best-effort, never retried, never journaled)

Body is the same JSON as `/debug/device-state` (single source of truth,
`runtime/status_snapshot.*`) minus `framebuffer_bytes`. Cadence:
`HEARTBEAT_INTERVAL_MS` (20s). A failed heartbeat just tries again next
cycle — no backoff of its own, since losing one heartbeat is harmless by
design.

## Idempotency store — backend contract (unchanged from Phase 0 design)

```text
device_id
event_id UNIQUE
device_seq
payload_hash
processing_status
result_snapshot
received_at
```
Business transaction must be atomic: receive + dedupe + business mutation +
`state_version` increment + response snapshot, in one consistency boundary.
Retention `>= max offline window + safety margin`.

## Error taxonomy (Phase 2 additions)

```text
STATE_CONFLICT                    expected_state_version stale -> RESYNC (see above)
STATE_INVALID_TRANSITION          event type not valid from the current state
STATE_VERSION_REGRESSION          device-side: StateProjection saw a version go backward
STATE_UNSUPPORTED                 device-side: server sent a state name this firmware
                                   doesn't recognize (§53 -- never mapped to WAIT_EMPLOYEE)
STATE_SNAPSHOT_INCONSISTENT       same version, different content, on either an /events
                                   response or a /state RESYNC fetch
EMPLOYEE_NOT_FOUND / EMPLOYEE_DISABLED
OPERATION_NOT_FOUND / OPERATION_CLOSED
SESSION_ALREADY_ACTIVE
QUANTITY_INVALID                  QUANTITY_SUBMITTED with no quantity_good at all, or any of
                                   good/defect/rework negative
REWORK_EXCEEDS_DEFECT             quantity_rework > quantity_defect -- server independently
                                   re-validates this even though the device also enforces it
                                   locally before ever sending (defense in depth, §15 of the
                                   GOOD/DEFECT/REWORK quantity flow task)
IDEMPOTENCY_KEY_REUSE_MISMATCH    same (device_id, event_id), different payload hash
                                   -- the only HTTP 409 in this protocol
```

## A real environment limitation found this session (not a code defect)

This dev sandbox's Wi-Fi network segment cannot route back to this host's
own LAN/VPN addresses from the kiosk (an isolated guest/IoT VLAN policy) —
`ping`/HTTP from host→device works, but device→host does not, even though
the device reaches the public internet fine (NTP succeeds quickly,
Cloudflare Quick Tunnel's edge was reachable but never routed traffic back
in this sandbox for reasons not fully diagnosed). Phase 1's protocol
verification (bootstrap/events/retry wire mechanics) was done against
`httpbin.org`'s public echo endpoint as a workaround, since the real mock
backend wasn't reachable from the device at that time.

**Phase 2 resolved this** for real business-state verification: an SSH
reverse tunnel (`ssh -R 80:localhost:8799 nokey@localhost.run`) exposes
`tools/mock_backend.py` at a public `https://*.lhr.life` hostname the
device configures as its `api_endpoint`. Every Phase 2 scenario in the
report (bootstrap, all 6 business states, business rejection, STATE_CONFLICT
+ RESYNC, dropped-response + idempotent retry, DEVICE_DISABLED/MAINTENANCE,
reboot-restore) was verified live through this path against the project's
own stateful mock backend — not httpbin — with real hardware evidence
(serial logs, `/debug/device-state`, and `/debug/screenshot` captures) for
every gate. The `localhost.run` free tier is NOT durably reliable across a
long session (it silently rotates the assigned hostname roughly every
15-20 minutes of connection age, requiring the device's `api_endpoint` to
be re-pointed) — acceptable for this phase's DEV verification, but not a
path to depend on for anything beyond it.
