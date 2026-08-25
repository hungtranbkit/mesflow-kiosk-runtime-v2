#include "recovery_supervisor.h"

#include <Preferences.h>

#include <cstring>
#include <string>

#include "structured_log.h"

namespace kiosk::health {

namespace {
RecoveryEvent g_history[kRecoveryHistoryCapacity];
int g_history_count = 0;  // number of valid entries so far (caps at capacity)
int g_history_next = 0;   // next slot to write (wraps)

constexpr const char* kPrefsNamespace = "recovery";
constexpr uint32_t kSafeModeThreshold = 3;

bool g_safe_mode = false;
uint32_t g_same_fault_streak = 0;
bool g_marked_stable_this_boot = false;
std::string g_safe_mode_reason;  // last persisted last_code, read once at init
}  // namespace

const char* recovery_code_to_string(RecoveryCode code) {
  switch (code) {
    case RecoveryCode::LOW_MEMORY: return "RECOVERY_LOW_MEMORY";
    case RecoveryCode::JOURNAL_PRESSURE: return "RECOVERY_JOURNAL_PRESSURE";
    case RecoveryCode::TASK_CREATE_FAILED: return "RECOVERY_TASK_CREATE_FAILED";
    case RecoveryCode::NETWORK_TIMEOUT: return "RECOVERY_NETWORK_TIMEOUT";
    case RecoveryCode::STATE_DESYNC: return "RECOVERY_STATE_DESYNC";
    case RecoveryCode::UI_STALL: return "RECOVERY_UI_STALL";
    case RecoveryCode::WATCHDOG: return "RECOVERY_WATCHDOG";
    case RecoveryCode::REBOOT_LOOP: return "RECOVERY_REBOOT_LOOP";
    case RecoveryCode::SCANNER_REINIT: return "RECOVERY_SCANNER_REINIT";
    case RecoveryCode::KEYPAD_REINIT: return "RECOVERY_KEYPAD_REINIT";
  }
  return "RECOVERY_UNKNOWN";
}

void record_recovery_event(RecoveryCode code, const char* detail, uint8_t journal_pressure,
                           uint32_t memory_free_bytes, uint32_t memory_largest_block_bytes) {
  RecoveryEvent& e = g_history[g_history_next];
  e.code = code;
  std::strncpy(e.detail, detail ? detail : "", sizeof(e.detail) - 1);
  e.detail[sizeof(e.detail) - 1] = '\0';
  e.uptime_ms = millis();
  e.journal_pressure = journal_pressure;
  e.memory_free_bytes = memory_free_bytes;
  e.memory_largest_block_bytes = memory_largest_block_bytes;

  g_history_next = (g_history_next + 1) % kRecoveryHistoryCapacity;
  if (g_history_count < kRecoveryHistoryCapacity) ++g_history_count;

  log_structured("WARN", recovery_code_to_string(code), "recovery_supervisor", detail ? detail : "");
}

int recovery_history_count() { return g_history_count; }

const RecoveryEvent& recovery_history_at(int index) {
  // Oldest-first indexing over however many slots are actually filled --
  // once the ring has wrapped (count == capacity), the oldest entry is
  // whatever g_history_next currently points to (the next slot due to be
  // overwritten); before it wraps, oldest is simply slot 0.
  int start = (g_history_count < kRecoveryHistoryCapacity) ? 0 : g_history_next;
  int actual = (start + index) % kRecoveryHistoryCapacity;
  return g_history[actual];
}

void recovery_supervisor_init() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {  // read-only open -- fine if the namespace doesn't exist yet
    g_same_fault_streak = 0;
    g_safe_mode = false;
    return;
  }
  g_same_fault_streak = prefs.getUInt("streak", 0);
  g_safe_mode_reason = prefs.getString("last_code", "").c_str();
  prefs.end();
  g_safe_mode = g_same_fault_streak >= kSafeModeThreshold;
  if (g_safe_mode) {
    char msg[96];
    snprintf(msg, sizeof(msg),
             "same_fault_streak=%u -- SAFE_MODE flag set (detection+logging only, no boot-path change yet)",
             static_cast<unsigned>(g_same_fault_streak));
    log_structured("ERROR", "RECOVERY_REBOOT_LOOP", "recovery_supervisor", msg);
  }
}

void recovery_supervisor_mark_stable() {
  if (g_marked_stable_this_boot) return;  // only need to clear the persisted streak once per boot
  g_marked_stable_this_boot = true;
  if (g_same_fault_streak == 0) return;   // nothing to clear

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUInt("streak", 0);
    prefs.end();
  }
  log_structured("INFO", "RECOVERY_STREAK_CLEARED", "recovery_supervisor",
                 "uptime has been stable this boot -- same-fault streak reset");
  g_same_fault_streak = 0;
  g_safe_mode = false;
}

bool is_safe_mode() { return g_safe_mode; }
uint32_t same_fault_streak() { return g_same_fault_streak; }
const char* safe_mode_reason() { return g_safe_mode_reason.c_str(); }

void request_controlled_reboot(RecoveryCode code, const char* detail, uint8_t journal_pressure,
                               uint32_t memory_free_bytes, uint32_t memory_largest_block_bytes) {
  record_recovery_event(code, detail, journal_pressure, memory_free_bytes, memory_largest_block_bytes);

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    String last_code = prefs.getString("last_code", "");
    const char* this_code = recovery_code_to_string(code);
    uint32_t streak = prefs.getUInt("streak", 0);
    streak = (last_code == this_code) ? streak + 1 : 1;
    prefs.putString("last_code", this_code);
    prefs.putUInt("streak", streak);
    prefs.putString("last_detail", detail ? detail : "");
    prefs.putULong("last_uptime_ms", millis());
    prefs.end();
    g_same_fault_streak = streak;
    g_safe_mode = streak >= kSafeModeThreshold;
  }

  char msg[128];
  snprintf(msg, sizeof(msg), "code=%s detail=%s same_fault_streak=%u", recovery_code_to_string(code),
           detail ? detail : "", static_cast<unsigned>(g_same_fault_streak));
  log_structured("ERROR", "RECOVERY_REBOOT", "recovery_supervisor", msg);

  Serial.flush();
  delay(100);  // give the log line a real chance to leave the UART before restart tears things down
  ESP.restart();
  while (true) {
    delay(1000);
  }  // unreachable -- ESP.restart() never returns; keeps the compiler happy about [[noreturn]]
}

void force_safe_mode_for_test(RecoveryCode code) {
  const char* this_code = recovery_code_to_string(code);
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putString("last_code", this_code);
    prefs.putUInt("streak", kSafeModeThreshold);
    prefs.putString("last_detail", "forced by DEV test hook (force-safe-mode)");
    prefs.putULong("last_uptime_ms", millis());
    prefs.end();
  }
  char msg[96];
  snprintf(msg, sizeof(msg), "forcing SAFE_MODE for code=%s, rebooting now", this_code);
  log_structured("WARN", "RECOVERY_TEST_FORCED", "recovery_supervisor", msg);
  Serial.flush();
  delay(100);
  ESP.restart();
  while (true) {
    delay(1000);
  }  // unreachable
}

}  // namespace kiosk::health
