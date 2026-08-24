# Test Plan

## Status legend

- **DONE** — covered today by `test/host/` or manual bring-up
- **DESIGNED** — behavior is specified in `docs/`, not yet testable (needs a
  later phase's code to exist first)

## Minimum scenario list

| ID | Scenario | Status |
|---|---|---|
| KIOSK-001 | Happy path | **DONE** — live, real hardware: WAIT_EMPLOYEE(v1) → scan EMP → WAIT_OPERATION(v2) → scan OP → SESSION_ACTIVE(v3) → `#` → QUANTITY_INPUT(v4) → digits+`#` → WAIT_EMPLOYEE(v5), every step confirmed via `/debug/device-state` AND a `/debug/screenshot` capture |
| KIOSK-002 | Invalid employee | **DONE** — live: unknown employee id → `EMPLOYEE_NOT_FOUND`, state stayed WAIT_EMPLOYEE/v5 (no version bump on rejection), error text rendered on-screen and captured |
| KIOSK-003 | Invalid operation | DESIGNED — backend-owned validation (`OPERATION_NOT_FOUND`/`OPERATION_CLOSED`) is host-tested indirectly via the mock backend's own transition table logic being exercised for EMPLOYEE_NOT_FOUND; not separately re-exercised live this session for the operation-code case specifically |
| KIOSK-004 | Duplicate physical scan | DESIGNED — Phase 0 scanner driver has debounce hook, needs bring-up on real hardware to confirm timing |
| KIOSK-005 | Duplicate network event | DESIGNED (needs backend idempotency store) |
| KIOSK-006 | Quantity zero vs absent | **DONE** — `test/host/test_protocol_codec.cpp` asserts `quantity_good: 0` encodes distinctly from an absent field |
| KIOSK-007 | Request timeout before server commit | DESIGNED (needs Phase 3 retry/journal) |
| KIOSK-008 | Response lost after server commit | **DONE** — live, real hardware: `POST /_test/drop-next` delayed a response 9s past the device's per-attempt timeout; device retried under the SAME `event_id`, mock backend's idempotency store returned the cached response (log line: `DUPLICATE -- returning cached response (seq not re-bumped)`), device converged to the correct state exactly once |
| KIOSK-009 | Reboot during active session | PARTIAL — reboot-restore itself is **DONE** live (see KIOSK-099); rebooting specifically MID an active session (SESSION_ACTIVE/QUANTITY_INPUT) rather than at WAIT_EMPLOYEE was not separately re-exercised this session |
| KIOSK-010 | Offline finish | DESIGNED (needs Phase 3 journal + offline policy) |
| KIOSK-011 | Replay pending journal | DESIGNED (needs Phase 3 journal) |
| KIOSK-012 | Replay duplicate | DESIGNED (needs Phase 3 journal + backend dedupe) |
| KIOSK-013 | Stale state_version | **DONE** — live: server-side version bumped out-of-band (via `/mock/admin`) while device held an older `expected_state_version`; next event → `STATE_CONFLICT`/`action:RESYNC` → device fetched `GET /state` and converged to the true version. Also see KIOSK-108 for the version-regression-refusal edge case this exposed. |
| KIOSK-014 | Human review conflict | DESIGNED (needs Phase 3 reconciliation) |
| KIOSK-015 | Backend 500 | DESIGNED — `api_client` has a typed error path, no retry policy yet (Phase 1) |
| KIOSK-016 | 429 with Retry-After | DESIGNED (Phase 1 retry policy) |
| KIOSK-017 | Storage nearly full | DESIGNED (Phase 3) |
| KIOSK-018 | Journal full | DESIGNED (Phase 3 queue pressure) |
| KIOSK-019 | Power loss mid journal append | DESIGNED (Phase 3, CRC32 helper already host-tested) |
| KIOSK-020 | Corrupt journal record | DESIGNED (Phase 3) |
| KIOSK-021 | Bad UI bundle checksum | DESIGNED (Phase 4) |
| KIOSK-022 | Bad UI bundle signature | DESIGNED (Phase 4) |
| KIOSK-023 | Unsupported schema | DESIGNED (Phase 4/5 compatibility matrix) |
| KIOSK-024 | Boot loop -> safe mode | DESIGNED (Phase 3: >=3 abnormal boots in 5 min) |
| KIOSK-025 | Scanner UART failure | PARTIAL — `hardware_selftest` reports scanner init failure; no fault-injection test yet |
| KIOSK-026 | Network loss during session | DESIGNED (Phase 2/3) |
| KIOSK-027 | Certificate expired | DESIGNED (Phase 1) |
| KIOSK-028 | Device revoked | DESIGNED (Phase 5) |
| KIOSK-029 | Command expired | DESIGNED (Phase 5) |
| KIOSK-030 | OTA rollback | DESIGNED (Phase 6) |
| KIOSK-031 | Long Vietnamese employee name | DESIGNED (needs Phase 4 bundle renderer) |
| KIOSK-032 | Missing Vietnamese font glyph | DESIGNED (needs Phase 4 bundle compiler) |
| KIOSK-033 | Label overflow | DESIGNED (needs Phase 4 bundle compiler) |
| KIOSK-034 | Invalid component coordinates | DESIGNED (needs Phase 4 bundle compiler) |
| KIOSK-035 | Unsupported font ID | DESIGNED (needs Phase 4 bundle compiler) |
| KIOSK-036 | UI exceeds component budget | DESIGNED (needs Phase 4 bundle compiler) |
| KIOSK-037 | Hold `*` 9 seconds -> no setup | PARTIAL — `WifiRecoveryController` logic implemented and builds/flashes; not yet exercised with a real held key press (needs a person at the device) |
| KIOSK-038 | Hold `*` 10 seconds -> Wi-Fi setup | PARTIAL — same as above |
| KIOSK-039 | Release `*` before threshold | PARTIAL — same as above |
| KIOSK-040 | Wi-Fi wrong password | PARTIAL — `WifiSetupPortal`'s test-before-commit path implemented (`TESTING` -> `FAILED` after 15s), not yet exercised live |
| KIOSK-041 | New Wi-Fi fails -> old credential retained | PARTIAL — implemented (`ConfigStore` is never written until `WL_CONNECTED`), not yet exercised live |
| KIOSK-042 | Wi-Fi setup timeout | PARTIAL — 10-minute inactivity teardown implemented, not yet exercised live (would require an actual 10-minute wait) |
| KIOSK-043 | Backend unavailable but Wi-Fi setup works | PARTIAL — `WifiSetupPortal` has no backend dependency by construction; not yet exercised live |
| KIOSK-044 | Bad UI bundle but Wi-Fi setup works | DESIGNED — trivially true today since no bundle renderer exists yet to be "bad"; revisit once Phase 4 exists |
| KIOSK-045 | Safe mode can access Wi-Fi setup | DESIGNED (Phase 3: `SAFE_MODE` doesn't exist yet) |
| KIOSK-046 | Screenshot endpoint returns current frame | **DONE** — verified live via `capture_screen.py` |
| KIOSK-047 | Screenshot orientation matches TFT | **DONE** — 240x320 portrait, matches `docs/HARDWARE.md` |
| KIOSK-048 | Screenshot while UI changing is consistent | **DONE** — `frame_id` retry loop in `capture_screen.py`, exercised live during the Wi-Fi hold transition |
| KIOSK-049 | ui-state matches current screen | **DONE** — verified (`screen_id`/`lines[]` match the actual drawn screen each time) |
| KIOSK-050 | Text overflow reported | **DONE** — found a real overflow bug this way; `UI_TEXT_OVERFLOW` log + `lines[].overflow`/`measured_w` verified live |
| KIOSK-051 | Missing glyph reported | NOT IMPLEMENTED — Phase 0 has no Vietnamese glyph support at all (stock ASCII-only font), so there's no glyph-fallback path to report on yet. Phase 4. |
| KIOSK-052 | Debug input SCAN uses EventBus | **DONE** — verified live (injected SCAN produced the same scan_result screen a real scan would) |
| KIOSK-053 | Debug key input uses same runtime path | **DONE** — verified live (injected KEY_DOWN '*' triggered the real `WifiRecoveryController` hold-timer and portal, not a shortcut) |
| KIOSK-054 | Debug endpoint disabled in production profile | NOT IMPLEMENTED — `MESFLOW_DEBUG_API` is one compile flag, currently always on; no real production profile split exists yet |
| KIOSK-055 | Wi-Fi recovery screen screenshot works | **DONE** — countdown and portal-active screens both captured and inspected |
| KIOSK-056 | Safe-mode screenshot works | DESIGNED — `SAFE_MODE` itself doesn't exist yet (Phase 3) |
| KIOSK-057 | Capture bundle metadata matches firmware/boot | **DONE** — `manifest.json` includes firmware_version/build_id/boot_id/screen_id/frame_id, verified against live device-state |
| KIOSK-058 | Screenshot rate limiting | PARTIAL — implemented (429 + `DEBUG_RATE_LIMITED`), not exercised with an actual rapid-fire test this session |
| KIOSK-059 | No credentials exposed in debug state | **DONE** — `/debug/device-state` reviewed: SSID/RSSI/IP only, no password field exists anywhere in the response |
| KIOSK-060 | Framebuffer memory remains within resource budget | **DONE** — measured before/after: ~156KB PSRAM used (matches the 153,600-byte framebuffer almost exactly), <2% of ~8MB total, well within the 25% headroom target |
| KIOSK-061 | event_id unique | **DONE** — `generate_random_hex_id` is 16 random bytes (128 bits); every live test this session showed a distinct event_id per scan |
| KIOSK-062 | Retry preserves event_id | **DONE** — live: 5 attempts against a 503 endpoint, same `event_id` in every `EVENT_SEND_ATTEMPT`/final `EVENT_FAILED` log line |
| KIOSK-063 | Retry preserves device_seq | **DONE** — live, same test as above, same `device_seq` throughout |
| KIOSK-064 | device_seq survives reboot | **DONE** — live: climbed `0 → 1 → 1000 → 2000 → 3001 → 4001` across 5+ real reboots, never resetting. Also caught and fixed a real bug here (NVS access from a global object's constructor, before `nvs_flash_init()` — see docs/PROTOCOL.md) |
| KIOSK-065 | boot_id changes after reboot | **DONE** — live: a different boot_id observed on every one of ~10 reboots this session |
| KIOSK-066 | Protocol unsupported version rejected | PARTIAL — `BootstrapClient` checks `accepted_version` against `{0,1}` and would report REJECTED; not live-tested against an actual backend returning a different version (mock_backend.py always sends 1) |
| KIOSK-067 | Backend not configured — explicit error | PARTIAL — `CONFIG_BACKEND_NOT_SET` implemented (`ConfigStore::api_endpoint()` returns `""`, `kiosk_runtime.cpp` checks for it before ever attempting a send); not re-verified live this session with a freshly-unset endpoint (device already had one configured from earlier testing) |
| KIOSK-068 | NTP sync success | **DONE** — live, `TIME_SYNC_OK` observed (docs/TIME_SYNC.md) |
| KIOSK-069 | NTP unavailable does not block runtime | **DONE** — live: device fully responsive (scans, debug API, heartbeat) throughout a ~105s NTP retry window |
| KIOSK-070 | Device event with UNSYNCED time valid | **DONE** — host test (`test_protocol_codec.cpp`) |
| KIOSK-071 | Bootstrap accepted | **DONE** — Phase 2, live: real `accepted:true` observed on-device (`BOOTSTRAP_OK` + `STATE_APPLY source=BOOTSTRAP apply=APPLIED state=WAIT_EMPLOYEE version=1`) against the project's own stateful mock backend, reached via SSH tunnel. Found and fixed a real bug getting here: `BootstrapClient` was POSTing to the `/events` URL instead of deriving `/bootstrap` (see docs/PROTOCOL.md). |
| KIOSK-072 | Bootstrap rejected | **DONE** — live, `accepted:false` correctly detected against `httpbin.org` every boot |
| KIOSK-073 | Retry timeout | **DONE** — live, `NET_CONNECT_REFUSED`/timeout-class retries observed with correct backoff |
| KIOSK-074 | Retry 503 | **DONE** — live (docs/RETRY_POLICY.md's full trace) |
| KIOSK-075 | 400 — no blind retry | **DONE** — live: `httpbin.org/status/400` → exactly 1 attempt, `retry_count: 0`, `error_code: API_HTTP_4XX` |
| KIOSK-076 | 429 obeys Retry-After | PARTIAL — live-verified that 429 is retried up to max attempts with normal backoff (`httpbin.org/status/429`, which doesn't send a `Retry-After` header); the "honor an actual Retry-After value over normal backoff" path is host-test-verified only (`test_retry_policy.cpp`), not live |
| KIOSK-077 | DEV: debug input enabled | **DONE** — used throughout this session (`/debug/input` SCAN/KEY_DOWN/KEY_UP) |
| KIOSK-078 | PROD: debug input disabled | **DONE** — build-time verified: 0 occurrences of `/debug/*` route strings in the PROD `.elf` vs 4 in DEV (`strings` on both binaries) |
| KIOSK-079 | Identity: UNPROVISIONED state | **DONE** — live: fresh flash showed `device_id:""`, `provisioning_state:"UNPROVISIONED"`, identity screen rendered (captured & inspected) |
| KIOSK-080 | Identity: REVOKED state | PARTIAL — `revoke` command + `IDENTITY_REVOKED` rejection code implemented and code-reviewed; not re-exercised live this exact session (SUSPENDED/REVOKED share the same tested code path as UNPROVISIONED's rejection branch, `identity_error_code()`) |
| KIOSK-081 | Client cert missing error | DESIGNED ONLY — no CertificateStore exists yet (docs/SECURITY.md) |
| KIOSK-082 | Backend URL invalid rejected | **DONE** — host test (`test_backend_url_validation.cpp`, 13 cases) |
| KIOSK-083 | Heartbeat best-effort only | **DONE** — live: heartbeat failures (`status=-1`/`404`/`400`) never retried, never affected scan handling, logged and moved on every cycle |
| KIOSK-084 | Physical scan creates protocol event | PARTIAL — the full pipeline (EventBus → envelope → send → retry → ack → StateProjection → render) is proven via `/debug/input`-injected SCANs (same EventBus path real hardware uses, §12/§13); a scan from the actual physical GM65 scanner reading a real barcode was NOT re-exercised this session (no physical barcode/QR available in this environment) — carried forward as a real gap, see KIOSK-107. |
| KIOSK-085 | Visual debug regression still PASS | **DONE** — live, waiting/identity screens captured and inspected after all Phase 1 changes |
| KIOSK-086 | Wi-Fi recovery regression still PASS | **DONE** — live, full hold→AP→cancel→self-recovery cycle captured and inspected after all Phase 1 changes |

## Phase 2 — Server Authoritative State + Workflow Contract (KIOSK-087–110)

| ID | Scenario | Status |
|---|---|---|
| KIOSK-087 | `json_extract` whitespace-tolerant field extraction | **DONE** — host test (`test_json_extract.cpp`), including the exact bug this fixes: Python `json.dumps()`'s `"key": value` (space after colon) |
| KIOSK-088 | `StateProjection::apply()` version/consistency rules | **DONE** — host test (`test_state_projection.cpp`): first-applies, newer-applies, stale-rejected, identical-harmless, inconsistent-rejected, UNSUPPORTED-never-applied-even-as-first-or-newer |
| KIOSK-089 | `/events` SUCCESS response parses | **DONE** — host test (`test_event_response.cpp`) + live (every accepted scan/keypress this session) |
| KIOSK-090 | `/events` BUSINESS_REJECTED response parses, snapshot unchanged | **DONE** — host test + live (KIOSK-002) |
| KIOSK-091 | `/events` CONFLICT_RESYNC response parses, no snapshot trusted from it | **DONE** — host test + live (KIOSK-013) |
| KIOSK-092 | `produced_qty`/`target_qty` = 0 distinguishable from absent | **DONE** — host test (`test_state_projection.cpp`); live session used `target_qty:100` (mock's fixed value), the 0-vs-absent distinction itself is host-test-verified, not separately live-demonstrated with a real 0 |
| KIOSK-093 | Malformed `/events` response never silently applied | **DONE** — host test: `accepted:true` with no `state{}`, and a no-snapshot error body, both correctly resolve to `MALFORMED` |
| KIOSK-094 | Bootstrap response seeds StateProjection | **DONE** — live: `STATE_APPLY source=BOOTSTRAP apply=APPLIED` on every boot this session |
| KIOSK-095 | Bootstrap-to-`/bootstrap`-not-`/events` endpoint derivation | **DONE** — real bug found and fixed live this phase (see docs/PROTOCOL.md) |
| KIOSK-096 | GET /state client (`AsyncStateFetcher`) | **DONE** — live, exercised every RESYNC this session |
| KIOSK-097 | Device never decides a business transition locally | **DONE** — code audit: `handle_business_key`/`handle_scan` only ever pick an `EventType` to send, never a `BusinessState` to become; every transition observed live came from a server response |
| KIOSK-098 | All 6 canonical business states render correctly | **DONE** — live screenshots of WAIT_EMPLOYEE, WAIT_OPERATION, SESSION_ACTIVE, QUANTITY_INPUT (including live digit buffer), DEVICE_DISABLED, MAINTENANCE — see Phase 2 report |
| KIOSK-099 | Reboot never restores a locally-remembered business state | **DONE** — live: rebooted mid-session; device re-bootstrapped and rendered the SERVER's actual current state, not a resumed local one. Also exposed a real edge case: a DEV-only `/mock/admin reset` that rewinds `state_version` backward leaves the device correctly stuck in RESYNCING (invariant 16) until its next reboot. |
| KIOSK-100 | `#` triggers FINISH_REQUESTED / QUANTITY_SUBMITTED, digits are local-only | **DONE** — live: digit buffer accumulated "98" locally (visible on-screen, not sent), `#` submitted `quantity_good:98` explicitly |
| KIOSK-101 | STATE_CONFLICT → RESYNC → reconciliation | **DONE** — live (KIOSK-013's evidence) |
| KIOSK-102 | RESYNC itself returns a stale/inconsistent snapshot | **DONE** (as an unplanned but real finding) — live: see KIOSK-099's reset-rewind case; device stays honestly stuck rather than fabricating progress |
| KIOSK-103 | DEVICE_DISABLED transition + screen | **DONE** — live, via `/mock/admin {"disabled":true}` + RESYNC convergence, screenshot captured |
| KIOSK-104 | MAINTENANCE transition + screen | **DONE** — live, same path as KIOSK-103 |
| KIOSK-105 | UNSUPPORTED business state never applied | **DONE** host-test only — `test_state_projection.cpp` covers an unrecognized state name; not live-demonstrated (would require the mock backend to intentionally send a bogus state name, which its DEV admin surface doesn't expose) |
| KIOSK-106 | `/debug/device-state` state{} block (§44-46) | **DONE** — live: `business`/`state_version`/`workflow_version`/`resyncing`/`last_server_seq`/`source`/`view`/`local_quantity_buffer` all confirmed correct at every step of KIOSK-001 |
| KIOSK-107 | Physical GM65 barcode scan drives a real transition | NOT DONE — no physical barcode/QR available in this environment; `/debug/input`-injected SCAN (same EventBus path, §12/§13) fully substitutes for the application-layer proof, but the scanner-hardware leg itself was not re-exercised this phase (Phase 0 proved scanner→EventBus separately) |
| KIOSK-108 | `state_version` regression refused, not silently accepted | **DONE** (as a real live finding, not just designed) — see KIOSK-099 |
| KIOSK-109 | Idempotency key reuse with a DIFFERENT payload → `IDEMPOTENCY_KEY_REUSE_MISMATCH`/409 | DESIGNED — mock backend implements this (`payload_hash` mismatch check) and it is exercised implicitly by the retry path always resending an IDENTICAL payload (matching hash); a genuinely different payload under the same event_id was not separately live-tested this session |
| KIOSK-110 | PROD build regression after Phase 2 | **DONE** — `scripts/build-prod.sh` succeeds; `strings` on the PROD `.elf` still shows 0 `/debug/*` occurrences vs 6 in DEV |

## Phase 0 automated coverage (`test/host/`, run via `scripts/run_host_tests.sh`)

- Event envelope encodes required fields (`device_id`, `boot_id`,
  `device_seq`, `event_id`, `type`, `payload`, `context.*`).
- `quantity_good = 0` is distinguishable from `quantity_good` absent
  (KIOSK-006).
- CRC32 helper round-trips known test vectors (groundwork for KIOSK-019/020).

These run with plain `g++`, no ESP toolchain, so they run in CI without
hardware.

## Phase 2 automated coverage (`test/host/`, run via `scripts/run_host_tests.sh`)

- `test_json_extract.cpp` — whitespace-tolerant field/object extraction,
  nested-object scoping, null vs absent, escapes, malformed input.
- `test_state_projection.cpp` — `StateProjection::apply()`'s full version/
  consistency rule set, `parse_state_snapshot_json` round-trip.
- `test_event_response.cpp` — all three `/events` response shapes
  (SUCCESS/BUSINESS_REJECTED/CONFLICT_RESYNC) plus the MALFORMED fallback,
  built against the mock backend's actual response shapes.

All 7 host test binaries pass (`scripts/run_host_tests.sh` exit 0) as of
this phase's final commit — but per project instructions, none of this
substitutes for the live hardware evidence in the KIOSK-087–110 table
above; host tests only prove the portable parsing/logic is internally
consistent, not that the real device/network/backend triangle behaves.

## Phase 0 manual/hardware coverage

- Boot diagnostics print chip/flash/PSRAM/heap/reset-reason/version.
- Display initializes and renders a static screen.
- Scanner reads a line over UART and produces one local `SCAN` event.
- Keypad reports a raw PCF8574 bit-change.
- One event successfully POSTed to `tools/mock_backend` and logged there.

Recorded per-run in `README.md`'s "Hardware test" section rather than a
separate log, since Phase 0 only has one milestone to check off.
