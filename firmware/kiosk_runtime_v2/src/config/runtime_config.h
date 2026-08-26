#pragma once

#include "build_info.h"  // MESFLOW_PROFILE_DEV / MESFLOW_PROFILE_PROD

// Compile-time knobs. These are *targets to benchmark against* (see
// project spec §5), not yet enforced as hard policy anywhere in Phase 0
// code — hardware_selftest and boot_diagnostics report actuals; nothing
// aborts boot if a target is missed.

// PSRAM headroom target after normal runtime init.
#define RUNTIME_TARGET_PSRAM_FREE_PCT_MIN 25

// Local scan -> presentation feedback target (§12).
#define RUNTIME_TARGET_LOCAL_FEEDBACK_MS 100

// (Simplicity/memory pass, 2026-08-26: RUNTIME_TARGET_SERVER_UX_MS/
// _WARN_MS/_DEGRADED_MS used to live here -- another set of dead,
// misleading configuration, same class as RUNTIME_HTTP_HARD_DEADLINE_MS
// below: defined, described as "targets to benchmark against", never
// actually referenced by any code anywhere in this firmware. Removed
// rather than kept around unenforced -- "do not retain misleading
// configuration.")

// Scan-latency investigation (2026-08-26): lowered from 5000. Real evidence
// from the physical test board on its normal shop-floor Wi-Fi ("Airport"):
// the backend itself is consistently fast (confirmed via kiosk_v2.py's own
// _TIMING_ENABLED instrumentation -- ~70-150ms total_backend_ms per event,
// even including the idempotency lookup + employee/session lookups + a
// commit), and every genuinely SUCCESSFUL single-attempt round trip observed
// live landed well under 1.5s (267-1478ms across a real multi-scan session).
// A stalled attempt, though, was riding the full old 5000ms timeout before
// falling through to a retry -- two such attempts back to back (a real,
// observed retry_count=1 case measured 6438-6790ms for ONE retry alone) is
// exactly the >10s class of symptom reported. 2500ms keeps ~1.7x headroom
// over the worst observed SUCCESSFUL attempt (1478ms) -- generous enough not
// to punish a merely-slow-but-working request -- while roughly halving the
// worst-case single-attempt stall a genuinely bad request pays before
// retrying. Retry/backoff policy itself (RETRY_BACKOFF_BASE_MS etc. below)
// is unchanged -- no live evidence it needs to change, only the per-attempt
// ceiling did.
#define RUNTIME_HTTP_TIMEOUT_MS 2500
// (Simplicity/memory pass, 2026-08-26: RUNTIME_HTTP_HARD_DEADLINE_MS used to
// live here, flagged in the previous round's field report as dead code --
// defined, described in its own comment as "a separate FreeRTOS task +
// caller-side deadline" enforced by api_client.cpp, but grepping the actual
// source showed no such enforcement existed anywhere; only HTTPClient's own
// setTimeout()/setConnectTimeout() (which IS wired up and confirmed live to
// actually fire around RUNTIME_HTTP_TIMEOUT_MS) ever bounded a single
// attempt. Removed rather than implemented: this task's own explicit
// instruction is "if it is dead/unimplemented: enforce it correctly, or
// remove it" -- a real caller-side hard backstop is a legitimate future
// improvement, but building new complexity is the opposite of this pass's
// goal, and no live evidence has ever shown HTTPClient's own timeout
// failing to fire on this board.)

// Response-size guard (simplicity/memory pass, 2026-08-26): a real,
// previously-unbounded gap found auditing every HTTP call site in this
// firmware -- api_client.cpp/state_client.cpp/bootstrap_client.cpp all
// called http.getString() unconditionally, with no check on the response
// size at all, before this. Every real response this protocol ever sends
// is small (a live /events response is a few hundred bytes; a live UI
// bundle download -- the single largest legitimate payload this firmware
// ever fetches, sharing state_client.cpp's same code path -- measured
// 1772 bytes against the real backend). One shared cap, generous enough
// to leave ~9x headroom over that real bundle size, is simpler than a
// separate constant per endpoint and still small enough to protect the
// ~200KB+ internal-SRAM budget from a single pathological response (a
// server bug, misconfiguration, or compromised backend) ever trying to
// allocate an unbounded String. A response whose Content-Length is
// unknown (chunked, or the header missing) is treated the SAME as
// oversized -- rejected before ever calling getString() -- since this
// protocol's own real responses always carry a real Content-Length
// (plain Flask JSON, never streamed).
#define RUNTIME_MAX_RESPONSE_BODY_BYTES 16384

// Scanner physical-duplicate suppression window. Not a business dedupe
// window — just "the same physical swipe read twice" (§26).
#define SCANNER_DUPLICATE_SUPPRESS_MS 1500

// Wi-Fi connect attempt timeout before wifi_manager reports DISCONNECTED
// and backs off.
#define WIFI_CONNECT_TIMEOUT_MS 15000

// Universal escape gesture (2026-08-25 finish-anti-stuck-recovery
// follow-up, §5/§7): hold '*' ~5s -> local recovery menu; keep holding to
// ~10s -> Wi-Fi setup portal (unchanged threshold/behavior from before).
// Works from every screen (WifiRecoveryController is independent of
// whatever KioskRuntime is currently rendering) -- never mutates business
// state on its own.
#define RECOVERY_MENU_HOLD_MS 5000
// Local Wi-Fi recovery: hold '*' this long to enter the AP+portal recovery
// flow (docs/WIFI_RECOVERY.md, §16 of the task spec).
#define WIFI_RECOVERY_HOLD_MS 10000
// How long the portal waits for a candidate network to reach WL_CONNECTED
// before declaring it failed and rolling back to the previous credentials.
#define WIFI_RECOVERY_TEST_TIMEOUT_MS 15000
// Portal auto-exits after this long with no HTTP request (§23: recovery
// mode must not stay open forever). 12 minutes -- inside the 2026-08-24
// open-AP rework's requested "approximately 10-15 minutes" window, with a
// little more real-world margin than the previous flat 10 minutes now that
// the AP itself is open (an operator fumbling with their phone's Wi-Fi
// settings shouldn't get timed out mid-setup).
#define WIFI_RECOVERY_PORTAL_TIMEOUT_MS 720000

// Remote Visual Debug subsystem (docs/VISUAL_DEBUG.md). DEV profile only
// (§14/§30/§70) -- derived from the build profile now, not a standalone
// flag someone could forget to flip. A PROD build compiles the entire
// debug_server module out (see debug_server.h's #if), not just disables
// individual routes at runtime.
#if MESFLOW_PROFILE_DEV
#define MESFLOW_DEBUG_API 1
#else
#define MESFLOW_DEBUG_API 0
#endif
// Separate port from the Wi-Fi recovery portal's port 80, so the debug API
// can keep working even while the recovery portal is active (§17) --
// they're two independent WebServer instances, never sharing one port.
#define MESFLOW_DEBUG_API_PORT 8081
// Minimum interval between accepted /debug/screenshot requests (§26: don't
// let capture spam slow the kiosk down).
#define MESFLOW_DEBUG_SCREENSHOT_MIN_INTERVAL_MS 500

// §3 (Phase 1): there is deliberately NO default backend URL constant here
// anymore. Phase 0 had a placeholder-that-looked-valid
// ("http://192.168.1.50:8799/...") baked in, which is exactly the pattern
// this section forbids -- it silently "worked" (compiled, looked like a
// real config) while being wrong for anyone not on that exact subnet, and
// masked the real state: no backend was ever actually configured.
// ConfigStore::api_endpoint() now returns "" until explicitly set via
// `api-endpoint:<url>` (see kiosk_runtime_v2.ino), and callers must treat
// "" as CONFIG_BACKEND_NOT_SET, not attempt a request against it.

// Backend URL scheme policy (§41, revised 2026-08-24 -- see
// docs/KIOSK_V2_PLAIN_HTTP.md). Kiosk v2 business/event data is not
// confidential (explicit product decision); the canonical transport is now
// plain HTTP for BOTH profiles, not a DEV-only bring-up convenience --
// mbedTLS's internal-SRAM requirement was found to sit right at this
// chip's real contiguous-memory ceiling (see the SRAM fragmentation
// investigation), making HTTPS an active reliability liability for a
// non-confidential payload. Still validated either way (never accepts a
// malformed URL); this only decides which SCHEME is accepted, not whether
// validation happens at all.
#define BACKEND_URL_ALLOW_HTTP 1
#define BACKEND_URL_MAX_LENGTH 160

// --- Retry policy (docs/RETRY_POLICY.md, §22) ---
#define RETRY_BACKOFF_BASE_MS 1000
#define RETRY_BACKOFF_MAX_MS 30000
#define RETRY_JITTER_PCT_MAX 25  // 0-25% of the backoff, added on top

// --- Time sync (docs/TIME_SYNC.md, §15) ---
#define TIME_SYNC_NTP_SERVER "pool.ntp.org"
// A sync older than this is reported STALE rather than SYNCED, even though
// technically "once synced" -- device clocks drift, and staleness is worth
// surfacing separately from never-synced.
#define TIME_SYNC_STALE_AFTER_S (24u * 3600u)
// How often to retry NTP if it hasn't succeeded yet.
#define TIME_SYNC_RETRY_INTERVAL_MS 60000

// --- Heartbeat (§43/§44) ---
#define HEARTBEAT_INTERVAL_MS 20000

// --- Task watchdog (2026-08-26 ESP kiosk UX-hardening pass, §21) ---
// Before this, no esp_task_wdt_* call existed anywhere in this firmware --
// RecoveryCode::WATCHDOG was a declared-but-never-constructed enum value,
// and the only stall protection was a 5-minute SOFTWARE UI-stall self-check
// (kiosk_runtime_v2.ino) that can only ever catch a loop() that is STILL
// RUNNING but not rendering, never a genuinely hung loop() (e.g. a display
// SPI transaction blocking forever). This is a REAL hardware backstop for
// that gap. 15s is comfortably above every legitimate slow path measured on
// real hardware (the worst known: EventJournal::compact()'s one-shot,
// non-incremental rewrite path, ~4.2s for 214 records -- only taken on the
// rare CRITICAL low-memory escalation, see event_journal.h) while still
// being short enough that an operator isn't left staring at a truly frozen
// screen for long before the chip resets itself back to boot -> READY.
#define TASK_WATCHDOG_TIMEOUT_S 15
// Sequence reservation block size for persistent device_seq (§8, docs/PROTOCOL.md).
#define DEVICE_SEQ_RESERVE_BLOCK 1000
