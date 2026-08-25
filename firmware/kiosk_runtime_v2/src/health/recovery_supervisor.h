#pragma once

#include <Arduino.h>

#include <cstdint>

namespace kiosk::health {

// Structured recovery codes (2026-08-24, self-recovery task, §22) -- every
// automatic recovery action this runtime takes logs one of these, so
// /debug/device-state and the heartbeat body can show WHY a reboot/recovery
// happened without scraping free-text log lines. New project invariant this
// module exists to serve: "NO RECOVERABLE ERROR MAY LEAVE THE KIOSK
// PERMANENTLY STUCK" -- every transient/error state must have a structured
// error code, not just a log line a human has to go find.
enum class RecoveryCode {
  LOW_MEMORY,
  JOURNAL_PRESSURE,
  TASK_CREATE_FAILED,
  NETWORK_TIMEOUT,
  STATE_DESYNC,
  UI_STALL,
  WATCHDOG,
  REBOOT_LOOP,
  // Added 2026-08-25 (finish-anti-stuck-recovery follow-up):
  SCANNER_REINIT,
  KEYPAD_REINIT,
};
const char* recovery_code_to_string(RecoveryCode code);

constexpr int kRecoveryHistoryCapacity = 8;

// One entry in the bounded recovery history ring (§23) -- the last
// kRecoveryHistoryCapacity events, oldest overwritten first. In-RAM only
// (does NOT survive a reboot) -- reboot-loop protection below is the piece
// that DOES persist, separately, in NVS.
struct RecoveryEvent {
  RecoveryCode code = RecoveryCode::LOW_MEMORY;
  char detail[64] = {0};
  uint32_t uptime_ms = 0;
  uint8_t journal_pressure = 0;  // kiosk::protocol::JournalPressure as int -- avoids a header dependency here
  uint32_t memory_free_bytes = 0;
  uint32_t memory_largest_block_bytes = 0;
};

// Records one recovery-relevant event into the bounded in-RAM ring and logs
// it structured -- does NOT itself reboot. Call this for every RECOVERY_*
// condition detected, whether or not it escalates to
// request_controlled_reboot() below.
void record_recovery_event(RecoveryCode code, const char* detail, uint8_t journal_pressure,
                           uint32_t memory_free_bytes, uint32_t memory_largest_block_bytes);

// Read-only access for /debug/device-state and the heartbeat body.
// 0 = oldest still held, recovery_history_count()-1 = newest.
int recovery_history_count();
const RecoveryEvent& recovery_history_at(int index);

// --- Reboot-loop protection (§13) ---
// Call once from setup(), as early as anything that touches NVS safely can
// run -- reads the PERSISTED (Preferences/NVS) same-fault streak from any
// PRIOR reboot(s) request_controlled_reboot() wrote, so a streak survives
// across the actual reboot boundary (an in-RAM counter alone would reset to
// 0 every boot, defeating the whole point of loop detection).
void recovery_supervisor_init();

// Call once uptime has been "stable" for a while this boot (no recovery
// reboot needed) -- clears the persisted same-fault streak so sparse,
// well-separated incidents months apart don't eventually accumulate into a
// false SAFE_MODE trip. A genuine reboot LOOP (each boot crashing well
// before this point) never reaches this call, so the streak keeps growing
// for that case as intended.
void recovery_supervisor_mark_stable();

// True once request_controlled_reboot() has seen the SAME RecoveryCode
// trigger a reboot kSafeModeThreshold times in a row with no intervening
// recovery_supervisor_mark_stable(). Minimal SAFE_MODE (§14): this flag is
// exposed in /debug/device-state and logged loudly, but does NOT yet change
// the actual boot path (no stripped-down init) -- see the self-recovery
// report's Known Gaps for why the fuller behavior is deferred.
bool is_safe_mode();
uint32_t same_fault_streak();
// The persisted last_code that (repeatedly) triggered a reboot -- "" if
// none recorded yet. For the SAFE_MODE screen's "Reason: <short code>"
// line (§3 of the 2026-08-25 follow-up) -- read once from NVS in
// recovery_supervisor_init(), not re-read from flash on every render.
const char* safe_mode_reason();

// Logs the recovery event (via record_recovery_event), persists the reboot
// reason + updates the same-fault streak in NVS (so reboot-loop protection
// above sees it next boot), flushes Serial, then calls ESP.restart(). Never
// returns. `detail` should be short (truncated to RecoveryEvent::detail's
// size) and must never contain secrets (§34 pattern already used
// elsewhere) -- this is operational diagnostics only, never business data.
[[noreturn]] void request_controlled_reboot(RecoveryCode code, const char* detail, uint8_t journal_pressure,
                                            uint32_t memory_free_bytes,
                                            uint32_t memory_largest_block_bytes);

// DEV-only test hook (§9 of the 2026-08-25 follow-up): forces the
// persisted same-fault streak straight to the SAFE_MODE threshold for
// `code`, then reboots -- so SAFE_MODE's real boot path can be verified on
// actual hardware without waiting for kSafeModeThreshold genuine faults.
// Gated by the caller (kiosk_runtime_v2.ino's serial command dispatch),
// not by this function itself, matching this file's other functions.
[[noreturn]] void force_safe_mode_for_test(RecoveryCode code);

}  // namespace kiosk::health
