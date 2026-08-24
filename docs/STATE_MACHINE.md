# State Machine — Phase 2 implementation (canonical business states v1)

## Canonical business states v1 (implemented, server-authoritative)

```text
WAIT_EMPLOYEE
WAIT_OPERATION
SESSION_ACTIVE
QUANTITY_INPUT
DEVICE_DISABLED
MAINTENANCE
```

These are exactly `kiosk::protocol::BusinessState` (`state_projection.h`) plus
the reserved `UNSUPPORTED` value a real server can never legitimately send
(it exists purely as "the device received a state name from a newer server
than this firmware understands" — see invariant 16/§53, never silently
mapped to `WAIT_EMPLOYEE`). Every one of the six is confirmed live on real
hardware end-to-end through the tunnel-connected stateful mock backend (see
the Phase 2 report for the exact evidence per state).

The SERVER owns the transition table entirely
(`tools/mock_backend/mock_backend.py:apply_event`):

```text
WAIT_EMPLOYEE  --EMPLOYEE_SCANNED-->  WAIT_OPERATION
WAIT_OPERATION --OPERATION_SCANNED--> SESSION_ACTIVE
SESSION_ACTIVE --FINISH_REQUESTED-->  QUANTITY_INPUT
QUANTITY_INPUT --QUANTITY_SUBMITTED--> WAIT_EMPLOYEE
```

The device sends a small, generic `EventType` (`SCAN`/`FINISH_REQUESTED`/
`QUANTITY_SUBMITTED`/`CANCEL_REQUESTED`) — never a business-specific one like
`EMPLOYEE_SCANNED` — and the server alone decides what a SCAN's raw payload
means and whether the transition is valid (§79). `CANCEL_REQUESTED` is
defined in the protocol but not yet wired to a keypad key (deliberate Phase 2
scope cut — no UI trigger exists for it yet).

## Stable vs transient

The six business states above are the only ones `StateProjection` ever
holds, and it is the ONLY thing ever rendered when a snapshot exists. They
are never persisted to NVS/flash — invariant 15: a reboot always re-derives
them from a fresh `/bootstrap` response, never from local storage.

Transient, LOCAL-ONLY conditions (never part of `StateProjection`, never
restored, never sent to or trusted from the server):

```text
(pre-bootstrap) waiting screen  -- shown before StateProjection has ever
                                    held a snapshot this boot
RESYNCING                       -- shown while a STATE_CONFLICT response's
                                    GET /state fetch is outstanding
local quantity digit buffer     -- what the operator has typed so far in
                                    QUANTITY_INPUT, before pressing '#'
```

`RESYNCING` deliberately has its own `Renderer::draw_resyncing_screen`
rather than living inside `BusinessState` — it is a device-local "I know my
copy might be wrong, don't act yet" condition, not something the server
ever declares.

Reserved, NOT YET implemented (future phases; do not assume any code path
below exists):

```text
BOOT / OFFLINE / SAFE_MODE          -- Phase 3 (durable journal, offline policy)
CONNECTING / REGISTERING / SYNCING  -- reserved names only
```

## Reboot rule (implemented)

```text
StateProjection starts empty every boot (has_snapshot() == false)
  -> device shows the pre-bootstrap waiting screen
  -> bootstrap succeeds AND carries a state{}/workflow{}/view{} snapshot
       -> StateProjection::apply() (always APPLIED: nothing to be "stale"
          against yet) -> render whatever the server says RIGHT NOW
  -> bootstrap fails / has no snapshot
       -> stays on the pre-bootstrap waiting screen (no business event
          traffic is possible without a snapshot to send
          expected_state_version against)
```

A backend-unreachable-at-boot OFFLINE fallback (using a locally-persisted
"last trusted stable snapshot") is Phase 3 scope (durable journal) and is
NOT implemented — Phase 2 has no offline story for business state at all;
if the backend can't be reached, the device simply has no server authority
to render against yet and stays on the waiting screen.

## Immediate local feedback (§12, unchanged since Phase 0/1)

```text
SCAN     -> draw_scan_received() immediately -> "Đã nhận mã" -> "Đang kiểm tra..." -> server response
KEYPAD   -> render_current_business_state("Đang gửi...") immediately -> server response
```

Still optimistic **presentation** only, never an optimistic **business**
confirmation — the screen after a keypad/scan event always waits for
`StateProjection::apply()` to actually run before showing anything that
looks like a business outcome.
