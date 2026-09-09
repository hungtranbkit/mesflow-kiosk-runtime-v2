#pragma once

#include <Arduino.h>

#include "../hardware/display.h"
#include "../protocol/environment_label.h"  // Environment -- see draw_server_mismatch_screen
#include "../protocol/state_projection.h"  // StateSnapshot/ViewModel -- see draw_business_state
#include "../protocol/ui_bundle.h"         // UiScreen -- see draw_from_bundle
#include "../runtime/boot_diagnostics.h"
#include "../security/device_identity.h"  // ProvisioningState -- see draw_identity_screen

namespace kiosk::ui {

// Fixed set of hardcoded screens for Phase 0. Deliberately built from the
// same component vocabulary (label/value/status) that docs/UI_SCHEMA.md
// defines for the future bundle-driven renderer, so swapping this out later
// is additive rather than a rewrite. There is no bundle/schema/condition
// engine here yet — just direct draw calls.
// Deliberately not the network module's WifiState — the UI layer shouldn't
// depend on the network layer's types. UNKNOWN covers the brief window
// before the first WIFI_STATE event has ever arrived (e.g. right at boot).
enum class WifiIndicator {
  UNKNOWN,
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
};

class Renderer {
 public:
  explicit Renderer(kiosk::hardware::Display& display) : display_(display) {}

  void draw_boot_screen(const kiosk::runtime::BootDiagnostics& diagnostics,
                        bool scanner_ok, bool keypad_ok);

  // A persistent Wi-Fi status line is drawn on every "normal" screen below
  // (waiting/scan feedback/scan result) — a scan failure needs to be
  // visibly distinguishable as "no Wi-Fi at all" vs. "Wi-Fi is fine, the
  // backend/server didn't respond", not just a single generic error line.
  void draw_waiting_screen(WifiIndicator wifi);

  // §12: immediate LOCAL presentation feedback only — never an optimistic
  // BUSINESS confirmation. Called the instant a SCAN local event arrives,
  // before any network round trip.
  void draw_scan_received(const String& raw_code, WifiIndicator wifi);

  // Called once the network attempt resolves (or fails) — still not a
  // business confirmation, just "here's what happened when we asked".
  // §45: operator message (status_text) and technical error code are shown
  // as two separate lines, never merged into one string — an operator can
  // read the plain-language line and still report/track the exact code
  // (e.g. "HTTP_ERROR http=500") without needing a laptop on serial.
  // error_code == "" means don't show a code line (the OK case).
  void draw_scan_result(const String& raw_code, const String& status_text,
                        const String& error_code, WifiIndicator wifi);

  // --- Keypad calibration (docs/HARDWARE.md) ---
  void draw_keypad_calibration_prompt(char key, uint8_t index, uint8_t total);

  // --- Wi-Fi recovery (docs/WIFI_RECOVERY.md) ---
  // These are part of the "ESP32 built-in" emergency screen set: they must
  // render correctly even with no backend and no valid UI bundle, so they
  // are hardcoded here rather than driven by any bundle/theme mechanism.

  // seconds_held: 0 until the hold-progress threshold is reached, so callers
  // can call this every tick without flooding the display before anything
  // should show. 0 means "don't draw anything yet".
  void draw_wifi_hold_progress(int seconds_held);

  void draw_wifi_portal_active(const String& ssid);
  void draw_wifi_portal_testing(const String& ssid);
  void draw_wifi_portal_failed(const String& message);

  // --- Self-recovery UI (2026-08-25 finish-anti-stuck-recovery follow-up) ---
  // Both are ESP32-built-in emergency screens too, same reasoning as the
  // Wi-Fi ones above: hardcoded, never backend/bundle-driven, must render
  // with no server and no valid UI bundle.

  // §3: persistent screen shown for the ENTIRE SAFE_MODE boot (KioskRuntime
  // gates all normal rendering behind kiosk::health::is_safe_mode(), see
  // render_current_business_state()) -- reason_code is whatever
  // kiosk::health::safe_mode_reason() returned (a RecoveryCode string, e.g.
  // "RECOVERY_TASK_CREATE_FAILED"), "" if none persisted.
  // (2026-09-09) set_current_environment()/current_environment_ used to live
  // here, feeding the status bar's DEV/TEST/PROD label. The header now shows
  // the brand instead (see draw_status_bar()), which left that setter with
  // no reader at all -- removed rather than kept as write-only state, the
  // same call this codebase already made for RUNTIME_HTTP_HARD_DEADLINE_MS
  // and friends. Environment is still rendered where it's acted on:
  // draw_device_info_screen() and draw_server_mismatch_screen(), both of
  // which take it as an explicit parameter and never needed the cache.

  // §6/§18 (2026-08-26 UX-hardening pass): offline queue count, cached as a
  // member and read implicitly by draw_status_bar() rather than threaded
  // through all 16 of its call sites. 0 means "nothing pending" -- the
  // status bar only shows the
  // "Q:N" suffix when this is nonzero, so a healthy device's status bar
  // never grows a permanent "Q:0" nobody needs to see.
  void set_offline_queue_size(uint32_t n) { offline_queue_size_ = n; }

  // Operator wall clock in the status bar, "HH:MM" local time, or "" when
  // the clock isn't trusted (TimeSync::local_hhmm()'s §17 gate) -- same
  // cache-as-member/read-implicitly shape as the two setters above, for the
  // same reason: draw_status_bar() already runs on every screen, threading
  // a time parameter through all 16 call sites would buy nothing.
  //
  // The renderer never reads the clock itself: TimeSync owns the "is this
  // time trustworthy" decision, and the UI layer must not be able to draw a
  // confident-looking wall time the network layer wouldn't stand behind.
  void set_clock_text(const String& hhmm) { clock_text_ = hhmm; }

  void draw_safe_mode_screen(const String& reason_code, WifiIndicator wifi);

  // §3 (2026-08-26 UX-hardening pass): takes over the whole screen, same
  // precedence as SAFE_MODE -- see kiosk_runtime.cpp's
  // render_current_business_state() for why. No footer/dismiss action is
  // offered on purpose (§3: must not silently resume); it only clears when
  // KioskRuntime's own apply_server_environment() sees a later bootstrap
  // that no longer disagrees.
  void draw_server_mismatch_screen(kiosk::protocol::Environment expected, kiosk::protocol::Environment actual,
                                   WifiIndicator wifi);

  // §4: the local recovery menu opened by holding '*' ~5s
  // (WifiRecoveryController). Fixed 5 options, no submenu, no per-item
  // dynamic content beyond the header -- deliberately simple per the task's
  // own "keep it simple, no fancy layout work".
  void draw_recovery_menu(WifiIndicator wifi);

  // §4 (2026-08-26 UX-hardening pass): Device Info screen, reachable from
  // the recovery menu's new "6" option. Pure data display -- no secrets/
  // tokens among these params (§4's explicit rule; the caller must never
  // pass one). "" for any not-yet-known string field (never fabricated),
  // rssi=0/offline_queue=0 are real, honest zero values, not sentinels.
  void draw_device_info_screen(kiosk::protocol::Environment environment, const String& server_endpoint,
                               const String& server_version, const String& device_id,
                               const String& hardware_id, const String& firmware_version,
                               const String& wifi_ssid, const String& wifi_ip, int wifi_rssi, bool api_online,
                               const String& last_sync_iso, uint32_t offline_queue, WifiIndicator wifi);

  // --- Device identity / provisioning (§5/§36) ---
  // Shown as the persistent idle screen whenever provisioning_state is not
  // ACTIVE, in place of the normal waiting screen -- an unprovisioned or
  // revoked kiosk shows this, not a business-ready "San sang quet ma".
  // hardware_id is shown (not device_id, which may not exist yet) so
  // whoever is provisioning the unit can identify which physical device
  // this is. Still shows the Wi-Fi indicator -- recovery must stay usable
  // regardless of provisioning state (§40).
  void draw_identity_screen(kiosk::security::ProvisioningState state, const String& hardware_id,
                            WifiIndicator wifi);

  // --- Phase 2: server-authoritative business state (invariants 13-16) ---
  // The device renders EXACTLY what StateProjection currently holds -- these
  // methods never decide a transition, they only lay out whatever fields
  // `snapshot.view` already has (a field the server didn't send is simply
  // left blank, never guessed/defaulted to something plausible-looking).
  //
  // `transient_message`/`is_error` let the caller surface a business
  // rejection (e.g. EMPLOYEE_NOT_FOUND) or a benign note on top of the
  // CURRENT state screen, without a separate screen id per error code.
  // Covers WAIT_EMPLOYEE/WAIT_OPERATION/SESSION_ACTIVE/DEVICE_DISABLED/
  // MAINTENANCE/UNSUPPORTED -- QUANTITY_INPUT has its own method below since
  // it also needs to show the local (transient, non-business) digit buffer.
  void draw_business_state(const kiosk::protocol::StateSnapshot& snapshot,
                           const String& transient_message, bool is_error, WifiIndicator wifi);

  // `local_digit_buffer`: what the operator has typed so far THIS boot,
  // before pressing '#' -- purely local/transient UI state (§14: never a
  // business decision), cleared on submit or state change away from
  // QUANTITY_INPUT.
  void draw_quantity_input_screen(const kiosk::protocol::ViewModel& view,
                                  const String& local_digit_buffer,
                                  const String& transient_message, bool is_error,
                                  WifiIndicator wifi);

  // --- Phase 4.1: GOOD/DEFECT/REWORK quantity flow (local sub-steps within
  // server state QUANTITY_INPUT, see kiosk::runtime::QtyStep) ---
  // "SAN PHAM LOI" -- entered after GOOD is confirmed. good_so_far is shown
  // as a small reference line so the operator can see what they just
  // entered, same giant-digit visual weight as the GOOD screen.
  void draw_quantity_defect_screen(const kiosk::protocol::ViewModel& view,
                                   const String& local_digit_buffer, int32_t good_so_far,
                                   const String& transient_message, bool is_error, WifiIndicator wifi);
  // "LOI CO SUA DUOC KHONG?" -- only reached when defect_so_far > 0. Keys
  // 1=CO / 2=KHONG (handled in kiosk_runtime.cpp), not digit entry.
  void draw_rework_decision_screen(const kiosk::protocol::ViewModel& view, int32_t good_so_far,
                                   int32_t defect_so_far, const String& transient_message,
                                   bool is_error, WifiIndicator wifi);
  // "SO LUONG CAN SUA" -- defect_so_far shown as the visible cap reference
  // (validation itself happens in kiosk_runtime.cpp before this is ever
  // reached with an invalid value already submitted).
  void draw_quantity_rework_screen(const kiosk::protocol::ViewModel& view,
                                   const String& local_digit_buffer, int32_t defect_so_far,
                                   const String& transient_message, bool is_error, WifiIndicator wifi);
  // Real field report (2026-08-27): a review/confirm screen shown once
  // GOOD/DEFECT/REWORK are all collected, before the real submit fires --
  // see kiosk::runtime::QtyStep::SUMMARY's own comment for why. rework is
  // shown only when > 0 (DEFECT==0 and "not repairable" both leave it
  // meaninglessly 0 -- showing "Sửa: 0" in those cases would just be noise
  // next to values the operator never actually chose).
  void draw_quantity_summary_screen(const kiosk::protocol::ViewModel& view, int32_t good, int32_t defect,
                                    int32_t rework, const String& transient_message, bool is_error,
                                    WifiIndicator wifi);

  // Field report (2026-08-27): "khi quet op xong, nên co man hình tổng hợp
  // là tên gì, làm op gì... 5-10 giay gi do mới chuyen qua man hinh quet
  // thẻ" -- after a FINISH is actually accepted by the server (session
  // closed, device moved back to WAIT_EMPLOYEE), hold a plain confirmation
  // screen naming WHO just finished WHAT for a few seconds before the
  // normal card-scan screen takes over, instead of jumping back to
  // WAIT_EMPLOYEE instantly. employee_name/operation_code are captured by
  // the caller from the OUTGOING snapshot's view (the new WAIT_EMPLOYEE
  // snapshot carries neither) -- both plain strings, already resolved, no
  // ViewModel needed here.
  void draw_finish_result_screen(const String& employee_name, const String& operation_code, int32_t good,
                                 int32_t defect, int32_t rework, WifiIndicator wifi);

  // Shown while a STATE_CONFLICT response has told the device to re-fetch
  // authoritative state via GET /state (§8) -- deliberately not one of the
  // 6 canonical business states (it's a transient LOCAL condition, per
  // invariant 13/§4's RESYNCING note), so it gets its own screen rather than
  // living inside StateProjection.
  void draw_resyncing_screen(WifiIndicator wifi);

  // --- Phase 4.1: full-screen error presentation ---
  // Takes over the ENTIRE screen (not a one-line overlay) for an actual
  // error -- business rejection or network/backend failure -- matching
  // legacy's own visual weight (drawError(): red header, prominent title,
  // message body). is_network_error picks the title/framing so the two
  // classes stay visually AND textually distinct, never collapsed into one
  // generic "error" look (§3/§9 of the Phase 4.1 closure task). Does not
  // touch/invent any business state -- the caller (KioskRuntime) still owns
  // exactly when this is shown and when the next normal render call
  // replaces it, same dismiss mechanism the single-line transient_message
  // already used.
  void draw_error_view(const String& message, bool is_network_error, WifiIndicator wifi);

  // --- Phase 4: backend-managed, locally-cached, locally-rendered UI ---
  // Draws a UiScreen loaded from the active UI bundle (kiosk::storage::
  // UiBundleStore) -- component whitelist is TEXT/RECT/LINE only (see
  // ui_bundle.h). {{token}} placeholders in TEXT components are resolved
  // against `view` (server ViewModel) and `local_digit_buffer` (local-only,
  // never server data -- invariant 14).
  //
  // TRUE GEOMETRY (Phase 4 stabilization task): TEXT components draw at
  // their real x/y with their real font_size (Adafruit_GFX text-size
  // multiplier) via emit_component_text()/components() below -- no longer
  // quantized onto the fixed 10-row grid emit_line()/lines() still serves
  // the hardcoded (non-bundle) screens with. A bundle author is responsible
  // for not overlapping the reserved transient_message (~y=112-128) and
  // Wi-Fi indicator (~y=130-146) bands below, still drawn via the row grid.
  // RECT/LINE draw directly at their real coordinates too (unchanged).
  void draw_from_bundle(const kiosk::protocol::UiScreen& screen, const kiosk::protocol::ViewModel& view,
                        const String& local_digit_buffer, const String& transient_message, bool is_error,
                        WifiIndicator wifi);

  // --- Test-runner observability (DEV only, debug_server.cpp) ---
  // When active, every screen's Wi-Fi indicator line grows a small "[QA]"
  // suffix -- a person standing at the physical device can then tell at a
  // glance whether an automated test session currently has it, without
  // needing a laptop on serial. Pure observability: never read by any
  // business/rendering DECISION in this class, only appended to a line
  // that already draws unconditionally on every normal screen.
  void set_qa_active(bool active) { qa_active_ = active; }

  // --- Visual debug subsystem (docs/VISUAL_DEBUG.md) ---
  // Honest, minimal state: Phase 0 has no component/rect model (that's
  // Phase 4's bundle renderer, docs/UI_SCHEMA.md) -- this reports exactly
  // what actually gets drawn, a fixed set of text rows, not a fabricated
  // rect/font schema the renderer doesn't really have.
  static constexpr int kMaxLines = 10;
  struct DrawnLine {
    bool used = false;
    String text;
    uint16_t color = 0;
    bool overflow = false;    // measured width > available display width
    uint16_t measured_w = 0;  // from Display::getTextBounds, real font metrics
  };

  String current_screen_id() const { return screen_id_; }
  const DrawnLine* lines() const { return lines_; }

  // --- Phase 4 (true geometry): bundle-driven TEXT components drawn at
  // their REAL x/y/font_size instead of the fixed 10-row grid emit_line()
  // uses. Separate from lines_[] (which stays exactly as-is for hardcoded
  // screens) rather than retrofitting the row model to carry pixel
  // coordinates it was never designed for -- see draw_from_bundle().
  static constexpr int kMaxComponents = 16;
  struct DrawnComponent {
    bool used = false;
    int16_t x = 0, y = 0, w = 0, h = 0;
    uint8_t font_size = 1;
    String text;
    uint16_t color = 0;
    bool overflow = false;
    uint16_t measured_w = 0;
  };
  const DrawnComponent* components() const { return components_; }

 private:
  kiosk::hardware::Display& display_;
  String screen_id_ = "none";
  DrawnLine lines_[kMaxLines];
  DrawnComponent components_[kMaxComponents];
  int next_component_ = 0;
  bool qa_active_ = false;
  uint32_t offline_queue_size_ = 0;
  String clock_text_;  // "" until TimeSync trusts the clock -- see set_clock_text()

  // Draws a one-line Wi-Fi indicator at a fixed bottom row. Shared by every
  // "normal" screen so it's always visible, not something the operator has
  // to go dig for.
  void draw_wifi_indicator(WifiIndicator wifi);

  // --- Design-system helpers (2026-08-25 UI consistency cleanup, see
  // docs/KIOSK_UI_GUIDE.md) -- every draw_*screen() method above is built
  // ONLY from these plus begin_screen()/end_screen(). No numeric font size
  // or layout coordinate is picked at the call site of any of those.

  // Fixed header zone: WiFi signal bars + 4-char SSID prefix, same position
  // on every screen. draw_wifi_indicator() above now just delegates here.
  void draw_status_bar(WifiIndicator wifi);
  // One FONT_LARGE line, centered -- for a KNOWN short literal already
  // chosen to fit (not variable operator/server data -- see draw_fit_text).
  void draw_title_line(const String& text, int16_t y, uint16_t color);
  // Two fixed FONT_LARGE lines, centered as a block (the "QUÉT THẺ /
  // NHÂN VIÊN"-style primary instruction most workflow screens use).
  void draw_title_2line(const String& line1, const String& line2, int16_t y_top, uint16_t color);
  // Shared fit policy for variable-length content (employee/operation
  // names, server/error messages): LARGE-1-line -> SMALL-1-line ->
  // SMALL-2-line-wrap -> SMALL-2-line-wrap-with-ellipsis. Never a third
  // font size, never a silent clip. prefer_large=false skips straight to
  // the SMALL-first path (used for anything that was never meant to be the
  // screen's single dominant instruction, e.g. a raw scanned code).
  void draw_fit_text(const String& text, int16_t y_top, uint16_t color, bool prefer_large);
  // Field report (2026-09-08): the GOOD/DEFECT/REWORK_DECISION/REWORK/
  // SUMMARY quantity-entry screens showed only the Operation, never WHO is
  // entering -- on a shared kiosk, an operator had no on-screen way to
  // confirm "this is really my session" before typing numbers into it
  // (the exact scenario the same day's INTERRUPTED_QUANTITY_ENTRY backend
  // exception exists for). One combined "<employee_name> · <operation_
  // code>" line -- employee first (that's the actual new information),
  // operation_code (not the longer operation_name) so this stays ONE line
  // in the normal case, at the SAME y_top the operation-only line used to
  // occupy on every one of those screens -- no other layout on any of them
  // needed to move.
  void draw_identity_line(const kiosk::protocol::ViewModel& view, int16_t y_top, uint16_t color);
  // The one emphasized-size helper, for the giant quantity-VALUE digit only
  // -- still conceptually FONT_LARGE (see kFontValueScale's own comment in
  // renderer.cpp), just drawn with extra visual weight. No other call site
  // uses this.
  void draw_value_giant(const String& text, int16_t y, uint16_t color);
  // Consistent bottom action/hint row: SMALL only, fixed position, same
  // divider line on every screen that has one. Pass "" to skip a side.
  void draw_footer(const String& left, const String& right);

  // Starts a new logical screen: resets the tracked line/component state
  // and records its id for /debug/ui-state. Every draw_*screen method calls
  // this first.
  void begin_screen(const char* screen_id);
  // Draws one text row AND records it for /debug/ui-state, so the two can
  // never drift apart (no separate bookkeeping to forget to update).
  void emit_line(int row, const String& text, uint16_t color);
  // True-geometry counterpart for bundle-driven TEXT (see kMaxComponents
  // above): draws at the exact x/y with the given font_size (Adafruit_GFX
  // text-size multiplier), clamped/rejected if it would land outside the
  // physical screen. Records into components_[] for /debug/ui-state.
  void emit_component_text(int16_t x, int16_t y, const String& text, uint16_t color, uint8_t font_size);
  // Vietnamese-capable glyph blit (2026-08-24, replacing Adafruit_GFX's
  // stock ASCII-only default font for all component/measured text -- see
  // src/protocol/vn_font_core.h/src/ui/vn_font_data.h). Draws each UTF-8
  // codepoint via a binary-searched bitmap glyph, nearest-neighbor scaled
  // by font_size (same semantics as Adafruit_GFX's own setTextSize(N)) --
  // a codepoint with no glyph in the subset falls back to a blank
  // ascent/2-wide advance rather than drawing nothing at the wrong x
  // (matches measure_text_width()'s own fallback so measured and drawn
  // width never disagree). (x, y) is the top-left of the text, same
  // convention emit_component_text()'s callers already use.
  void draw_vn_text(int16_t x, int16_t y, const String& text, uint16_t color, uint8_t font_size);
  // Draws a variable-length operator-facing message (business rejection
  // reason, transient status) at font_size=2 for readability, falling back
  // to 1 if it would overflow the screen width at 2 -- same
  // try-then-step-down pattern emit_line() itself uses. transient_message
  // strings come from the SERVER (business rejection text) or this
  // runtime's own local messages, so length isn't bounded/known ahead of
  // time the way the fixed-format "label: number" strings elsewhere are.
  void emit_transient_message(int16_t x, int16_t y, const String& text, uint16_t color);
  // UI centering (Phase 4.1): measures a string's real pixel width at the
  // given font_size via Display::getTextBounds() -- the same real font
  // metrics emit_component_text() itself measures with, just callable
  // BEFORE drawing so draw_from_bundle()/hardcoded screens can compute a
  // true centered x from the RESOLVED (token-substituted) text, never a
  // guessed/precomputed-offline value. 0 for an empty string (getTextBounds
  // on "" is not meaningful -- same empty-string caveat as emit_line/
  // emit_component_text elsewhere in this file).
  uint16_t measure_text_width(const String& text, uint8_t font_size);
  // x for a string centered in the full display width at the given size.
  int16_t centered_x(const String& text, uint8_t font_size);
  // Shared word-wrap-into-at-most-2-lines-then-ellipsize algorithm, factored
  // out of draw_fit_text() (2026-08-25 bundle-text-clip fix) so
  // draw_from_bundle() can apply the exact same "never a silent clip"
  // policy to server-pushed centered text at whatever font_size its own
  // auto-shrink loop landed on, not just the hardcoded screens' fixed
  // kFontSmall. Splits at the last space that keeps line 1 within max_w;
  // with no such space (one unbreakable token wider than the screen),
  // line1=text/line2="" and line1 alone gets ellipsized. Both output lines
  // are ellipsized independently if they still don't fit after the split.
  // Single-line fit policy: return `text` unchanged if it already fits,
  // otherwise trim it character by character and append "..." until it
  // does. The ONE place that decides what a truncated string looks like --
  // wrap_and_ellipsize_two_lines() below and the fixed-row Device Info
  // screen both call it rather than each growing their own trimming loop
  // that could drift from the other.
  String ellipsize_to_width(const String& text, uint8_t font_size, int16_t max_w);

  void wrap_and_ellipsize_two_lines(const String& text, uint8_t font_size, int16_t max_w,
                                    String* out_line1, String* out_line2);
  // Call once a screen is fully drawn -- bumps Display's frame_id so a
  // debug capture can tell whether the screen changed between two requests.
  void end_screen();
};

}  // namespace kiosk::ui
