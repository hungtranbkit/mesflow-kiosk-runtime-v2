# Time Sync

**Status: implemented, verified live** (NTP synced in as little as ~100ms
to a few seconds on real hardware; also observed taking ~100s once on a
weak-signal network — see "Live evidence").

## Invariant 11: device clock is NOT authoritative

`TimeSync` (`src/network/time_sync.*`) exists to give operators/logs a real
human-readable timestamp and to compute staleness — nothing in this
codebase makes an ordering or business decision based on wall-clock time.
`device_seq` is the real ordering authority (docs/PROTOCOL.md), regardless
of sync state.

## State machine

```text
UNSYNCED -> SYNCING -> SYNCED -> STALE (after TIME_SYNC_STALE_AFTER_S, 24h)
                    \-> FAILED (clock became implausible after being synced --
                                shouldn't normally happen, handled honestly
                                rather than silently kept as SYNCED)
```

`begin()` is called once when Wi-Fi first connects (`configTime(0, 0,
TIME_SYNC_NTP_SERVER)` — UTC only, no DST/timezone handling, since device
timestamps are diagnostics-only). `poll()` (called every `loop()`
iteration) is cheap: just checks whether `time(nullptr)` has become
plausible (>= 2024-01-01, a floor that will itself go stale eventually —
"clearly not 1970", not a precise boundary) and re-issues `configTime()` on
a `TIME_SYNC_RETRY_INTERVAL_MS` (60s) cadence while not yet synced, and
periodically even while `STALE`, so a long-uptime device keeps trying to
reconfirm.

## Never blocks the runtime (KIOSK-069)

`configTime()` itself is non-blocking (starts the SNTP client in the
background); `poll()` never waits on anything. If NTP never succeeds, the
kiosk keeps running indefinitely in `UNSYNCED` — verified conceptually via
this non-blocking design, and empirically: the device ran and served scans/
debug requests normally throughout every `TIME_SYNC_STARTED` → eventual
`TIME_SYNC_OK` window observed this session, including one that took
~100 seconds.

## `timestamp_device` is never fabricated (§17, KIOSK-070)

```json
{"timestamp_device": null, "uptime_ms": 102233, "sync_status": "UNSYNCED"}
```

When `sync_status` is `UNSYNCED`/`SYNCING`/`FAILED`, `timestamp_device` is
JSON `null` — never an omitted field pretending nothing happened, and never
a fabricated `1970-01-01` epoch. Verified by host test
(`test_protocol_codec.cpp`) and confirmed the event still encodes/sends
successfully with `sync_status: UNSYNCED` (the runtime doesn't gate sending
on time sync at all — only on provisioning state and backend configuration).

## Live evidence (real hardware, this session)

```text
{"code":"TIME_SYNC_STARTED","message":"pool.ntp.org","uptime_ms":4532}
{"code":"TIME_SYNC_OK","uptime_ms":4532}
```
(effectively instant on one boot) and, on a different boot with weaker
Wi-Fi:
```text
{"code":"TIME_SYNC_STARTED","message":"pool.ntp.org","uptime_ms":4441}
{"code":"TIME_SYNC_OK","uptime_ms":109065}
```
(~105 seconds — several retry cycles, device kept running normally
throughout, confirmed via concurrent heartbeat/diagnostic log lines and
`/debug/device-state` remaining responsive).
