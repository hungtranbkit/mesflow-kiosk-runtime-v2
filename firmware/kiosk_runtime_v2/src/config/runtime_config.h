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

// Server interaction UX targets.
#define RUNTIME_TARGET_SERVER_UX_MS 300
#define RUNTIME_TARGET_SERVER_UX_WARN_MS 800
#define RUNTIME_TARGET_SERVER_UX_DEGRADED_MS 2000
#define RUNTIME_HTTP_TIMEOUT_MS 5000
// Hard backstop enforced by api_client itself (a separate FreeRTOS task +
// caller-side deadline), NOT just HTTPClient's own timeout knobs -- those
// were observed on real hardware to not reliably bound a connect attempt to
// an unreachable IP (the call can block far longer than configured, with no
// watchdog rescue since a properly-yielding blocked task doesn't starve the
// idle task). Deliberately a bit above RUNTIME_HTTP_TIMEOUT_MS: the
// library's own timeout should normally fire first; this is the guarantee
// that the UI is never stuck longer than this no matter what.
#define RUNTIME_HTTP_HARD_DEADLINE_MS 8000

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
