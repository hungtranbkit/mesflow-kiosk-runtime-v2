# Retry Policy

**Status: implemented, verified on real hardware** (see "Live evidence"
below — real exponential backoff, correct classification, retry_count,
event_id/device_seq preservation, and non-blocking behavior all observed on
the actual device, not just in host tests).

## Classification (`src/protocol/retry_policy.h`, host-tested)

```text
network timeout / connection refused / DNS failure  -> retryable
502/503/504                                          -> retryable (API_HTTP_5XX)
429                                                   -> retryable, honors Retry-After
400 (and other 4xx)                                  -> NOT retryable (no blind retry)
401/403                                              -> NOT retryable (no real auth
                                                         handshake exists yet to react
                                                         to differently -- Phase 1 doesn't
                                                         invent one)
TLS verification failure                             -> NOT retryable (no mTLS wired
                                                         into the live client yet, see
                                                         docs/SECURITY.md)
```

Every outcome is a structured `HttpOutcome` (`ok`, `http_status`,
`error_code`, `retryable`, `retry_after_s`) — never a bare `http_status = -1`
as the primary representation. The underlying HTTP client's negative
transport codes (e.g. ESP32 HTTPClient's `-1` = connection refused, `-11` =
read timeout) and this codebase's own `-999` hard-deadline marker are all
mapped to one of the taxonomy codes below, not left as magic numbers for
callers to interpret themselves.

## Backoff + jitter (`compute_backoff_ms`, host-tested)

```text
base:  1000ms
attempt 1: 1000ms   (+0-25% jitter)
attempt 2: 2000ms   (+0-25% jitter)
attempt 3: 4000ms   (+0-25% jitter)
attempt 4: 8000ms   (+0-25% jitter)
attempt 5+: 30000ms (capped, +0-25% jitter)
```

`RETRY_BACKOFF_BASE_MS`/`RETRY_BACKOFF_MAX_MS`/`RETRY_JITTER_PCT_MAX` in
`runtime_config.h`. Max attempts per event: 5 (`kMaxAttempts` in
`api_client.cpp`) — after that, the event is reported failed; there is no
durable journal yet to hand it off to (Phase 3), so the operator sees an
honest "chưa được lưu" message rather than a fake success.

429 responses honor a numeric `Retry-After` header over the normal backoff
schedule when present (`parse_retry_after`, host-tested). The HTTP-date form
of `Retry-After` is NOT supported — honestly unsupported, not guessed at.

## Non-blocking (§23) — the actual architecture change from Phase 0

Phase 0's `api_client` blocked the caller (with a hard deadline) for a
single attempt. Phase 1 replaced this with `AsyncEventSender`
(`network/api_client.*`): the entire retry loop — including every backoff
sleep — runs on its own FreeRTOS task. `KioskRuntime::poll()` (called every
`loop()` iteration) just checks whether that task has finished; it never
blocks. Display/keypad/scanner/DebugServer/heartbeat all keep running
normally while a multi-attempt retry sequence is in flight in the
background.

Single-in-flight by design: a new scan while one is still sending is
rejected with an honest `EVENT_DROPPED_BUSY` message (no durable journal to
queue it in yet — Phase 3).

## Live evidence (real hardware, this session)

Injected one `SCAN` via `/debug/input` against a backend forced to return
503 (`http://httpbin.org/status/503`, since this dev environment's own
network topology didn't let the real device reach the mock backend's LAN
address — see docs/PROTOCOL.md's note on this). Observed in the device's
own structured logs:

```text
EVENT_CREATED   event_id=4d20ecf... device_seq=1
attempt=1  status=-1   NET_CONNECT_REFUSED  retryable=1   backoff_ms=1100
attempt=2  status=-1   NET_CONNECT_REFUSED  retryable=1   backoff_ms=2100
attempt=3  status=503  API_HTTP_5XX         retryable=1   backoff_ms=4440
attempt=4  status=503  API_HTTP_5XX         retryable=1   backoff_ms=8640
attempt=5  status=503  API_HTTP_5XX         retryable=1   (max attempts reached)
EVENT_FAILED   event_id=4d20ecf... device_seq=1 http_status=503 latency_ms=29074 retry_count=4
```

The **same** `event_id` and `device_seq` appear in every line (KIOSK-062/
063) — never regenerated across retries. The backoff values roughly double
each time with visible jitter, matching the formula. And — critically — the
device's own `HEARTBEAT_LOCAL` diagnostic and heartbeat POSTs kept firing
on schedule throughout this ~29-second retry sequence, direct proof the
main loop was never blocked by it.

## Bug found and fixed: log line interleaving across tasks

`structured_log`'s multi-part `Serial.print()` calls (one call per JSON
field) were not synchronized across FreeRTOS tasks. When the background
send task and the main loop both logged around the same moment, their
output interleaved on the wire, corrupting a line (observed directly during
the retry test above — never affected the actual retry/protocol logic
itself, which is independently correct and confirmed via
`/debug/device-state`'s `protocol.*` fields, an in-memory struct, not parsed
log text). Fixed in the same session: `structured_log` now builds the full
JSON line in one `std::string` and makes exactly one mutex-guarded
`Serial.print()` call, so two concurrent callers can no longer interleave.
