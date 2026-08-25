#pragma once

#include <Arduino.h>

#include "../hardware/display.h"
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
  void draw_safe_mode_screen(const String& reason_code, WifiIndicator wifi);

  // §4: the local recovery menu opened by holding '*' ~5s
  // (WifiRecoveryController). Fixed 5 options, no submenu, no per-item
  // dynamic content beyond the header -- deliberately simple per the task's
  // own "keep it simple, no fancy layout work".
  void draw_recovery_menu(WifiIndicator wifi);

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

  // Draws a one-line Wi-Fi indicator at a fixed bottom row. Shared by every
  // "normal" screen so it's always visible, not something the operator has
  // to go dig for.
  void draw_wifi_indicator(WifiIndicator wifi);

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
  // Call once a screen is fully drawn -- bumps Display's frame_id so a
  // debug capture can tell whether the screen changed between two requests.
  void end_screen();
};

}  // namespace kiosk::ui
