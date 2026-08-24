# Offline Journal & Reliability (Phase 3 design — not implemented yet)

Phase 0 has no durable journal. This document fixes the design so Phase 3
implements against a stable contract instead of improvising later, and so
`api_client`/`event_bus` in Phase 0 are already shaped to slot a journal in
without a rewrite.

## What is durable vs not

Durable (must survive reboot/power loss, replayed in `device_seq` order):

```text
START_REQUEST
FINISH_REQUEST
QUANTITY_SUBMIT
any critical operator action
```

Non-durable (never written to the durable journal):

```text
heartbeat
spinner / temporary UI state
screen view telemetry
```

## Record shape (target)

```text
record_version
event_id
device_seq
event_type
payload_length
payload
CRC
commit_marker
sync_status
retry_metadata
```

A record missing a valid commit marker (i.e. power was lost mid-write) must
never be replayed as if it succeeded. Phase 0 already includes a portable
CRC32 helper (`src/protocol/crc32.h`, host-tested) in anticipation of this,
but no journal file format is implemented yet.

## Offline policy per event category

```text
SAFE_OFFLINE        telemetry
CONDITIONAL_OFFLINE employee scan, operation scan, start/finish, quantity
ONLINE_ONLY         admin reassignment, security operation, firmware operation
```

Not every business action is allowed to run offline by default — category is
assigned per event type, explicitly, not inferred.

## Queue pressure

```text
< 70%       NORMAL
70-90%      WARNING
90-100%     RESTRICTED
100%        SAFE_STOP
```

Never overwrite an older business event to make room for a newer one. If a
new action cannot be durably recorded, the device must show a clear error —
never a fake "success".

## Reconciliation

```text
device -> reconcile -> send pending events in device_seq order
                     -> server dedupes
                     -> server applies / rejects / conflicts
```

A conflict gets `HUMAN_REVIEW` status. After an admin resolves it, the
backend bumps `state_version`; the device learns this via heartbeat/state
poll (`RESYNC_REQUIRED`), then re-fetches authoritative state and clears the
local conflict marker. The device must never sit on `HUMAN_REVIEW` forever
without a resync path.

## No silent fallback (applies beyond just the journal)

- Security verification fails -> do not silently accept.
- Journal full -> do not silently drop the write.
- UI bundle incompatible -> do not attempt a partial load.
- Server reports state conflict -> do not guess the state locally.

## Relationship to Phase 0

`event_bus.h/.cpp` already separates event publication from delivery, and
`api_client` already returns a typed result (not a bare bool), specifically
so that Phase 3 can insert "write to journal, then attempt send, then mark
synced" without restructuring the runtime loop.
