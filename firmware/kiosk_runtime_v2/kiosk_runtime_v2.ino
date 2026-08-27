// MESFlow Kiosk Runtime v2 — entry point.
//
// This file only wires modules together (setup/loop). Business logic,
// protocol, hardware drivers, etc. all live under src/ — see
// docs/ARCHITECTURE.md for the module map and docs/ for the full design.
//
// DO NOT DEPLOY TO PRODUCTION — see README.md.

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "src/config/hardware_pins.h"
#include "src/debug/debug_server.h"
#include "src/health/low_memory_supervisor.h"
#include "src/health/memory_diag.h"
#include "src/health/recovery_supervisor.h"
#include "src/health/structured_log.h"
#include "src/hardware/display.h"
#include "src/hardware/hardware_selftest.h"
#include "src/hardware/keypad_pcf8574.h"
#include "src/hardware/scanner_gm65.h"
#include "src/network/bootstrap_client.h"
#include "src/network/endpoint_utils.h"
#include "src/network/heartbeat_client.h"
#include "src/network/net_diag.h"
#include "src/network/network_worker.h"
#include "src/network/time_sync.h"
#include "src/network/wifi_manager.h"
#include "src/network/wifi_setup_portal.h"
#include "src/protocol/ids.h"
#include "src/runtime/boot_diagnostics.h"
#include "src/runtime/event_bus.h"
#include "src/runtime/kiosk_runtime.h"
#include "src/runtime/ui_sync_controller.h"
#include "src/runtime/wifi_recovery_controller.h"
#include "src/security/device_identity.h"
#include "src/storage/config_store.h"
#include "src/storage/event_journal.h"
#include "src/storage/ui_bundle_store.h"
#include "src/ui/renderer.h"

namespace {

kiosk::runtime::EventBus g_bus;
// Must be declared before g_display, in this same translation unit: Display
// now inherits Adafruit_ILI9341 (for shadow-framebuffer overrides, see
// display.h), and a base class is always constructed before any derived
// data members -- so the SPIClass instance cannot be owned by Display
// itself. Declaration order here is what guarantees it exists first.
SPIClass g_display_spi(HSPI);
kiosk::hardware::Display g_display(&g_display_spi);
kiosk::hardware::ScannerGm65 g_scanner(g_bus);
kiosk::hardware::KeypadPcf8574 g_keypad(g_bus);
kiosk::storage::ConfigStore g_config;
kiosk::security::DeviceIdentity g_identity;
kiosk::network::TimeSync g_time_sync;
kiosk::ui::Renderer g_renderer(g_display);
kiosk::network::WifiManager g_wifi(g_bus);
kiosk::network::WifiSetupPortal g_wifi_portal(g_bus, g_config, g_identity);
kiosk::storage::UiBundleStore g_ui_bundle_store;
// Phase 3A durable journal (docs/OFFLINE.md), SHADOW MODE only -- see
// kiosk_runtime.cpp's send_business_event()/apply_event_response(). 256KB
// out of the SPIFFS partition's 1.5MB (0x180000), leaving well over 1MB
// free for UI bundle A/B slots (each ~4KB) plus headroom -- see the Phase
// 3A report for the real measured SPIFFS numbers this was sized against.
constexpr uint32_t kJournalCapacityBytes = 256u * 1024u;
kiosk::storage::EventJournal g_journal;
// Single persistent network worker (2026-08-26 "eliminate recurrent server
// connection failures" pass, §2-§5) -- ONE FreeRTOS task for the whole
// device's lifetime, created once by begin() in setup() below. Declared
// here (not inside KioskRuntime) so it can eventually also be shared by
// HeartbeatClient/BootstrapClient once those are migrated onto it too
// (pending work -- see network_worker.h's own header comment).
kiosk::network::NetworkWorker g_network_worker;
kiosk::runtime::KioskRuntime g_runtime(g_bus, g_renderer, g_config, g_identity, g_time_sync, g_ui_bundle_store,
                                       g_journal, g_network_worker);
kiosk::runtime::UiSyncController g_ui_sync(g_ui_bundle_store, g_runtime);
// §4 of the 2026-08-25 finish-anti-stuck-recovery follow-up -- the recovery
// menu's "RETRY NETWORK"/"RESYNC" actions forwarded in as callbacks (see
// WifiRecoveryController's own header comment for why: it must not depend
// on WifiManager's/KioskRuntime's concrete types). Safe to reference g_wifi/
// g_runtime here even though this line runs before either's begin()/init()
// -- these lambdas aren't INVOKED until the operator actually picks a menu
// option, long after both are fully set up.
kiosk::runtime::WifiRecoveryController g_wifi_recovery(
    g_bus, g_renderer, g_wifi_portal, [] { g_wifi.retry_now(); },
    [] { g_runtime.request_manual_resync(); }, [] { g_runtime.refresh_idle_screen(); },
    [] {
      // §4 (2026-08-26 UX-hardening pass): gathered here, not inside
      // WifiRecoveryController, so that class stays independent of
      // KioskRuntime's/WiFi's concrete types (same reasoning as its other
      // 3 callbacks) -- everything needed is already in scope in this file.
      bool online = g_wifi.state() == kiosk::network::WifiState::CONNECTED;
      // Inlined rather than calling current_wifi_indicator() -- that helper
      // is declared further down this same anonymous namespace, after this
      // lambda is parsed (this object's initializer runs at static-init
      // time, but the LAMBDA BODY itself must still name only symbols
      // already visible at the point it's written).
      kiosk::ui::WifiIndicator wifi_ind = kiosk::ui::WifiIndicator::UNKNOWN;
      switch (g_wifi.state()) {
        case kiosk::network::WifiState::CONNECTED: wifi_ind = kiosk::ui::WifiIndicator::CONNECTED; break;
        case kiosk::network::WifiState::CONNECTING: wifi_ind = kiosk::ui::WifiIndicator::CONNECTING; break;
        case kiosk::network::WifiState::DISCONNECTED: wifi_ind = kiosk::ui::WifiIndicator::DISCONNECTED; break;
      }
      g_renderer.draw_device_info_screen(
          g_runtime.server_environment(), g_runtime.api_endpoint_value(), g_runtime.server_version(),
          g_runtime.device_id(), g_runtime.hardware_id(), String(KIOSK_RUNTIME_VERSION),
          online ? WiFi.SSID() : String(""), online ? WiFi.localIP().toString() : String(""),
          online ? WiFi.RSSI() : 0, online, g_runtime.last_sync_iso(), g_runtime.offline_queue_size(),
          wifi_ind);
    });
kiosk::network::BootstrapClient g_bootstrap;

kiosk::hardware::SelfTestResult g_selftest;
kiosk::runtime::BootDiagnostics g_diagnostics;

kiosk::network::HeartbeatClient g_heartbeat(g_identity, g_time_sync, g_runtime, g_keypad, g_selftest,
                                            g_diagnostics, g_bootstrap, g_network_worker);

#if MESFLOW_DEBUG_API
// Constructed after g_selftest/g_diagnostics exist (it holds a reference to
// them) but only actually started in setup() once they've been populated.
kiosk::debug::DebugServer g_debug_server(g_bus, g_display, g_renderer, g_runtime, g_keypad,
                                          g_selftest, g_diagnostics, g_identity, g_time_sync,
                                          g_bootstrap, g_ui_bundle_store, g_ui_sync);
#endif

unsigned long g_last_diagnostics_print_ms = 0;
constexpr unsigned long kDiagnosticsPrintIntervalMs = 30000;

// Real, confirmed root cause of a periodic reboot found live (2026-08-26
// field report: "quét thẻ ... đang kiểm tra rất lâu ... rồi reload lại
// wifi"): the low-memory supervisor below (WARNING/CRITICAL check +
// opportunistic journal compaction rescue) used to run ONLY once per
// kDiagnosticsPrintIntervalMs (30s), piggybacked on the diagnostics-print
// timer purely because it happened to live in the same `if` block. Each
// business event costs real internal-SRAM (a JournalRecord's several
// std::string fields, kept live in EventJournalIndex's in-memory map until
// compaction runs) that is NEVER reclaimed until compaction actually
// executes -- confirmed live via a 100-scan/100s stress test: int_free fell
// from ~180KB to single-digit KB well within one 30s window at a ~1
// scan/second cadence (unrealistically fast for a human operator, but this
// device's cumulative event count across a full day's testing reaches the
// same wall eventually at any cadence, just slower). The compaction
// policy's OWN "should_consider_compaction()" trigger (event_journal_index.h)
// watches FLASH journal usage_pct, which stayed under 50% throughout every
// crash observed -- SRAM and flash-quota pressure are unrelated signals,
// and only the flash one was ever checked more than once per 30s.
// Decoupled onto its own MUCH shorter interval so the WARNING/CRITICAL
// rescue (and, in the worst case, the controlled-reboot escalation) can
// react within ~1s of real pressure building, not up to 30s later --
// heap_caps_get_largest_free_block()/get_free_size() are cheap calls,
// already called this often elsewhere in this same file with no measured
// cost concern (see send_business_event()'s own failure-path check).
unsigned long g_last_low_memory_check_ms = 0;
constexpr unsigned long kLowMemoryCheckIntervalMs = 1000;
// See the CRITICAL branch's own comment (below, in loop()) for the real
// bug this pair fixes: the reactive rescue used to call the BLOCKING
// compact() (measured ~4.7s) and reboot immediately if still CRITICAL
// right after -- now it starts/lets run the non-blocking incremental
// compact_tick() path instead, and only escalates to reboot after CRITICAL
// has persisted this many consecutive ~1s checks (10s total patience),
// giving compaction a real chance to actually catch up first.
uint32_t g_consecutive_critical_low_memory_checks = 0;
constexpr uint32_t kCriticalRebootPatienceChecks = 10;

// Network self-recovery (2026-08-26 field report; reboot escalation
// REMOVED 2026-08-26 "eliminate recurrent server connection failures" pass
// -- see kNetworkStuckReconnectThreshold's own comment below). RECONNECT
// (WifiManager::force_reconnect() -- a plain disconnect+fresh WiFi.begin(),
// the SAME path a genuine drop already takes) is retried every
// kNetworkStuckReconnectRepeatEvery consecutive TCP_CONNECT_FAIL events,
// FOREVER -- there is deliberately NO reboot escalation for this condition
// anymore. Explicit instruction from this task: "Do not solve this by
// rebooting/resetting the ESP when a request fails. The runtime must
// recover networking in-place and remain usable" -- DNS_FAIL/
// TCP_CONNECT_FAIL/HTTP_TIMEOUT/HTTP_5XX are all recoverable operating
// conditions, never a reboot condition. A previous round DID escalate to
// reboot after a fixed number of failed reconnects; that is the exact
// policy this removes. g_network_reconnect_attempted_this_streak still
// prevents calling force_reconnect() on every single failure within one
// repeat window -- cleared the moment the streak itself clears (a success,
// or a differently-classified failure).
constexpr uint32_t kNetworkStuckReconnectThreshold = 2;
constexpr uint32_t kNetworkStuckReconnectRepeatEvery = 3;
// 0 means "no reconnect fired yet this streak" -- otherwise the streak
// value AT which the most recent reconnect fired, so the next one is due
// once the streak has grown by kNetworkStuckReconnectRepeatEvery beyond it.
uint32_t g_last_reconnect_at_streak = 0;

// §6 of the 2026-08-25 finish-anti-stuck-recovery follow-up: lightweight UI
// stall detection. g_display.frame_id() (bumped once per Renderer::end_screen()
// call -- already existed, this just reuses it) is the "render_generation"
// the task asks for; g_renderer.current_screen_id() is "last_screen_id".
// Honest limitation: this can only ever catch a loop() that is STILL
// RUNNING but somehow not rendering -- a genuinely hung loop() (the display
// SPI transaction itself blocking forever, say) would also freeze this
// check, since it runs on the same task. Real value is still there: most
// realistic "stuck" cases (nothing left to trigger a redraw, not the loop
// itself dying) are exactly what this catches.
uint32_t g_last_seen_frame_id = 0;
unsigned long g_last_render_progress_ms = 0;
bool g_ui_stall_redraw_attempted = false;
bool g_ui_stall_display_reinit_attempted = false;
// 5 minutes -- long enough that a genuinely idle WAIT_EMPLOYEE screen
// (nothing SHOULD redraw for a while) never false-positives, short enough
// to matter for an operator who's actually stuck.
constexpr unsigned long kUiStallThresholdMs = 300000;

// Auto-compaction (2026-08-24, self-recovery task) -- checked on the same
// cadence as the periodic diagnostics print above, not tied to it: compact()
// itself is a fast no-op (JOURNAL_COMPACTION_SKIP) whenever nothing is over
// its retention count, so checking every 30s costs nothing when there's
// nothing to do, and catches real pressure promptly when there is.
unsigned long g_last_compaction_check_ms = 0;
constexpr unsigned long kCompactionCheckIntervalMs = 30000;
// Tracks the active->inactive edge across compact_tick() calls (2026-08-25)
// so the AFTER memory snapshot logs exactly once, when the (possibly
// multi-tick) compaction actually finishes, not right after begin_compaction()
// merely starts it.
bool g_compaction_was_active = false;

bool g_bootstrap_attempted = false;
// 2026-08-27: true from the moment a bootstrap request is enqueued through
// NetworkWorker until its async result is picked up -- see the enqueue
// site's own comment for why this exists (distinguishes "already asked,
// waiting" from "done, or free to try again" now that the call is async).
bool g_bootstrap_inflight = false;
bool g_time_sync_started = false;

// Real race found live (2026-08-24): WiFi.status()==WL_CONNECTED can flip
// true a short moment before the DHCP client has actually finished (no
// usable default gateway/DNS yet) -- bootstrap firing on the very FIRST
// loop() iteration after CONNECTED can hit http.POST() failing almost
// instantly (status<=0, "no response from backend" within ~130ms --
// nowhere near RUNTIME_HTTP_TIMEOUT_MS, so this is a fast connect-level
// failure, not a real timeout). Reproduced 2/2 on a cold boot. Since
// bootstrap only ever fired ONCE per boot with no retry, a device that
// hit this race stayed on state=null (no business state at all) for the
// rest of that boot. Retry a bounded number of times with a short cooldown
// -- only a TRANSPORT-level failure (BootstrapStatus::FAILED) retries;
// a real response (OK or a business REJECTED) still marks this done
// immediately, same as before.
//
// Lowered from 5 -> 2 (2026-08-27, real field report same day as the DHCP
// race fix's own original bug): BootstrapClient::attempt() is STILL a
// single fully-SYNCHRONOUS HTTPClient call on the main loop() thread (never
// migrated onto NetworkWorker) -- each attempt can block loop() for up to
// RUNTIME_HTTP_TIMEOUT_MS (2.5s), during which the keypad/scanner are never
// polled at all. This retry loop re-arms on EVERY WiFi reconnect (see
// g_last_seen_reconnect_count below), including the pre-existing (not new
// today) consecutive_tcp_connect_fail_ -> force_reconnect() self-heal path.
// At 5 attempts x (up to 2.5s block + 3s cooldown), a single reconnect
// could cost up to ~27.5s of intermittent-but-real UI unresponsiveness --
// closely matching a live report of "quét thẻ báo nhận mã rồi treo ~30s".
// 2 attempts (one real retry, for the DHCP race above) caps the worst case
// at ~8s while still covering that original bug. The real architectural
// fix -- migrating BootstrapClient onto NetworkWorker so it never blocks
// loop() at all -- is a bigger change, flagged separately, not done here
// under time pressure.
constexpr int kBootstrapMaxAttempts = 2;
constexpr unsigned long kBootstrapRetryCooldownMs = 3000;
int g_bootstrap_attempt_count = 0;
unsigned long g_bootstrap_next_attempt_ms = 0;

// §19 of the 2026-08-26 UX-hardening pass: real confirmed gap found reading
// this file -- g_bootstrap_attempted was NEVER reset anywhere after boot,
// so a later Wi-Fi drop+reconnect resumed sending events/heartbeats against
// whatever state_projection_/UI-bundle-version was last established at
// BOOT time, never re-verifying anything (environment/version, once Phase
// 2 lands; UI-bundle desired version; the state snapshot itself) until a
// full manual reboot. g_wifi.reconnect_count() (already existed, added
// 2026-08-25 for net-diag) increments on every CONNECTED-after-drop
// transition -- tracking it here and re-arming the exact same
// once-per-"boot" bootstrap machinery on change re-triggers a real
// bootstrap/resync/ui-bundle-check with zero new plumbing.
uint32_t g_last_seen_reconnect_count = 0;

void keypad_calibration_prompt(char key, uint8_t index, uint8_t total) {
  g_renderer.draw_keypad_calibration_prompt(key, index, total);
}

kiosk::ui::WifiIndicator current_wifi_indicator() {
  switch (g_wifi.state()) {
    case kiosk::network::WifiState::CONNECTED: return kiosk::ui::WifiIndicator::CONNECTED;
    case kiosk::network::WifiState::CONNECTING: return kiosk::ui::WifiIndicator::CONNECTING;
    case kiosk::network::WifiState::DISCONNECTED: return kiosk::ui::WifiIndicator::DISCONNECTED;
  }
  return kiosk::ui::WifiIndicator::UNKNOWN;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);  // let USB-serial enumerate before the first log line

  Serial.printf("{\"level\":\"INFO\",\"code\":\"PROFILE\",\"profile\":\"%s\"}\n",
                MESFLOW_PROFILE_NAME);

#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("BOOT_EARLY");
#endif

  // §13 of the self-recovery task -- reads any PERSISTED same-fault reboot
  // streak from NVS as early as possible, before anything else this boot
  // could itself crash on. PROD+DEV both (this is a safety feature, not a
  // debug tool).
  kiosk::health::recovery_supervisor_init();


  g_config.init();
  g_identity.init();
  g_ui_bundle_store.init();  // loads last-known-good UI bundle from NVS, if any (§5/§16)
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_UI_BUNDLE_LOAD");
#endif
  g_journal.init(kJournalCapacityBytes);  // Phase 3A: recovers the durable event journal's index (SPIFFS-backed, shadow mode)
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_JOURNAL_INIT");
#endif

  // §2 of the 2026-08-26 "eliminate recurrent server connection failures"
  // pass: create the ONE persistent network worker task now, before
  // anything below can possibly try to send/fetch anything through it.
  // Never created again for the rest of this boot.
  g_network_worker.begin();
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_NETWORK_WORKER_BEGIN");
#endif

  String boot_id = kiosk::protocol::generate_random_hex_id(8).c_str();

  // SPI pin binding for the TFT (root cause of a real, since-fixed bug: see
  // display.cpp's Display::init()) now lives entirely inside Display::init()
  // itself, so display bring-up has exactly one owner.
  g_selftest.display_ok = g_display.init();
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_DISPLAY_INIT");
#endif
  g_selftest.scanner_ok = g_scanner.init();
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_SCANNER_INIT");
#endif
  g_selftest.keypad_ok = g_keypad.init();  // false = DEGRADED, not fatal
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_KEYPAD_INIT");
#endif

  g_diagnostics = kiosk::runtime::collect_boot_diagnostics(boot_id);

  Serial.printf("{\"level\":\"INFO\",\"code\":\"BOOT\",\"boot_id\":\"%s\","
                "\"fw_version\":\"%s\",\"build_id\":\"%s\",\"chip\":\"%s\","
                "\"flash_mb\":%u,\"reset_reason\":\"%s\",\"hardware_id\":\"%s\","
                "\"device_id\":\"%s\",\"provisioning_state\":\"%s\"}\n",
                g_diagnostics.boot_id.c_str(), g_diagnostics.fw_version.c_str(),
                g_diagnostics.build_id.c_str(), g_diagnostics.chip_model.c_str(),
                g_diagnostics.flash_mb, g_diagnostics.reset_reason.c_str(),
                g_identity.hardware_id().c_str(), g_identity.device_id().c_str(),
                kiosk::security::provisioning_state_to_string(g_identity.state()));

  g_runtime.begin(boot_id);
  g_wifi_recovery.begin();
  g_renderer.draw_boot_screen(g_diagnostics, g_selftest.scanner_ok, g_selftest.keypad_ok);
  delay(2000);  // hold the boot/diagnostics screen briefly before going to waiting/identity state
  g_runtime.refresh_idle_screen();

  g_wifi.begin(g_config.wifi_ssid(), g_config.wifi_password());
#if MESFLOW_DEBUG_API
  kiosk::health::log_memory_snapshot("AFTER_WIFI_INIT");
#endif

#if MESFLOW_DEBUG_API
  // §2 of the 2026-08-25 finish-anti-stuck-recovery follow-up: SAFE_MODE
  // avoids "heavy debug services" -- the debug HTTP server (screenshot/
  // ui-state/device-state over HTTP) is exactly that, so it's skipped
  // entirely while SAFE_MODE is active. Serial debug commands (including
  // the fault-injection test hooks below) are UNAFFECTED -- they don't go
  // through g_debug_server at all, so SAFE_MODE force-testing itself never
  // depends on the thing SAFE_MODE is deliberately not starting.
  if (!kiosk::health::is_safe_mode()) {
    // TEMPORARY diagnostic fix (real bug found live, 2026-08-23): on a
    // genuinely fresh/never-provisioned board (blank NVS, no Wi-Fi
    // credentials ever saved), WifiManager::begin() correctly short-circuits
    // without ever calling WiFi.mode()/WiFi.begin() -- but DebugServer::begin()
    // (WebServer::begin()) unconditionally assumes the WiFi/LWIP stack is
    // already initialized, and asserts (`xQueueSemaphoreTake queue.c:1709`)
    // almost immediately after setup() if it never was. This crash-loops
    // before loop() ever runs once, meaning it can't even be worked around
    // via a serial command. Confirmed: our provisioned kiosk board (which has
    // always had Wi-Fi credentials since Phase 0) never hits this path.
    // Calling WiFi.mode(WIFI_STA) unconditionally -- even with no SSID yet --
    // is enough to initialize the stack without actually attempting a
    // connection. Left here as a real fix candidate; revisit whether this
    // should be unconditional in WifiManager::begin() itself instead of only
    // guarded here.
    WiFi.mode(WIFI_STA);
    g_debug_server.begin();
    kiosk::health::log_memory_snapshot("AFTER_DEBUG_SERVER_INIT");

    // §6 of the 2026-08-24 open-AP rework: pause the (unauthenticated,
    // DEV-only) debug HTTP API for as long as the now password-less setup AP
    // is up -- see DebugServer::set_paused()'s comment. Serial debug commands
    // are unaffected (don't go through web_ at all).
    g_bus.subscribe([](const kiosk::runtime::LocalEvent& event) {
      if (event.kind != kiosk::runtime::LocalEventKind::WIFI_RECOVERY_STATE) return;
      g_debug_server.set_paused(event.text != "INACTIVE");
    });
  } else {
    kiosk::health::log_structured("WARN", "SAFE_MODE_BOOT", "kiosk_runtime_v2",
                                  "SAFE_MODE active -- debug HTTP server not started (serial commands still work)");
  }
#endif

  // §3 of the 2026-08-24 open-AP rework: the setup AP must never be
  // permanent -- it only runs when (a) there are no usable stored Wi-Fi
  // credentials, (b) the operator holds '*' for 10s, or (c) an explicit
  // recovery mode is entered. (b)/(c) are handled entirely by
  // WifiRecoveryController/WifiSetupPortal already; this is (a), which
  // previously had no code path at all -- a fresh/never-provisioned board
  // just sat DISCONNECTED forever with no way back in except the physical
  // '*' hold (which itself requires keypad calibration to have already run
  // -- see docs/WIFI_RECOVERY.md's "Prerequisite" section). Runs AFTER the
  // WiFi.mode(WIFI_STA) debug-server workaround above so it always wins:
  // WifiSetupPortal::start() sets WIFI_AP_STA itself.
  if (g_config.wifi_ssid().length() == 0) {
    g_wifi_portal.start("no_credentials");
  }

  // §21 of the 2026-08-26 UX-hardening pass: a REAL hardware watchdog on
  // loopTask, fed once per loop() iteration below. Registered at the very
  // END of setup(), not the start -- a REAL bug found live flashing this to
  // the actual board: registering it early (before journal recovery/
  // hardware init/the boot-screen hold, which together routinely take
  // 8-10+ seconds one-time at boot) with feeding only happening in loop()
  // meant setup() itself never got to finish before the watchdog fired,
  // panic-crash-looping the device forever on every single boot. Boot's
  // one-time slow work is fully bounded on its own already (SPIFFS
  // recovery/hardware init/WIFI_CONNECT_TIMEOUT_MS's async connect, none of
  // which can hang indefinitely) -- the watchdog's real job is catching a
  // STEADY-STATE loop() that stops iterating, which this still does.
  // trigger_panic=true so a timeout resets the chip (visible next boot as
  // reset_reason=TASK_WDT/PANIC via the EXISTING boot_diagnostics.cpp
  // reset-reason reporting -- no separate "wire it into RecoveryCode" step
  // needed, since that taxonomy is for firmware-REQUESTED controlled
  // reboots, not an async hardware reset this code isn't running to log
  // during). idle_core_mask=0: only loopTask itself is subscribed
  // (esp_task_wdt_add(NULL) below) -- the idle tasks are Arduino core's own
  // concern, not this firmware's.
  {
    esp_task_wdt_config_t wdt_config = {};
    wdt_config.timeout_ms = TASK_WATCHDOG_TIMEOUT_S * 1000;
    wdt_config.idle_core_mask = 0;
    wdt_config.trigger_panic = true;
    esp_err_t wdt_err = esp_task_wdt_init(&wdt_config);
    if (wdt_err == ESP_ERR_INVALID_STATE) {
      // Arduino core's own startup already initialized the TWDT (a common
      // IDF5-based arduino-esp32 default, watching idle tasks only) --
      // reconfigure it to our own values/scope instead of treating this as
      // a failure.
      wdt_err = esp_task_wdt_reconfigure(&wdt_config);
    }
    esp_err_t add_err = esp_task_wdt_add(NULL);  // NULL = current task (loopTask, since setup() runs on it)
    Serial.printf("{\"level\":\"%s\",\"code\":\"WATCHDOG_INIT\",\"timeout_s\":%d,\"init_err\":%d,\"add_err\":%d}\n",
                  (wdt_err == ESP_OK && add_err == ESP_OK) ? "INFO" : "WARN", TASK_WATCHDOG_TIMEOUT_S,
                  static_cast<int>(wdt_err), static_cast<int>(add_err));
  }
}

namespace {

// DEV-ONLY provisioning/maintenance stub over the serial console (the same
// USB cable used for logs/flashing):
//
//   wifi:<ssid>,<password>    save Wi-Fi creds directly to NVS, then reboot
//   keypad-calibrate          run the guided 12-key calibration (blocking)
//   recovery-info             print this device's Wi-Fi recovery AP SSID
//                             (ap_password is always "" -- the AP is open,
//                             no password, see docs/WIFI_RECOVERY.md).
//                             Read-only; does not start the portal.
//   display-test              DEV-only physical display diagnostic: drives the
//                             REAL display hardware (not just the shadow
//                             framebuffer) through white/black/red/green/blue/
//                             checkerboard/border, ~2.5s each (blocking)
//   display-pins               prints the compiled BL/RST/CS/DC/SCLK/MOSI/MISO
//                             pin map and the ACTUAL current level read back
//                             off BL (CS/DC are SPI-library-managed, not held
//                             statically -- reported as n/a, not guessed)
//   display-reinit             backlight OFF -> reset (hardware pulse if wired,
//                             else honestly logs SKIPPED) -> begin()/rotation
//                             -> fill RED -> backlight ON. No reboot, no UI.
//   backlight-test              raw GPIO cycle only (LOW 2s / HIGH 5s / LOW 2s
//                             / HIGH) -- isolates the backlight pin/driver
//                             from all SPI/display code
//   lcd-physical-test          named screen sequence with real text (TEST LCD /
//                             WHITE SCREEN / RED / GREEN / BLUE / bordered
//                             MESFLOW+LCD OK+1234567890), ~3s each, then holds
//                             on "LCD TEST / IF YOU CAN READ THIS / DISPLAY IS
//                             WORKING" indefinitely. Real hardware calls only.
//   api-endpoint:<url>        validate + save api_endpoint to NVS, then reboot
//                             (KioskRuntime reads it once at boot, same as
//                             Wi-Fi creds -- so this reboots too, deliberately
//                             matching the wifi: command's behavior)
//   provision:<device_id>     assign device_id, move provisioning_state to
//                             ACTIVE, then reboot (§4/§5/§36 -- DEV-path
//                             provisioning skeleton; see docs/PROVISIONING.md)
//   suspend / revoke          DEV-only: force provisioning_state for testing
//                             §5's behavior without a real backend admin
//                             action (KIOSK-079/080)
//   debug-screenshot           Serial Visual Debug Fallback (MESFLOW_DEBUG_API
//                             builds only): writes the current framebuffer
//                             over Serial as a framed binary blob (magic
//                             "MFSB", header + CRC32) -- see
//                             DebugServer::write_screenshot_serial(). Use
//                             when the host can't reach /debug/screenshot
//                             over HTTP (different subnet). Host-side:
//                             tools/capture_screen_serial.py or
//                             scripts/capture-screen-serial.sh.
//   debug-ui-state              same idea as debug-screenshot but for
//                             GET /debug/ui-state's JSON, wrapped in a
//                             "##MFDBG-BEGIN ui-state##" / "##MFDBG-END##"
//                             marker pair.
//   debug-device-state          same idea for GET /debug/device-state's JSON.
//   debug-net-diag               2026-08-25 connectivity investigation:
//                             layer-by-layer WiFi/gateway/DNS/TCP/HTTP probe
//                             against dev.mesflow.net + prod.mesflow.net,
//                             plain WiFiClient only (no TLS) -- see
//                             src/network/net_diag.h's own header comment.
//   debug-input:<json>          same SCAN/KEY_DOWN/KEY_UP dispatch as
//                             POST /debug/input, e.g.
//                             debug-input:{"type":"SCAN","value":"00152"} --
//                             drives real state transitions over serial when
//                             there's no HTTP path and no physical scanner/
//                             keypad access. Response wrapped the same way
//                             (##MFDBG-BEGIN debug-input##/##MFDBG-END##).
//
// The real, backend-independent recovery path for an operator on the floor
// is holding '*' for 10s (WifiRecoveryController) -- this serial interface
// is only for bring-up/dev convenience and initial keypad calibration,
// which nothing else can bootstrap (see docs/HARDWARE.md).
void poll_serial_provisioning() {
  static String line;
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (line.startsWith("wifi:")) {
        String rest = line.substring(5);
        int comma = rest.indexOf(',');
        if (comma > 0) {
          String ssid = rest.substring(0, comma);
          String pass = rest.substring(comma + 1);
          g_config.set_wifi_credentials(ssid, pass);
          Serial.printf("{\"level\":\"INFO\",\"code\":\"CONFIG_WIFI_SET\","
                        "\"module\":\"provisioning\",\"ssid\":\"%s\"}\n",
                        ssid.c_str());
          Serial.println("Rebooting to apply Wi-Fi credentials...");
          delay(200);
          ESP.restart();
        } else {
          Serial.println("Usage: wifi:<ssid>,<password>");
        }
      } else if (line == "keypad-calibrate") {
        Serial.println("Starting keypad calibration -- press and release each key shown on-screen.");
        bool ok = g_keypad.run_calibration(keypad_calibration_prompt);
        Serial.println(ok ? "Calibration OK." : "Calibration FAILED.");
        g_runtime.refresh_idle_screen();
      } else if (line.startsWith("api-endpoint:")) {
        String url = line.substring(13);
        url.trim();
        auto result = g_config.set_api_endpoint(url);
        if (result != kiosk::protocol::BackendUrlValidation::OK) {
          Serial.printf("Rejected: %s\n", kiosk::protocol::backend_url_validation_to_string(result));
        } else {
          Serial.printf("{\"level\":\"INFO\",\"code\":\"CONFIG_API_ENDPOINT_SET\","
                        "\"module\":\"provisioning\",\"url\":\"%s\"}\n",
                        url.c_str());
          Serial.println("Rebooting to apply api_endpoint...");
          delay(200);
          ESP.restart();
        }
      } else if (line.startsWith("expected-env:")) {
        // §3 of the 2026-08-26 UX-hardening pass: operator-declared "which
        // environment should this device be talking to" -- compared against
        // the backend's own real server_role on every bootstrap
        // (KioskRuntime::apply_server_environment()). Accepts any string
        // (validated/mapped case-insensitively at comparison time via
        // kiosk::protocol::environment_from_config_string(); an unrecognized
        // value just maps to UNKNOWN, same "fail closed, don't hard-block a
        // typo silently" posture as everywhere else in this file) --
        // deliberately not rejecting here the way api-endpoint: does, since
        // there's no real backend round-trip to validate against, just a
        // local label.
        String env = line.substring(13);
        env.trim();
        g_config.set_expected_environment(env);
        Serial.printf("{\"level\":\"INFO\",\"code\":\"CONFIG_EXPECTED_ENV_SET\","
                      "\"module\":\"provisioning\",\"expected_environment\":\"%s\"}\n",
                      env.c_str());
        Serial.println("Rebooting to apply expected_environment...");
        delay(200);
        ESP.restart();
      } else if (line.startsWith("provision:")) {
        String new_id = line.substring(10);
        new_id.trim();
        if (g_identity.provision(new_id)) {
          Serial.printf("Provisioned as %s. Rebooting...\n", new_id.c_str());
          delay(200);
          ESP.restart();
        } else {
          Serial.println("Usage: provision:<device_id>");
        }
      } else if (line == "suspend") {
        g_identity.set_state(kiosk::security::ProvisioningState::SUSPENDED);
        g_runtime.refresh_idle_screen();
      } else if (line == "revoke") {
        g_identity.set_state(kiosk::security::ProvisioningState::REVOKED);
        g_runtime.refresh_idle_screen();
      } else if (line == "recovery-info") {
        g_wifi_portal.compute_ap_credentials();
        Serial.printf("{\"level\":\"INFO\",\"code\":\"NET_WIFI_RECOVERY_INFO\","
                      "\"device_id\":\"%s\",\"hardware_id\":\"%s\",\"ap_ssid\":\"%s\","
                      "\"ap_password\":\"%s\"}\n",
                      g_identity.device_id().c_str(), g_identity.hardware_id().c_str(),
                      g_wifi_portal.ap_ssid().c_str(), g_wifi_portal.ap_password().c_str());
      } else if (line == "display-test") {
        // DEV-only physical display diagnostic: calls g_display's REAL
        // drawing entry points directly (the SAME overridden fillScreen/
        // fillRect that Display::init()/Renderer use -- see display.cpp,
        // every override calls Adafruit_ILI9341:: first, THEN mirrors into
        // the shadow framebuffer) -- this is not a shadow-framebuffer-only
        // test. Each phase is announced on serial AND held long enough to
        // observe physically, and /debug/screenshot can be captured during
        // any phase to compare "what the shadow framebuffer has" against
        // "what a person sees on the physical panel" (docs/VISUAL_DEBUG.md
        // invariant 8's whole point: serial/software success is not visual
        // proof).
        struct { const char* name; uint16_t color; } solids[] = {
            {"WHITE", ILI9341_WHITE}, {"BLACK", ILI9341_BLACK}, {"RED", ILI9341_RED},
            {"GREEN", ILI9341_GREEN}, {"BLUE", ILI9341_BLUE},
        };
        for (auto& phase : solids) {
          Serial.printf("{\"level\":\"INFO\",\"code\":\"DISPLAY_TEST_PHASE\",\"phase\":\"%s\"}\n", phase.name);
          g_display.fillScreen(phase.color);
          delay(2500);
        }
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_TEST_PHASE\",\"phase\":\"CHECKERBOARD\"}");
        {
          const int tile = 20;
          for (int y = 0; y < g_display.height(); y += tile) {
            for (int x = 0; x < g_display.width(); x += tile) {
              bool even = ((x / tile) + (y / tile)) % 2 == 0;
              g_display.fillRect(x, y, tile, tile, even ? ILI9341_WHITE : ILI9341_BLACK);
            }
          }
        }
        delay(2500);
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_TEST_PHASE\",\"phase\":\"BORDER\"}");
        g_display.fillScreen(ILI9341_BLACK);
        g_display.drawRect(0, 0, g_display.width(), g_display.height(), ILI9341_YELLOW);
        g_display.drawRect(4, 4, g_display.width() - 8, g_display.height() - 8, ILI9341_YELLOW);
        delay(2500);
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_TEST_DONE\"}");
        g_display.bump_frame_id();  // so a /debug/screenshot taken right after reflects the final phase
        g_runtime.refresh_idle_screen();
      } else if (line == "display-pins") {
        // Electrical diagnostic (§1/§3 of the display-fault investigation):
        // prints exactly what this build has compiled in, plus the ACTUAL
        // current level read back off each pin this code controls directly
        // -- never just "what we think we commanded". CS/DC are driven
        // internally by Adafruit_ILI9341/the SPI library on every transfer,
        // not held at a static level by our code, so their "current level"
        // is transient/meaningless between transfers -- reported as N/A
        // rather than a misleading snapshot.
        {
          String msg = "{\"level\":\"INFO\",\"code\":\"DISPLAY_PINS\",\"pins\":{";
          msg += "\"BL\":{\"gpio\":" + String(PIN_TFT_BL) + ",\"mode\":\"OUTPUT\",\"level\":" +
                 String(digitalRead(PIN_TFT_BL)) + "},";
          msg += "\"RST\":{\"gpio\":" + String(PIN_TFT_RST) + ",\"mode\":\"NOT_WIRED\",\"level\":\"n/a\"},";
          msg += "\"CS\":{\"gpio\":" + String(PIN_TFT_CS) +
                 ",\"mode\":\"SPI_LIBRARY_MANAGED\",\"level\":\"n/a (toggled per-transfer, "
                 "not held statically by this code)\"},";
          msg += "\"DC\":{\"gpio\":" + String(PIN_TFT_DC) +
                 ",\"mode\":\"SPI_LIBRARY_MANAGED\",\"level\":\"n/a (same as CS)\"},";
          msg += "\"SCLK\":" + String(PIN_TFT_SCLK) + ",\"MOSI\":" + String(PIN_TFT_MOSI) +
                 ",\"MISO\":" + String(PIN_TFT_MISO) + "}}";
          Serial.println(msg);
        }
      } else if (line == "display-reinit") {
        // DEV-only electrical re-init (§4): backlight OFF, (attempted)
        // hardware reset, SPI/display begin(), rotation/init, fill RED,
        // backlight ON. Deliberately does NOT re-run Display::init() (which
        // would re-allocate the PSRAM shadow framebuffer on top of the one
        // already allocated at boot) -- calls the same underlying
        // Adafruit_ILI9341 entry points directly instead. No reboot, no
        // business/UI code involved.
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"backlight_off\"}");
        digitalWrite(PIN_TFT_BL, LOW);
        delay(300);
#if PIN_TFT_RST >= 0
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"hardware_reset_pulse\"}");
        pinMode(PIN_TFT_RST, OUTPUT);
        digitalWrite(PIN_TFT_RST, LOW);
        delay(20);
        digitalWrite(PIN_TFT_RST, HIGH);
        delay(150);
#else
        // Honest limitation, not silently skipped: PIN_TFT_RST is -1 (not
        // wired) on this board -- there is no GPIO to pulse. Only a
        // SOFTWARE reset (SWRESET over SPI, inside begin() below) is
        // possible. If the panel controller itself is wedged in a state
        // only a real hardware reset can clear, this step cannot help.
        Serial.println("{\"level\":\"WARN\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"hardware_reset_pulse\","
                       "\"message\":\"SKIPPED -- PIN_TFT_RST is -1 (not wired), only software reset is possible\"}");
#endif
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"begin\"}");
        g_display.begin();
        g_display.setRotation(0);
        g_display.invertDisplay(true);
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"fill_red\"}");
        g_display.fillScreen(ILI9341_RED);
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"backlight_on\"}");
        digitalWrite(PIN_TFT_BL, HIGH);
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_DONE\"}");
        g_display.bump_frame_id();
      } else if (line == "backlight-test") {
        // §5: isolates the backlight GPIO/driver path from everything else
        // -- no SPI/display calls at all in this command, just the raw pin.
        Serial.println("{\"level\":\"INFO\",\"code\":\"BACKLIGHT_TEST_STEP\",\"step\":\"LOW\",\"hold_s\":2}");
        digitalWrite(PIN_TFT_BL, LOW);
        delay(2000);
        Serial.println("{\"level\":\"INFO\",\"code\":\"BACKLIGHT_TEST_STEP\",\"step\":\"HIGH\",\"hold_s\":5}");
        digitalWrite(PIN_TFT_BL, HIGH);
        delay(5000);
        Serial.println("{\"level\":\"INFO\",\"code\":\"BACKLIGHT_TEST_STEP\",\"step\":\"LOW\",\"hold_s\":2}");
        digitalWrite(PIN_TFT_BL, LOW);
        delay(2000);
        Serial.println("{\"level\":\"INFO\",\"code\":\"BACKLIGHT_TEST_STEP\",\"step\":\"HIGH\",\"hold_s\":\"final\"}");
        digitalWrite(PIN_TFT_BL, HIGH);
        Serial.println("{\"level\":\"INFO\",\"code\":\"BACKLIGHT_TEST_DONE\"}");
      } else if (line == "lcd-physical-test") {
        // Requested physical LCD verification sequence: every draw call
        // below goes through Display's overridden Adafruit_GFX methods,
        // which call the REAL Adafruit_ILI9341:: implementation first and
        // mirror into the shadow framebuffer second (see display.cpp) --
        // the shadow framebuffer is a side effect here, never the only
        // output. Backlight is forced HIGH before the sequence and left on.
        digitalWrite(PIN_TFT_BL, HIGH);

        auto centered_text = [](const char* text, uint8_t size, int16_t y, uint16_t color, uint16_t bg) {
          int16_t x1, y1;
          uint16_t w, h;
          g_display.setTextSize(size);
          g_display.setTextColor(color, bg);
          g_display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
          int16_t x = (g_display.width() - static_cast<int16_t>(w)) / 2;
          g_display.setCursor(x, y);
          g_display.print(text);
        };

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN1_TEST_LCD\"}");
        g_display.fillScreen(ILI9341_BLACK);
        centered_text("TEST LCD", 3, 140, ILI9341_WHITE, ILI9341_BLACK);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN2_WHITE\"}");
        g_display.fillScreen(ILI9341_WHITE);
        centered_text("WHITE SCREEN", 2, 140, ILI9341_BLACK, ILI9341_WHITE);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN3_RED\"}");
        g_display.fillScreen(ILI9341_RED);
        centered_text("RED", 3, 140, ILI9341_WHITE, ILI9341_RED);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN4_GREEN\"}");
        g_display.fillScreen(ILI9341_GREEN);
        centered_text("GREEN", 3, 140, ILI9341_BLACK, ILI9341_GREEN);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN5_BLUE\"}");
        g_display.fillScreen(ILI9341_BLUE);
        centered_text("BLUE", 3, 140, ILI9341_WHITE, ILI9341_BLUE);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN6_BORDER\"}");
        g_display.fillScreen(ILI9341_BLACK);
        g_display.drawRect(0, 0, g_display.width(), g_display.height(), ILI9341_WHITE);
        g_display.drawRect(4, 4, g_display.width() - 8, g_display.height() - 8, ILI9341_WHITE);
        centered_text("MESFLOW", 2, 110, ILI9341_WHITE, ILI9341_BLACK);
        centered_text("LCD OK", 2, 140, ILI9341_WHITE, ILI9341_BLACK);
        centered_text("1234567890", 2, 170, ILI9341_WHITE, ILI9341_BLACK);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"FINAL_HOLD\"}");
        g_display.fillScreen(ILI9341_BLACK);
        centered_text("LCD TEST", 2, 100, ILI9341_GREEN, ILI9341_BLACK);
        centered_text("IF YOU CAN READ THIS", 1, 140, ILI9341_GREEN, ILI9341_BLACK);
        centered_text("DISPLAY IS WORKING", 1, 160, ILI9341_GREEN, ILI9341_BLACK);
        // Left on indefinitely -- no further delay/redraw here.

        g_display.bump_frame_id();
        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_DONE\"}");
      }
      // Serial Visual Debug Fallback: the exact same three payloads the
      // HTTP /debug/* endpoints serve, over USB serial -- for when the host
      // and the device aren't on the same LAN/subnet (HTTP unreachable) but
      // a USB cable still reaches the device. Gated by MESFLOW_DEBUG_API
      // (not compiled into PROD) because g_debug_server itself only exists
      // under that guard; the underlying data these read is always present
      // regardless of profile, but there is deliberately only ONE builder
      // for each payload (DebugServer::build_ui_state_json() /
      // build_device_state_json() / write_screenshot_serial()), reused by
      // both HTTP and serial -- not a second hand-maintained copy.
#if MESFLOW_DEBUG_API
      else if (line == "debug-screenshot") {
        g_debug_server.write_screenshot_serial(Serial);
      } else if (line == "debug-ui-state") {
        g_debug_server.write_ui_state_serial(Serial);
      } else if (line == "debug-device-state") {
        g_debug_server.write_device_state_serial(Serial);
      } else if (line == "debug-net-diag") {
        kiosk::network::run_net_diag(Serial, g_wifi.reconnect_count());
      } else if (line == "debug-wifi-drop") {
        // §1 of the 2026-08-27 "Final Field-Readiness Verification" pass:
        // exercises the REAL WifiManager recovery path (poll()'s own
        // CONNECTED -> DISCONNECTED -> cooldown -> CONNECTING -> CONNECTED
        // cycle) without needing physical AP control. Does NOT touch
        // persisted Wi-Fi credentials (config_store/NVS) at all -- see
        // WifiManager::simulate_disconnect_for_test()'s own comment.
        g_wifi.simulate_disconnect_for_test();
        Serial.println("{\"level\":\"WARN\",\"code\":\"NET_WIFI_TEST_DROP_ARMED\",\"module\":\"kiosk_runtime_v2\","
                       "\"message\":\"radio disconnected for testing -- watch for the normal reconnect cycle\"}");
      } else if (line.startsWith("debug-input:")) {
        // Same SCAN/KEY_DOWN/KEY_UP JSON body as POST /debug/input, e.g.
        // debug-input:{"type":"SCAN","value":"00152"} -- needed to drive
        // real state transitions for visual-parity testing when there's no
        // HTTP path to the device and no physical scanner/keypad access.
        g_debug_server.write_input_result_serial(std::string(line.substring(12).c_str()), Serial);
      } else if (line == "force-task-create-failure") {
        // OBSOLETE as of the 2026-08-26 "eliminate recurrent server
        // connection failures" pass (§2-§5): there is no more per-call
        // xTaskCreate() to fail -- NetworkWorker's one task is created once,
        // at boot, by begin(). The scenario this command used to arm
        // (KioskRuntime::force_next_task_create_failure_for_test(), and its
        // whole retry-once -> controlled-reboot path) no longer exists.
        // Kept as a recognized-but-inert command (rather than silently
        // falling through to "unknown command") so an old
        // tools/kiosk_test_runner.py --scenario task_create_failure run
        // gets an honest answer instead of a confusing timeout.
        Serial.println("{\"level\":\"WARN\",\"code\":\"API_FAULT_OBSOLETE\",\"module\":\"kiosk_runtime_v2\","
                       "\"message\":\"force-task-create-failure is obsolete -- NetworkWorker has no "
                       "per-call task creation left to fail\"}");
      } else if (line.startsWith("simulate-compaction-crash:")) {
        // §10 of the 2026-08-25 follow-up: sets up the exact on-disk file
        // state a real crash would leave at one of compact()'s 5
        // interruption windows, then reboots so the NEXT boot's real
        // init() orphan-recovery logic is what actually recovers it (not
        // this test hook) -- see event_journal.h's own comment for the
        // scenario numbers.
        int scenario = line.substring(26).toInt();  // strlen("simulate-compaction-crash:") == 26
        Serial.printf("Simulating compaction-crash scenario %d, rebooting...\n", scenario);
        g_journal.simulate_compaction_crash_for_test(scenario);
        delay(200);
        ESP.restart();
      } else if (line == "reinit-scanner") {
        // §7: manual scanner re-init (see scanner_gm65.h's comment on why
        // this has no automatic trigger).
        g_scanner.reinit();
        Serial.println("{\"level\":\"INFO\",\"code\":\"HW_SCANNER_REINIT_REQUESTED\",\"module\":\"kiosk_runtime_v2\"}");
      } else if (line == "force-safe-mode") {
        // §9: forces the persisted same-fault streak to the SAFE_MODE
        // threshold and reboots -- verifies the REAL boot path (not just
        // code inspection) without waiting for 3 genuine faults.
        Serial.println("Forcing SAFE_MODE (TASK_CREATE_FAILED) and rebooting...");
        delay(200);
        kiosk::health::force_safe_mode_for_test(kiosk::health::RecoveryCode::TASK_CREATE_FAILED);
      }
#endif
      line = "";
    } else if (line.length() < 96) {
      line += c;
    }
  }
}

}  // namespace

void loop() {
  esp_task_wdt_reset();  // §21: feed the real hardware watchdog every iteration -- see setup()'s init comment
  g_scanner.poll();
  g_keypad.poll();
  g_wifi_recovery.tick();
  // The recovery portal drives Wi-Fi itself (AP+STA) while active; letting
  // wifi_manager also call WiFi.begin() at the same time would fight over
  // the radio, so pause normal reconnect attempts during recovery.
  if (!g_wifi_recovery.portal_active()) {
    g_wifi.poll();
  }
  poll_serial_provisioning();
  g_runtime.poll();  // non-blocking: picks up completed async event sends (§23)
#if MESFLOW_DEBUG_API
  g_debug_server.poll();
#endif

  // Network self-recovery -- see kNetworkStuckReconnectThreshold's own
  // comment above for the full context. NO reboot escalation: if the
  // streak keeps growing past the first reconnect, just keep reconnecting
  // every kNetworkStuckReconnectRepeatEvery failures, indefinitely -- the
  // device stays usable and keeps trying, exactly as this task requires.
  {
    uint32_t streak = g_runtime.consecutive_tcp_connect_fail();
    if (streak == 0) {
      g_last_reconnect_at_streak = 0;
    } else if (streak >= kNetworkStuckReconnectThreshold &&
              streak - g_last_reconnect_at_streak >=
                  (g_last_reconnect_at_streak == 0 ? kNetworkStuckReconnectThreshold
                                                   : kNetworkStuckReconnectRepeatEvery)) {
      g_last_reconnect_at_streak = streak;
      kiosk::health::record_recovery_event(
          kiosk::health::RecoveryCode::NETWORK_TIMEOUT,
          "sustained TCP_CONNECT_FAIL despite Wi-Fi CONNECTED -- forcing reconnect", 0, 0, 0);
      g_wifi.force_reconnect();
    }
  }

  if (g_wifi.state() == kiosk::network::WifiState::CONNECTED) {
    // §19: a reconnect (not the first-ever connect this boot) re-arms the
    // same bootstrap/ui-bundle-check machinery boot uses -- see
    // g_last_seen_reconnect_count's own comment above for why this was a
    // real gap.
    uint32_t reconnects = g_wifi.reconnect_count();
    if (reconnects != g_last_seen_reconnect_count) {
      g_last_seen_reconnect_count = reconnects;
      g_bootstrap_attempted = false;
      g_bootstrap_inflight = false;
      g_bootstrap_attempt_count = 0;
      g_bootstrap_next_attempt_ms = 0;
      kiosk::health::log_structured(
          "INFO", "NET_WIFI_RECONNECT_REBOOTSTRAP", "kiosk_runtime_v2",
          (std::string("reconnect_count=") + std::to_string(reconnects) +
           " -- re-arming bootstrap/ui-bundle-check")
              .c_str());
      // §17/§19: a reconnect is also exactly when a real offline backlog
      // (if any accumulated during the outage) should start draining.
      g_runtime.start_offline_replay_if_needed();
    }

    if (!g_time_sync_started) {
      g_time_sync_started = true;
      g_time_sync.begin();
    }
    g_time_sync.poll();

    // 2026-08-27 field report ("máy quét phải luôn sẵn sàng" -- the kiosk
    // must always be ready): this used to call g_bootstrap.attempt()
    // directly here, a single BLOCKING HTTPClient POST on this same loop()
    // thread -- up to RUNTIME_HTTP_TIMEOUT_MS per attempt, during which the
    // keypad/scanner were never polled at all. Every other request kind
    // already moved off loop()-blocking calls via NetworkWorker; this was
    // the one left over. Now enqueues through the SAME worker every other
    // request kind uses and picks the result up on a LATER loop() iteration
    // via take_bootstrap_result() -- g_bootstrap_inflight distinguishes
    // "already asked, waiting for the async result" from "done, or free to
    // try again", so this enqueue check never fires twice for the same
    // attempt.
    if (!g_bootstrap_attempted && !g_bootstrap_inflight && millis() >= g_bootstrap_next_attempt_ms) {
      String bootstrap_url = kiosk::network::derive_sibling_endpoint(g_config.api_endpoint(), "bootstrap");
      if (bootstrap_url.length() == 0) {
        // Can't even derive the endpoint -- same "count it as a real
        // attempt so this doesn't spin forever" treatment the old blocking
        // path gave an equivalent failure, just without ever touching the
        // network at all.
        ++g_bootstrap_attempt_count;
        kiosk::health::log_structured("ERROR", "BOOTSTRAP_REJECTED", "kiosk_runtime_v2",
                                      "could not derive /bootstrap endpoint from configured URL");
        if (g_bootstrap_attempt_count >= kBootstrapMaxAttempts) {
          g_bootstrap_attempted = true;
          kiosk::network::BootstrapResult failed;
          failed.status = kiosk::network::BootstrapStatus::FAILED;
          g_runtime.on_bootstrap_result(failed);
        } else {
          g_bootstrap_next_attempt_ms = millis() + kBootstrapRetryCooldownMs;
        }
      } else {
        std::string body = kiosk::network::BootstrapClient::build_request_body(
            g_identity.device_id(), g_identity.hardware_id(), g_diagnostics.boot_id,
            static_cast<uint32_t>(g_runtime.device_seq()));
#if MESFLOW_DEBUG_API
        kiosk::health::log_memory_snapshot("BEFORE_TLS_REQUEST");
#endif
        if (g_runtime.enqueue_bootstrap(bootstrap_url, body)) {
          ++g_bootstrap_attempt_count;
          g_bootstrap_inflight = true;
        }
        // If enqueue fails (HIGH-tier queue momentarily full, e.g. a
        // foreground scan just ahead of it), simply try again the very
        // next loop() iteration -- no attempt consumed, no cooldown, and
        // certainly no blocking wait either way.
      }
    }

    // Picked up on whatever LATER loop() iteration the async bootstrap
    // request actually completes on -- decoupled from the "should a NEW
    // attempt start" check above by design (see g_bootstrap_inflight).
    kiosk::network::NetworkResult bootstrap_net_result;
    if (g_runtime.take_bootstrap_result(bootstrap_net_result)) {
      g_bootstrap_inflight = false;
#if MESFLOW_DEBUG_API
      // These two snapshots now bracket the full async round trip (enqueue
      // to result), not a single blocking call the way they used to --
      // still useful as "cost of a bootstrap cycle", just a wall-clock
      // window instead of a pure CPU-blocked one.
      kiosk::health::log_memory_snapshot("AFTER_TLS_REQUEST");
      if (g_bootstrap_attempt_count == 1) kiosk::health::log_memory_snapshot("AFTER_FIRST_BOOTSTRAP");
#endif
      auto bootstrap_result = g_bootstrap.parse_response(bootstrap_net_result.outcome.http_status,
                                                         bootstrap_net_result.response_body);
      if (bootstrap_result.status == kiosk::network::BootstrapStatus::FAILED &&
          g_bootstrap_attempt_count < kBootstrapMaxAttempts) {
        // Transport-level failure (see the race documented above) -- retry
        // after a short cooldown rather than stranding the device on
        // state=null for the rest of this boot. A real response (success
        // OR a business rejection) falls through below and marks this done
        // immediately, same as before.
        g_bootstrap_next_attempt_ms = millis() + kBootstrapRetryCooldownMs;
        kiosk::health::log_structured(
            "INFO", "BOOTSTRAP_RETRY", "kiosk_runtime_v2",
            (std::string("attempt=") + std::to_string(g_bootstrap_attempt_count) + "/" +
             std::to_string(kBootstrapMaxAttempts))
                .c_str());
      } else {
        g_bootstrap_attempted = true;
      }
      // Phase 2: seed StateProjection from the SAME bootstrap response --
      // invariant 15, the device never restores a business state locally
      // across reboot, it always starts from server authority this boot.
      g_runtime.on_bootstrap_result(bootstrap_result);
      // §17/§19: also the FIRST-boot equivalent of the reconnect trigger
      // above -- the durable journal persists across reboots by design, so
      // a fresh boot can easily start with a real PENDING backlog from
      // before a crash/power-loss, not only from a mid-session Wi-Fi drop.
      if (bootstrap_result.status == kiosk::network::BootstrapStatus::OK) {
        g_runtime.start_offline_replay_if_needed();
      }
      // Phase 4: check whether the server wants a different UI bundle than
      // whatever's currently active -- read once at boot (heartbeat could
      // also carry this for faster propagation without a reboot; not
      // implemented yet, see the Phase 4 report's Known Gaps).
      //
      // §2 of the 2026-08-25 follow-up: SAFE_MODE explicitly avoids "UI
      // bundle sync unless needed" -- bootstrap/resync itself (the "basic
      // backend state/resync" minimal subsystem) still runs above
      // regardless of SAFE_MODE, only this extra network round-trip is
      // skipped.
      if (!kiosk::health::is_safe_mode()) {
        g_ui_sync.check_desired(g_config.api_endpoint(), bootstrap_result.ui_bundle_version,
                               std::string(bootstrap_result.ui_bundle_hash.c_str()));
      }
    }
    // §2: heartbeat is a "nonessential worker" during SAFE_MODE -- health
    // telemetry has no value to an operator trying to recover a stuck
    // kiosk, and it's one more thing this loop doesn't need to spend time
    // on while minimal.
    if (!kiosk::health::is_safe_mode()) {
      g_heartbeat.poll(g_config.api_endpoint());
    }
  }
  // non-blocking: picks up a completed bundle download/verify/activate
  // (§23/§24). A REAL bug caught live via the Serial Visual Debug Fallback
  // tooling: activating a new bundle alone does NOT redraw the currently
  // displayed screen -- nothing else triggers a redraw for a bundle switch
  // with no underlying business-state transition, so the operator would
  // keep looking at stale content until some unrelated event (scan/resync/
  // keypress) happened to redraw. refresh_idle_screen() when poll()
  // reports an activation closes that gap. Skipped entirely in SAFE_MODE
  // (check_desired() above never runs there, so poll() would never have
  // anything to report anyway).
  if (!kiosk::health::is_safe_mode() && g_ui_sync.poll()) {
    g_runtime.refresh_idle_screen();
  }

  unsigned long now = millis();
  // §1 (2026-08-25 follow-up): the ROUTINE periodic trigger uses the
  // INCREMENTAL compact_tick() API, not the one-shot compact() -- a full
  // compaction measured ~4.2s blocking the main loop (and therefore
  // scanner/keypad/display polling) on real hardware. compact_tick() below
  // runs whenever a compaction is already in progress (every loop()
  // iteration, cheap no-op otherwise); begin_compaction() only starts a new
  // one on the 30s check, same as before.
  if (g_journal.compaction_active()) {
    g_journal.compact_tick();
    g_compaction_was_active = true;
  } else {
    if (g_compaction_was_active) {
      // Just finished (this tick or an earlier one moved active_ to false)
      // -- log the AFTER snapshot exactly once, matching where the OLD
      // one-shot call used to log it.
      g_compaction_was_active = false;
#if MESFLOW_DEBUG_API
      kiosk::health::log_memory_snapshot("AFTER_JOURNAL_COMPACTION");
#endif
    }
    if (now - g_last_compaction_check_ms >= kCompactionCheckIntervalMs) {
      g_last_compaction_check_ms = now;
      if (g_journal.should_consider_compaction()) {
#if MESFLOW_DEBUG_API
        kiosk::health::log_memory_snapshot("BEFORE_JOURNAL_COMPACTION");
#endif
        g_journal.begin_compaction();
      }
    }
  }

  // §11 of the self-recovery task -- low-memory supervisor, on its OWN
  // ~1s cadence (see kLowMemoryCheckIntervalMs's own comment for the real
  // incident this decoupling fixes -- it used to only run once per
  // kDiagnosticsPrintIntervalMs/30s, far too slow to catch per-event SRAM
  // pressure under any reasonably fast scan cadence). Deliberately reading
  // INTERNAL SRAM specifically (not diagnostics.largest_free_block_bytes
  // below, which is MALLOC_CAP_8BIT -- internal+PSRAM blended, the exact
  // thing that looked deceptively abundant during the earlier fragmentation
  // investigation). PROD+DEV both -- this is a safety feature, not a debug
  // tool.
  if (now - g_last_low_memory_check_ms >= kLowMemoryCheckIntervalMs) {
    g_last_low_memory_check_ms = now;
    uint32_t internal_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    uint32_t internal_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    auto mem_level = kiosk::health::classify_low_memory(internal_largest);
    if (mem_level == kiosk::health::LowMemoryLevel::WARNING) {
      // Real bug found live (2026-08-26, 300-scan stress test): compaction
      // used to only ever start once CRITICAL was ALREADY hit -- by then,
      // under any sustained scan rate, a real backlog had already built up
      // (retention trims to a handful of records once compaction actually
      // runs, but nothing started that process early). Starting it here,
      // at the EARLIER WARNING threshold, gives the incremental
      // compact_tick() path (already running every loop() iteration
      // whenever active) a real head start before things get critical,
      // instead of only reacting after the fact.
      if (!g_journal.compaction_active() && g_journal.record_count() > 0) {
        g_journal.begin_compaction();
      }
      kiosk::health::record_recovery_event(kiosk::health::RecoveryCode::LOW_MEMORY,
                                           "internal SRAM WARNING -- largest contiguous block low",
                                           static_cast<uint8_t>(g_journal.pressure()), internal_free,
                                           internal_largest);
    } else if (mem_level == kiosk::health::LowMemoryLevel::CRITICAL) {
      // Real bug found live (2026-08-26, 300-scan stress test): this used
      // to call g_journal.compact() -- the BLOCKING, one-shot compaction --
      // right here. That function measured ~4.7s wall-clock to finish on
      // real hardware (this codebase's OWN routine 30s-timer compaction
      // path already knew this and deliberately uses the INCREMENTAL
      // begin_compaction()+compact_tick() pair instead, specifically
      // because "a full compaction measured ~4.2s blocking the main loop
      // (and therefore scanner/keypad/display polling)" -- see that code's
      // own comment above). Calling the blocking version here meant the
      // ONE moment memory pressure is already critical was ALSO the moment
      // this froze the entire loop() -- including the scanner/keypad
      // polling and serial command processing that would otherwise let
      // pressure ease -- for multiple seconds, worsening exactly the
      // problem it was trying to fix. Confirmed live: the device rebooted
      // repeatedly even immediately AFTER a compaction that successfully
      // shrank the record count, because the blocking call itself was part
      // of the pressure.
      //
      // Fixed to start (or let continue) the SAME incremental compaction
      // the routine path already uses -- compact_tick() above already runs
      // every loop() iteration whenever compaction_active(), so this only
      // needs to START one if none is running yet. Escalating to a
      // controlled reboot is now PATIENT: only after CRITICAL has persisted
      // for kCriticalRebootPatienceChecks consecutive ~1s checks WHILE
      // compaction has had a real chance to run, not immediately upon the
      // first CRITICAL reading (§22: "watchdog/reboot should be last
      // resort, not normal memory management").
      if (!g_journal.compaction_active() && g_journal.record_count() > 0) {
        g_journal.begin_compaction();
      }
      ++g_consecutive_critical_low_memory_checks;
      if (g_consecutive_critical_low_memory_checks >= kCriticalRebootPatienceChecks) {
        kiosk::health::request_controlled_reboot(
            kiosk::health::RecoveryCode::LOW_MEMORY,
            "internal SRAM stayed CRITICAL despite incremental journal compaction -- no more application-level relief available",
            static_cast<uint8_t>(g_journal.pressure()), internal_free, internal_largest);
        // never returns.
      }
      kiosk::health::record_recovery_event(kiosk::health::RecoveryCode::LOW_MEMORY,
                                           "internal SRAM CRITICAL -- incremental journal compaction running",
                                           static_cast<uint8_t>(g_journal.pressure()), internal_free,
                                           internal_largest);
    } else {
      g_consecutive_critical_low_memory_checks = 0;
    }
  }

  if (now - g_last_diagnostics_print_ms >= kDiagnosticsPrintIntervalMs) {
    g_last_diagnostics_print_ms = now;
    kiosk::runtime::refresh_memory_fields(g_diagnostics);
    Serial.printf("{\"level\":\"INFO\",\"code\":\"HEARTBEAT_LOCAL\",\"uptime_ms\":%lu,"
                  "\"free_heap\":%u,\"largest_block\":%u,\"psram_free\":%u,"
                  "\"psram_low\":%s}\n",
                  now, g_diagnostics.free_heap_bytes,
                  g_diagnostics.largest_free_block_bytes,
                  g_diagnostics.psram_free_bytes,
                  kiosk::runtime::is_psram_headroom_low(g_diagnostics) ? "true" : "false");
#if MESFLOW_DEBUG_API
    kiosk::health::log_memory_snapshot("PERIODIC_30S");
    char stack_msg[64];
    snprintf(stack_msg, sizeof(stack_msg), "task=loopTask high_water_words=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    kiosk::health::log_structured("INFO", "TASK_STACK_DIAG", "kiosk_runtime_v2", stack_msg);
#endif

    // §13: uptime has been stable through a full periodic-check cycle with
    // no recovery reboot needed -- safe to clear any persisted same-fault
    // streak from a PAST boot's incident so it doesn't eventually
    // accumulate into a false SAFE_MODE trip from sparse, unrelated events
    // months apart. A genuine rapid reboot LOOP never reaches this line (it
    // crashes/reboots well before kDiagnosticsPrintIntervalMs's first tick).
    kiosk::health::recovery_supervisor_mark_stable();

    // §6: UI stall detection (see g_last_seen_frame_id's own comment above
    // for the honest scope/limitation).
    uint32_t frame_id = g_display.frame_id();
    if (frame_id != g_last_seen_frame_id) {
      g_last_seen_frame_id = frame_id;
      g_last_render_progress_ms = now;
      g_ui_stall_redraw_attempted = false;
      g_ui_stall_display_reinit_attempted = false;
    } else if (now - g_last_render_progress_ms >= kUiStallThresholdMs) {
      if (!g_ui_stall_redraw_attempted) {
        g_ui_stall_redraw_attempted = true;
        kiosk::health::record_recovery_event(kiosk::health::RecoveryCode::UI_STALL,
                                             "no render in 5min -- forcing one redraw",
                                             static_cast<uint8_t>(g_journal.pressure()), 0, 0);
        g_runtime.refresh_idle_screen();
      } else if (!g_ui_stall_display_reinit_attempted) {
        g_ui_stall_display_reinit_attempted = true;
        kiosk::health::record_recovery_event(kiosk::health::RecoveryCode::UI_STALL,
                                             "forced redraw didn't advance frame_id -- reinitializing display",
                                             static_cast<uint8_t>(g_journal.pressure()), 0, 0);
        g_display.init();
        g_runtime.refresh_idle_screen();
      } else {
        // Forced redraw AND a display reinit both failed to advance
        // frame_id -- genuinely unrecoverable at this layer.
        uint32_t mem_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        uint32_t mem_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        kiosk::health::request_controlled_reboot(
            kiosk::health::RecoveryCode::UI_STALL,
            "display reinit did not recover rendering -- unrecoverable at this layer",
            static_cast<uint8_t>(g_journal.pressure()), mem_free, mem_largest);
        // never returns.
      }
    }
  }

  // Real, confirmed root cause of a periodic reboot found live (2026-08-26,
  // "quét thẻ ... đang kiểm tra rất lâu ... rồi reload lại wifi" field
  // report): loop() ran with NO cooperative yield at all -- every iteration
  // of this function returns immediately back into Arduino's own loopTask,
  // which itself runs at priority 1 (tskIDLE_PRIORITY + 1), the SAME
  // priority the OLD AsyncEventSender's/HeartbeatClient's own per-call
  // worker tasks used (api_client.cpp's send_task_entry, heartbeat_client
  // .cpp's heartbeat_task_entry -- both ended with vTaskDelete(nullptr), a
  // SELF-deletion). FreeRTOS documents that a task's own stack/TCB cannot
  // be freed while it's still the one executing -- that memory is only
  // actually reclaimed later, by the IDLE task (priority 0, strictly
  // BELOW 1) running and doing the cleanup. With loopTask never yielding,
  // and worker tasks spawned on every scan (plus every 20s for heartbeat)
  // also sitting at priority 1, the priority-0 idle task on whichever core
  // these land on could be starved of scheduling time for long stretches --
  // self-deleted tasks piled up unreclaimed, internal SRAM dropped
  // (confirmed live: int_free fell from ~180KB to ~9KB over ~110s of
  // repeated scans, while int_largest independently collapsed to 6644
  // bytes -- just under a new task's required stack allocation), and the
  // next xTaskCreate() call failed.
  //
  // The 2026-08-26 "eliminate recurrent server connection failures" pass
  // (§2-§5) removed the per-call task pattern entirely -- NetworkWorker
  // creates exactly ONE task, once, at boot (g_network_worker.begin() in
  // setup()), so there is no more per-scan/per-heartbeat task creation left
  // to starve the idle task or fail. This vTaskDelay(1) is kept anyway: it
  // is still the correct, standard, minimal-risk cooperative yield for
  // loopTask relative to NetworkWorker's own persistent task and any other
  // equal-priority task on this chip, and costs nothing next to this loop's
  // own per-iteration work.
  vTaskDelay(1);
}
