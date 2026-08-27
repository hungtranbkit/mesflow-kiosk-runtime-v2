# Reliability Contract — MESFlow ESP Kiosk

Added 2026-08-27, "Final Reliability Standardization" pass. This is the
permanent durability/reliability contract for the kiosk-as-durable-event-
terminal design (see `docs/ARCHITECTURE.md`'s invariants 1-16 for the
broader architecture; this doc is the narrower reliability slice that pass
asked to be made explicit and testable).

Core principle the whole contract serves:

> Every business-changing user action → persisted locally first →
> asynchronously delivered → retried safely → applied exactly once at
> business level → never lost because of WiFi/server instability.

## R1 — User action is never acknowledged locally before durable persistence

`KioskRuntime::send_business_event()` calls `journal_.append_event()`
unconditionally, before any network attempt, for every business-changing
event type (SCAN/FINISH_REQUESTED/QUANTITY_SUBMITTED/CANCEL_REQUESTED).

Fixed 2026-08-27 (this pass): `append_event()`'s return value used to be
discarded — a FULL journal proceeded exactly as if the record were
durable, and could show "ĐÃ LƯU" for an action that was never actually
persisted. Now checked: a failed append aborts immediately with an honest
"BỘ NHỚ CHỜ GỬI ĐÃ ĐẦY - CHƯA được lưu" message, no network attempt is
made, and no false success is ever shown. See `send_business_event()`'s
own comment for the exact incident this closes.

**Status: PASS** (structural, verified by code inspection this pass; the
FULL-journal path itself is exercised by `decide_append()`'s host tests in
`test/host/test_event_journal.cpp`, not by a live full-journal drill on
real hardware — see the Final Report's honest gap list).

## R2 — Network failure never deletes a durable event

`EventJournal`'s compaction (`select_records_to_keep()`) never drops
PENDING/IN_FLIGHT records regardless of pressure; only terminal statuses
(ACKED/REJECTED/CONFLICT/HUMAN_REVIEW) are ever bounded, and only down to
a retained-count floor per status, never to zero for the most recent ones.

**Status: PASS** (host-tested, `test/host/test_event_journal.cpp`; live
crash-boundary evidence: `EventJournal::simulate_compaction_crash_for_test()`
covers 4 real interruption windows during compaction specifically).

## R3 — Retry never changes event_id

`event_id` is generated exactly once, in `send_business_event()`
(`generate_random_hex_id(16)`), before the journal append. Every retry —
foreground (`NetworkWorker::execute()`'s own attempt loop) and background
(offline replay, `pending_in_device_seq_order()`) — reuses the journaled
record's original `event_id`/payload unchanged; nothing on any retry path
regenerates it.

**Status: PASS** (structural; the whole idempotency contract in R4 depends
on this and was verified empirically, see R4).

## R4 — Duplicate delivery never creates duplicate business effect

Backend contract (`app/mesflow/web/kiosk_v2.py`): idempotency key is
`(device_id, event_id)`. Same key + same payload → the original stored
response is replayed verbatim, no second business effect. Same key +
different payload → `409 IDEMPOTENCY_KEY_REUSE_MISMATCH`, no second
effect either. A second, independent layer exists inside
`WorkSessionRepository.start()`/`finish()` (`kiosk_idempotency` table,
keyed by `request_id = device_id:event_id`), plus a third at the DB level
(`work_sessions`' `uq_open_session_per_employee` unique constraint).

**Status: PASS — verified empirically at scale.** 100x literal replay of
one fixed `(device_id, event_id, payload)` for both START and FINISH
(qty=17): 100/100 HTTP delivered, exactly 1 business effect each time (1
`work_sessions` row; final `good_qty=17`, not 1700), exactly 1 stored row
in `kiosk_v2_events` per event_id. A same-event_id payload-mismatch replay
correctly got `409` with no second effect. See the 2026-08-27 "Close Final
Two Runtime Gaps" report for the full evidence.

Fixed this pass (a real gap the 100x test didn't itself exercise, since it
only ever produced HTTP 200s): a non-retryable transport-level failure
(401/403/409/generic 4xx) used to be journaled PENDING and retried forever
by every future offline-replay cycle — this couldn't create a *duplicate
business effect* (the backend's own idempotency already prevented that),
but it violated R9 below (a permanently-doomed event could never reach a
terminal state) and wasted retry effort on something that could never
succeed. Now: any `outcome.retryable == false` result marks the record
`HUMAN_REVIEW` (terminal, never replayed again) instead of `PENDING`.

## R5 — Reboot resumes PENDING events

`EventJournal::init()` recovers the on-disk index at boot
(`apply_scanned_frame()` replays every valid EVENT/TRANSITION frame in
file order); `start_offline_replay_if_needed()` picks up every remaining
PENDING/IN_FLIGHT record via `pending_in_device_seq_order()` on the next
reconnect.

**Status: PASS** (verified live this session: journal ACK-persistence
regression test across a real reboot; `apply_event_response()`'s ACKED/
REJECTED decision is taken from the server's response kind alone,
independent of whether the accompanying snapshot happens to apply locally
— the earlier "stuck PENDING forever" bug this fixed).

## R6 — Server/network failure does not reboot kiosk

Full audit of every `ESP.restart()`/`request_controlled_reboot()` call
site (2026-08-27 "Final Runtime Closure" pass): none are reachable from a
DNS/TCP/HTTP/timeout/outage failure. Confirmed live across this session's
entire physical WiFi-drop/backend-outage test campaign (20+ drop cycles,
multiple outage windows): zero unexpected reboots (same `boot_id`
throughout each run).

**Status: PASS.**

## R7 — Scanner/UI remain responsive while network is unavailable

All network I/O — business events, offline replay, bootstrap, heartbeat,
state resync, UI-bundle download — flows through the single persistent
`NetworkWorker` task (`src/network/network_worker.cpp`), never
synchronously on the main `loop()` thread. The last two remaining
exceptions were closed this session: `BootstrapClient` (was a direct
blocking `HTTPClient` POST — root cause of a real "esp rất chậm, bấm số
phải bấm nhiều lần" field report) and `AsyncStateFetcher`/UI-bundle
download (was a per-call `xTaskCreate`). Firmware-wide grep confirms
exactly one live `xTaskCreate()` call remains (`network_worker.cpp`'s own
`begin()`) and one live `HTTPClient`/`WiFiClient` owner
(`network_worker.cpp`) plus one explicitly-marked DEV-only diagnostic
exception (`net_diag.cpp`'s `debug-net-diag` command).

**Status: PASS** (verified live: bootstrap-under-reconnect timing dropped
to 168ms/3ms from up to ~27.5s of loop-blocking; 100 real op cycles with 0
task-count drift; UI-bundle v9→v10 download/activate in 1.2s with the
scanner never blocked).

## R8 — PENDING events are never silently evicted

Compaction's `select_records_to_keep()` always keeps 100% of PENDING/
IN_FLIGHT regardless of journal pressure — only terminal statuses are ever
bounded. The hard record-count cap (`kMaxInMemoryJournalRecords`) triggers
compaction *sooner*, it does not change what compaction is allowed to
drop.

**Status: PASS** (host-tested).

## R9 — Terminal events may only be compacted after durable terminal state

A record only becomes eligible for compaction once its `sync_status` is
one of ACKED/REJECTED/CONFLICT/HUMAN_REVIEW, and that status is itself
only ever written via a durable `append_transition()` call — never
inferred/assumed in RAM only. Fixed this pass: the 401/403/409-class
permanent-failure gap above meant an event could never *reach* a terminal
status at all (stuck PENDING forever) — now closed via the HUMAN_REVIEW
transition described in R4.

**Status: PASS** (post-fix; the underlying compaction-eligibility rule
was already correct and host-tested — the gap was upstream, in what got
classified PENDING vs terminal in the first place).

## R10 — Every created durable event is always accountably PENDING, ACKED, REJECTED, or HUMAN_REVIEW

`JournalSyncStatus` has exactly these plus `IN_FLIGHT`/`CONFLICT` (both
transient waypoints toward one of the four terminal-or-pending buckets,
never a fifth permanent bucket of their own) — no code path constructs or
reads any other status string (`journal_sync_status_from_string()` returns
false, not a guessed default, for anything else).

**Status: PASS by construction** (the type system doesn't allow a fifth
value). **Not yet verified at the 1000-unique-event accounting scale this
pass asks for** — see the Final Report's Data Conservation section for
what was and wasn't actually run.

---

Every invariant above must stay true across any future kiosk firmware
change. A change that would violate one of these needs an explicit,
documented decision to change the CONTRACT here, not a silent regression.
