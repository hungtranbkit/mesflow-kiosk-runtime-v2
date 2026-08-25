#include "renderer.h"

#include <WiFi.h>

#include <cstdlib>

#include "../health/structured_log.h"
#include "../protocol/vn_font_core.h"
#include "vn_font_data.h"

namespace kiosk::ui {

namespace {
constexpr uint16_t kColorFg = ILI9341_WHITE;
constexpr uint16_t kColorAccent = ILI9341_GREEN;
constexpr uint16_t kColorWarn = ILI9341_YELLOW;
constexpr uint16_t kColorMuted = 0x9517;  // #94A3B8, matches the bundle palette's own muted tone
constexpr uint16_t kColorErr = 0xF1EB;    // #F43F5E, matches the bundle palette's own error/critical tone

// ==========================================================================
// Design system (2026-08-25 UI consistency cleanup). See docs/KIOSK_UI_GUIDE.md
// for the full rationale -- this is the ONE place any numeric layout/font
// value is allowed to live. No draw_*screen() function below picks its own
// font size, spacing, or content-zone coordinate.
// ==========================================================================

// ---- Screen geometry (fixed, physical: 2.8" 240x320 ILI9341) ----
constexpr int16_t kScreenW = 240;
constexpr int16_t kScreenH = 320;
constexpr int16_t kMarginX = 8;

// Three fixed zones, same on EVERY normal screen -- only the content zone's
// content changes between screens (§7/§25 of the task: header/footer
// geometry never moves).
constexpr int16_t kHeaderH = 24;                    // status bar: wifi icon + SSID prefix
constexpr int16_t kFooterH = 30;                    // hint area
constexpr int16_t kContentTop = kHeaderH;
constexpr int16_t kContentBottom = kScreenH - kFooterH;  // 290
constexpr int16_t kFooterY = kContentBottom + 8;         // 298 -- baseline for footer text
constexpr int16_t kFooterDividerY = kContentBottom - 4;  // 286

// ---- Spacing scale ----
constexpr int16_t kSpacingSmall = 4;
constexpr int16_t kSpacingNormal = 8;
constexpr int16_t kSpacingLarge = 16;

// ---- Typography: EXACTLY TWO roles for normal operator UI ----
// FONT_SMALL/FONT_LARGE are the only two values any draw_*screen() function
// below is allowed to pass to the shared helpers -- both map onto the
// existing 3-native-size Vietnamese font (src/ui/vn_font_data.h, 12/16/24px,
// see that file's own header for why 3 native sizes exist: bundle-declared
// content and the giant quantity-digit emphasis case still use the full
// range internally via select_vn_font(), but that's a font-LAYER mapping
// detail, not something screen code ever sees).
constexpr uint8_t kFontSmall = 2;  // -> kVnFontBody, 16px native, scale 1
constexpr uint8_t kFontLarge = 3;  // -> kVnFontLarge, 24px native, scale 1
// NOT a third typographic role -- the giant quantity-VALUE digit is still
// conceptually FONT_LARGE, just drawn with extra visual weight (native 24px
// glyph, block-scaled x2) via draw_value_giant() below, the one place this
// is used. No screen calls this scale directly.
constexpr uint8_t kFontValueScale = 6;  // select_vn_font(6) -> Large @ scale 2 (~48px)

constexpr int16_t kLineHeightSmall = 22;
constexpr int16_t kLineHeightLarge = 34;

// "#RRGGBB" -> RGB565. Malformed/short strings fall back to white rather
// than a garbage color or a crash -- a bundle author typo should produce a
// visibly-wrong-but-safe screen, not corrupt memory.
uint16_t parse_hex_color(const std::string& hex) {
  if (hex.size() != 7 || hex[0] != '#') return ILI9341_WHITE;
  auto hex_byte = [&](size_t pos) -> uint8_t {
    return static_cast<uint8_t>(strtol(hex.substr(pos, 2).c_str(), nullptr, 16));
  };
  uint8_t r = hex_byte(1), g = hex_byte(3), b = hex_byte(5);
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Selects a NATIVE glyph table (12/16/24px, matching v1's proven three-size
// strategy -- see vn_font_data.h's module comment) for a given font_size,
// plus the small integer block-scale still applied ON TOP of the 24px table
// for values above kFontLarge -- this is font-LAYER plumbing, not a
// screen-level typography role. Bundle content (draw_from_bundle()) can
// still declare any font_size 1-N (backward-compatible wire contract,
// unchanged); screen-level C++ code (every other draw_*screen() below)
// only ever passes kFontSmall/kFontLarge/kFontValueScale.
//
// font_size 1    -> Small (12px), scale 1 (kept for bundle back-compat only
//                   -- no hardcoded screen uses this after the 2026-08-25
//                   two-size cleanup)
// font_size 2    -> Body  (16px), scale 1 == kFontSmall
// font_size 3    -> Large (24px), scale 1 == kFontLarge
// font_size >=4  -> Large (24px), scale = round(font_size * 7 / 24), floor 1
//                   -- kFontValueScale(6) lands at scale 2 (~48px), the
//                   giant quantity-digit case
void select_vn_font(uint8_t font_size, const kiosk::protocol::VnFontData** out_font, uint8_t* out_scale) {
  const uint8_t size = font_size == 0 ? 1 : font_size;
  if (size == 1) {
    *out_font = &kiosk::ui::kVnFontSmall;
    *out_scale = 1;
  } else if (size == 2) {
    *out_font = &kiosk::ui::kVnFontBody;
    *out_scale = 1;
  } else if (size == 3) {
    *out_font = &kiosk::ui::kVnFontLarge;
    *out_scale = 1;
  } else {
    *out_font = &kiosk::ui::kVnFontLarge;
    uint16_t scaled = static_cast<uint16_t>((size * 7 + 12) / 24);  // round, not truncate
    *out_scale = scaled == 0 ? 1 : static_cast<uint8_t>(scaled);
  }
}
}  // namespace

void Renderer::begin_screen(const char* screen_id) {
  screen_id_ = screen_id;
  for (auto& line : lines_) {
    line.used = false;
    line.text = "";
    line.color = 0;
  }
  for (auto& comp : components_) {
    comp.used = false;
    comp.text = "";
    comp.color = 0;
  }
  next_component_ = 0;
  display_.clear(ILI9341_BLACK);
}

void Renderer::emit_line(int row, const String& text, uint16_t color) {
  // Legacy row-grid path -- still used by a handful of ESP32-built-in
  // emergency screens (boot, Wi-Fi recovery) that predate the fixed-zone
  // layout and don't need it (they're diagnostic/technical, not part of
  // the "normal operator UI" the two-size/fixed-zone rules apply to). Draws
  // through the same Vietnamese-capable glyph font as everything else.
  const int line_height = kLineHeightSmall;
  uint8_t line_font_size = kFontSmall;
  if (text.length() > 0 && measure_text_width(text, line_font_size) > display_.width() - 8) {
    line_font_size = 1;  // last-resort shrink for the few long diagnostic lines (boot screen), not a UI role
  }
  draw_vn_text(4, 4 + row * line_height, text, color, line_font_size);

  if (row < 0 || row >= kMaxLines) return;

  uint16_t w = 0;
  bool overflow = false;
  if (text.length() > 0) {
    w = measure_text_width(text, line_font_size);
    overflow = (4 + static_cast<int>(w)) > display_.width();
  }

  lines_[row].used = true;
  lines_[row].text = text;
  lines_[row].color = color;
  lines_[row].overflow = overflow;
  lines_[row].measured_w = w;

  if (overflow) {
    char msg[80];
    snprintf(msg, sizeof(msg), "screen=%s row=%d measured_w=%u available_w=%d",
             screen_id_.c_str(), row, w, display_.width() - 4);
    kiosk::health::log_structured("WARN", "UI_TEXT_OVERFLOW", "renderer", msg);
  }
}

uint16_t Renderer::measure_text_width(const String& text, uint8_t font_size) {
  if (text.length() == 0) return 0;  // same empty-string caveat this had with getTextBounds("")
  const kiosk::protocol::VnFontData* font = nullptr;
  uint8_t scale = 1;
  select_vn_font(font_size, &font, &scale);
  int32_t w = kiosk::protocol::measure_vn_text(*font, text.c_str(), scale);
  return w < 0 ? 0 : static_cast<uint16_t>(w);
}

void Renderer::draw_vn_text(int16_t x, int16_t y, const String& text, uint16_t color, uint8_t font_size) {
  if (text.length() == 0) return;
  const kiosk::protocol::VnFontData* font_ptr = nullptr;
  uint8_t scale = 1;
  select_vn_font(font_size, &font_ptr, &scale);
  const kiosk::protocol::VnFontData& font = *font_ptr;
  int16_t pen_x = x;
  const int16_t baseline = y + static_cast<int16_t>(font.ascent) * scale;
  const char* cursor = text.c_str();
  while (*cursor) {
    const uint32_t cp = kiosk::protocol::utf8_decode_next(&cursor);
    if (cp == 0) break;
    kiosk::protocol::VnGlyph glyph;
    if (kiosk::protocol::find_glyph(font, cp, &glyph)) {
      uint32_t bit = 0;
      for (uint8_t yy = 0; yy < glyph.height; ++yy) {
        for (uint8_t xx = 0; xx < glyph.width; ++xx, ++bit) {
          const uint8_t byte_val = font.bitmap[glyph.bitmap_offset + bit / 8];
          if (!(byte_val & (0x80 >> (bit & 7)))) continue;
          const int16_t px = pen_x + (static_cast<int16_t>(glyph.x_offset) + xx) * scale;
          const int16_t py = baseline + (static_cast<int16_t>(glyph.y_offset) + yy) * scale;
          if (scale == 1) {
            display_.drawPixel(px, py, color);
          } else {
            display_.fillRect(px, py, scale, scale, color);
          }
        }
      }
      pen_x += static_cast<int16_t>(glyph.advance) * scale;
    } else {
      // No glyph for this codepoint -- matches measure_vn_text()'s own
      // fallback exactly, so drawn and measured width never disagree.
      pen_x += (static_cast<int16_t>(font.ascent) / 2) * scale;
#if MESFLOW_DEBUG_API
      char msg[48];
      snprintf(msg, sizeof(msg), "missing glyph U+%04lX", static_cast<unsigned long>(cp));
      kiosk::health::log_structured("WARN", "UI_FONT_MISSING_GLYPH", "renderer", msg);
#endif
    }
  }
}

void Renderer::emit_transient_message(int16_t x, int16_t y, const String& text, uint16_t color) {
  uint8_t size = kFontSmall;
  if (measure_text_width(text, size) > display_.width() - x - 4) {
    size = 1;  // last-resort shrink for an unusually long backend message, never a planned UI role
  }
  emit_component_text(x, y, text, color, size);
}

int16_t Renderer::centered_x(const String& text, uint8_t font_size) {
  uint16_t w = measure_text_width(text, font_size);
  int16_t x = static_cast<int16_t>((display_.width() - static_cast<int>(w)) / 2);
  return x < 0 ? 0 : x;  // never negative -- a too-wide string clips at the left edge, not off-screen
}

void Renderer::emit_component_text(int16_t x, int16_t y, const String& text, uint16_t color,
                                   uint8_t font_size) {
  if (x < 0 || y < 0 || x >= display_.width() || y >= display_.height()) {
    char msg[80];
    snprintf(msg, sizeof(msg), "screen=%s x=%d y=%d display=%dx%d", screen_id_.c_str(), x, y,
             display_.width(), display_.height());
    kiosk::health::log_structured("WARN", "UI_COMPONENT_OUT_OF_BOUNDS", "renderer", msg);
    return;
  }
  uint8_t size = font_size == 0 ? 1 : font_size;

  draw_vn_text(x, y, text, color, size);

  if (next_component_ >= kMaxComponents) return;  // silently drop past the cap -- logged once below
  DrawnComponent& comp = components_[next_component_++];
  comp.used = true;
  comp.x = x;
  comp.y = y;
  comp.font_size = size;
  comp.text = text;
  comp.color = color;

  // §7 (fixed 2026-08-25: measure with the font/scale ACTUALLY selected for
  // this call, not always Body -- using a fixed table here used to disagree
  // with what draw_vn_text() drew for anything other than font_size==2, a
  // real independently-drifting-measurement bug class this project has hit
  // before).
  bool overflow = false;
  uint16_t w = 0, h = 0;
  if (text.length() > 0) {
    const kiosk::protocol::VnFontData* font = nullptr;
    uint8_t scale = 1;
    select_vn_font(size, &font, &scale);
    w = measure_text_width(text, size);
    h = (static_cast<uint16_t>(font->ascent) + font->descent) * scale;
    overflow = (x + static_cast<int>(w)) > display_.width() || (y + static_cast<int>(h)) > display_.height();
  }
  comp.w = w;
  comp.h = h;
  comp.overflow = overflow;
  comp.measured_w = w;

  if (overflow) {
    char msg[96];
    snprintf(msg, sizeof(msg), "screen=%s x=%d y=%d measured_w=%u measured_h=%u display=%dx%d",
             screen_id_.c_str(), x, y, w, h, display_.width(), display_.height());
    kiosk::health::log_structured("WARN", "UI_TEXT_OVERFLOW", "renderer", msg);
  }
}

// ==========================================================================
// Shared render helpers (§26 of the task) -- every normal screen below is
// built ONLY from these plus begin_screen()/end_screen()/draw_status_bar().
// No screen-level draw_*screen() function calls draw_vn_text()/
// emit_component_text() with a raw numeric font size directly.
// ==========================================================================

void Renderer::draw_status_bar(WifiIndicator wifi) {
  // Graphical signal-bar icon, top-right corner. Three bars of increasing
  // height, filled = on. This WifiIndicator enum has no per-dBm signal data
  // (unlike legacy's RSSI-driven bar count), so it's a coarse approximation:
  // CONNECTED=3 bars, CONNECTING=1 bar, DISCONNECTED/UNKNOWN=0 bars (outline
  // only) -- honest about what's actually known, not a fabricated strength.
  int bars_on = wifi == WifiIndicator::CONNECTED ? 3 : wifi == WifiIndicator::CONNECTING ? 1 : 0;
  uint16_t bar_on_color = wifi == WifiIndicator::CONNECTED ? kColorAccent : kColorWarn;
  constexpr int16_t kIconRight = 234;
  constexpr int16_t kIconBaseY = kHeaderH - 4;  // 20 -- sits just above the header/content boundary
  constexpr int16_t kBarW = 5;
  constexpr int16_t kBarGapX = 7;
  for (int i = 0; i < 3; ++i) {
    int16_t bar_h = 4 + i * 4;
    int16_t x = kIconRight - (3 - i) * kBarGapX;
    int16_t y = kIconBaseY - bar_h;
    if (i < bars_on) {
      display_.fillRect(x, y, kBarW, bar_h, bar_on_color);
    } else {
      display_.drawRect(x, y, kBarW, bar_h, kColorMuted);
    }
  }
  if (wifi == WifiIndicator::DISCONNECTED) {
    display_.drawLine(kIconRight - 3 * kBarGapX, kIconBaseY - 12, kIconRight, kIconBaseY, kColorWarn);
  }

  // SSID prefix: first 4 printable characters only (deliberate privacy/
  // space bound, never the full SSID on the operator screen). "----" when
  // disconnected/empty. FONT_SMALL, same as every other secondary text --
  // no third size just because this lives in the header.
  String ssid = wifi == WifiIndicator::DISCONNECTED || wifi == WifiIndicator::UNKNOWN
                    ? String("") : WiFi.SSID();
  String prefix = ssid.length() > 0 ? ssid.substring(0, ssid.length() < 4 ? ssid.length() : 4)
                                     : String("----");
  uint16_t w = measure_text_width(prefix, kFontSmall);
  int16_t leftmost_bar_x = kIconRight - 3 * kBarGapX;
  int16_t x = leftmost_bar_x - kSpacingSmall - static_cast<int>(w);
  if (x < 0) x = 0;
  emit_component_text(x, kSpacingSmall, prefix, kColorMuted, kFontSmall);
}

// One-line FONT_LARGE, centered. For a KNOWN short literal (not variable
// operator/server data) that's already been chosen to fit -- see
// draw_title_fit() below for variable-length content.
void Renderer::draw_title_line(const String& text, int16_t y, uint16_t color) {
  emit_component_text(centered_x(text, kFontLarge), y, text, color, kFontLarge);
}

// Two fixed FONT_LARGE lines, centered as a block -- the "QUÉT THẺ /
// NHÂN VIÊN"-style primary instruction every normal workflow screen uses.
void Renderer::draw_title_2line(const String& line1, const String& line2, int16_t y_top, uint16_t color) {
  draw_title_line(line1, y_top, color);
  draw_title_line(line2, y_top + kLineHeightLarge, color);
}

// Shared word-wrap-into-at-most-2-lines-then-ellipsize algorithm (2026-08-25
// bundle-text-clip fix: factored out of draw_fit_text() so draw_from_bundle()
// can apply the same policy at an arbitrary font_size, not just kFontSmall).
// Splits at the last space that keeps line 1 within max_w; with no such
// space (one unbreakable token wider than the screen -- e.g. a RecoveryCode
// like "RECOVERY_TASK_CREATE_FAILED" with no spaces at all, caught live on
// real hardware during the 2026-08-25 UI cleanup), *out_line1 = text and
// *out_line2 stays empty -- there's nothing left to wrap to a second line.
// Both output lines are ellipsized independently afterward if they still
// don't fit (3+ lines' worth of real content, or a still-too-wide token).
void Renderer::wrap_and_ellipsize_two_lines(const String& text, uint8_t font_size, int16_t max_w,
                                            String* out_line1, String* out_line2) {
  String remaining = text;
  String line1, line2;
  int space_at = -1;
  for (unsigned int i = 0; i < remaining.length(); ++i) {
    if (remaining[i] == ' ') {
      String candidate = remaining.substring(0, i);
      if (measure_text_width(candidate, font_size) <= static_cast<uint16_t>(max_w)) {
        space_at = static_cast<int>(i);
      } else {
        break;
      }
    }
  }
  if (space_at >= 0) {
    line1 = remaining.substring(0, space_at);
    line2 = remaining.substring(space_at + 1);
  } else {
    line1 = remaining;
    line2 = "";
  }
  auto ellipsize_if_needed = [&](String& line) {
    if (line.length() == 0 || measure_text_width(line, font_size) <= static_cast<uint16_t>(max_w)) return;
    // Trim to fit + ellipsis, character by character (bounded: len(line) iterations max).
    while (line.length() > 1 &&
           measure_text_width(line + "...", font_size) > static_cast<uint16_t>(max_w)) {
      line = line.substring(0, line.length() - 1);
    }
    line += "...";
  };
  ellipsize_if_needed(line1);
  ellipsize_if_needed(line2);
  *out_line1 = line1;
  *out_line2 = line2;
}

// Shared fit policy (§9 of the task) for VARIABLE-length content (employee/
// operation names, server/error messages): try LARGE on one line, then
// SMALL on one line, then wrap to at most 2 SMALL lines (word boundary,
// never mid-glyph), then ellipsis on the second line if it still doesn't
// fit. Never a third font size, never a silent clip.
void Renderer::draw_fit_text(const String& text, int16_t y_top, uint16_t color, bool prefer_large) {
  if (text.length() == 0) return;
  const int16_t max_w = kScreenW - 2 * kMarginX;

  if (prefer_large && measure_text_width(text, kFontLarge) <= static_cast<uint16_t>(max_w)) {
    draw_title_line(text, y_top, color);
    return;
  }
  if (measure_text_width(text, kFontSmall) <= static_cast<uint16_t>(max_w)) {
    emit_component_text(centered_x(text, kFontSmall), y_top, text, color, kFontSmall);
    return;
  }
  String line1, line2;
  wrap_and_ellipsize_two_lines(text, kFontSmall, max_w, &line1, &line2);
  emit_component_text(centered_x(line1, kFontSmall), y_top, line1, color, kFontSmall);
  if (line2.length() > 0) {
    emit_component_text(centered_x(line2, kFontSmall), y_top + kLineHeightSmall, line2, color, kFontSmall);
  }
}

// The one place a quantity/count is shown at emphasized size -- still
// conceptually FONT_LARGE (see kFontValueScale's own comment above), not a
// new role. Digits only in practice, so no wrap/ellipsis policy needed.
void Renderer::draw_value_giant(const String& text, int16_t y, uint16_t color) {
  emit_component_text(centered_x(text, kFontValueScale), y, text, color, kFontValueScale);
}

// Consistent bottom action/hint row -- SMALL only, fixed position, same
// divider line, on every screen that has one. left/right may be "" to skip
// that side (e.g. a single centered hint instead of a two-key footer).
void Renderer::draw_footer(const String& left, const String& right) {
  display_.drawFastHLine(kMarginX, kFooterDividerY, kScreenW - 2 * kMarginX, kColorMuted);
  if (left.length() > 0) {
    emit_component_text(kMarginX, kFooterY, left, kColorMuted, kFontSmall);
  }
  if (right.length() > 0) {
    emit_component_text(kScreenW - measure_text_width(right, kFontSmall) - kMarginX, kFooterY, right,
                        kColorMuted, kFontSmall);
  }
}

void Renderer::end_screen() {
  display_.bump_frame_id();
}

// Kept for the ESP32-built-in emergency screens (boot/keypad calibration)
// that use the legacy row grid, not the fixed-zone layout -- delegates to
// draw_status_bar() so there's still only one WiFi-icon implementation.
void Renderer::draw_wifi_indicator(WifiIndicator wifi) { draw_status_bar(wifi); }

void Renderer::draw_boot_screen(const kiosk::runtime::BootDiagnostics& d,
                                 bool scanner_ok, bool keypad_ok) {
  // Diagnostic/technical screen, not part of the "normal operator UI" the
  // two-size/fixed-zone rules apply to (§1 of the task lists it separately
  // from the workflow screens) -- kept on the legacy row grid, English
  // technical fields as before.
  begin_screen("boot");
  char line[64];

  emit_line(0, "MESFlow Kiosk Runtime v2", kColorAccent);

  snprintf(line, sizeof(line), "fw %s", d.fw_version.c_str());
  emit_line(1, line, kColorFg);
  emit_line(2, d.build_id.c_str(), kColorMuted);

  snprintf(line, sizeof(line), "%s x%d, flash %uMB", d.chip_model.c_str(),
           d.chip_cores, d.flash_mb);
  emit_line(3, line, kColorFg);

  snprintf(line, sizeof(line), "psram %u/%u KB free", d.psram_free_bytes / 1024,
           d.psram_total_bytes / 1024);
  emit_line(4, line, kColorFg);

  snprintf(line, sizeof(line), "heap %u KB, block %u KB", d.free_heap_bytes / 1024,
           d.largest_free_block_bytes / 1024);
  emit_line(5, line, kColorFg);

  snprintf(line, sizeof(line), "reset: %s", d.reset_reason.c_str());
  emit_line(6, line, kColorFg);

  snprintf(line, sizeof(line), "scanner: %s  keypad: %s",
           scanner_ok ? "OK" : "FAIL", keypad_ok ? "OK" : "DEGRADED");
  emit_line(7, line, scanner_ok ? kColorAccent : kColorWarn);

  end_screen();
}

void Renderer::draw_waiting_screen(WifiIndicator wifi) {
  begin_screen("waiting");
  draw_title_2line("SẴN SÀNG", "QUÉT MÃ", 130, kColorFg);
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_scan_received(const String& raw_code, WifiIndicator wifi) {
  begin_screen("scan_received");
  draw_title_line("ĐÃ NHẬN MÃ", 100, kColorAccent);
  draw_fit_text(raw_code, 160, kColorFg, /*prefer_large=*/false);
  emit_component_text(centered_x("Đang kiểm tra...", kFontSmall), 210, "Đang kiểm tra...", kColorMuted,
                      kFontSmall);
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_scan_result(const String& raw_code, const String& status_text,
                                const String& error_code, WifiIndicator wifi) {
  begin_screen("scan_result");
  draw_fit_text(status_text, 120, kColorAccent, /*prefer_large=*/true);
  draw_fit_text(raw_code, 190, kColorMuted, /*prefer_large=*/false);
  if (error_code.length() > 0) {
    emit_component_text(centered_x("Mã lỗi: " + error_code, kFontSmall), 230, "Mã lỗi: " + error_code,
                        kColorWarn, kFontSmall);
  }
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_keypad_calibration_prompt(char key, uint8_t index, uint8_t total) {
  // Technical/setup screen (real technician holding the device, not an
  // operator mid-shift) -- legacy row grid, same reasoning as boot screen.
  begin_screen("keypad_calibration");
  char line[32];
  emit_line(0, "HIỆU CHỈNH BÀN PHÍM", kColorAccent);
  snprintf(line, sizeof(line), "BẤM PHÍM %c", key);
  emit_line(2, line, kColorFg);
  snprintf(line, sizeof(line), "%u / %u", index + 1, total);
  emit_line(4, line, kColorFg);
  emit_line(6, "Bấm và thả từng phím", kColorFg);
  end_screen();
}

void Renderer::draw_wifi_hold_progress(int seconds_held) {
  if (seconds_held <= 0) return;
  begin_screen("wifi_hold_progress");
  draw_title_2line("TIẾP TỤC GIỮ *", "MỞ MENU", 130, kColorFg);
  end_screen();
}

void Renderer::draw_wifi_portal_active(const String& ssid) {
  begin_screen("wifi_portal_active");
  draw_title_line("CÀI ĐẶT WI-FI", 70, kColorAccent);
  emit_component_text(centered_x("Kết nối:", kFontSmall), 140, "Kết nối:", kColorFg, kFontSmall);
  draw_fit_text(ssid, 166, kColorAccent, /*prefer_large=*/false);
  emit_component_text(centered_x("Không mật khẩu", kFontSmall), 210, "Không mật khẩu", kColorMuted,
                      kFontSmall);
  emit_component_text(centered_x("Mở: 192.168.4.1", kFontSmall), 236, "Mở: 192.168.4.1", kColorMuted,
                      kFontSmall);
  end_screen();
}

void Renderer::draw_wifi_portal_testing(const String& ssid) {
  begin_screen("wifi_portal_testing");
  draw_title_line("ĐANG KIỂM TRA", 130, kColorFg);
  draw_fit_text(ssid, 190, kColorAccent, /*prefer_large=*/false);
  emit_component_text(centered_x("Vui lòng đợi...", kFontSmall), 230, "Vui lòng đợi...", kColorMuted,
                      kFontSmall);
  end_screen();
}

void Renderer::draw_wifi_portal_failed(const String& message) {
  begin_screen("wifi_portal_failed");
  draw_title_line("THẤT BẠI", 100, kColorWarn);
  draw_fit_text(message, 160, kColorFg, /*prefer_large=*/false);
  emit_component_text(centered_x("Mở 192.168.4.1 để thử lại", kFontSmall), 220,
                      "Mở 192.168.4.1 để thử lại", kColorMuted, kFontSmall);
  end_screen();
}

void Renderer::draw_safe_mode_screen(const String& reason_code, WifiIndicator wifi) {
  begin_screen("safe_mode");
  draw_title_line("KHÔI PHỤC", 90, kColorErr);
  draw_fit_text(reason_code.length() > 0 ? reason_code : String("Không xác định"), 150, kColorWarn,
               /*prefer_large=*/false);
  emit_component_text(centered_x("Đang tự thử lại...", kFontSmall), 210, "Đang tự thử lại...",
                      kColorMuted, kFontSmall);
  draw_footer("* menu", "giữ lâu: Wi-Fi");
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_recovery_menu(WifiIndicator wifi) {
  begin_screen("recovery_menu");
  emit_component_text(centered_x("MENU KHÔI PHỤC", kFontLarge), 40, "MENU KHÔI PHỤC", kColorAccent,
                      kFontLarge);
  // Menu list: all FONT_SMALL, left-aligned (§22: menu lists are the
  // explicit exception to center-first alignment), consistent spacing.
  constexpr int16_t kMenuX = 24;
  constexpr int16_t kMenuY0 = 110;
  const char* items[] = {"1  Thử lại mạng", "2  Đồng bộ lại", "3  Cài đặt Wi-Fi", "4  Quay lại",
                         "5  Khởi động lại"};
  for (int i = 0; i < 5; ++i) {
    emit_component_text(kMenuX, kMenuY0 + i * kLineHeightSmall, items[i], kColorFg, kFontSmall);
  }
  draw_footer("* giữ lâu hơn:", "Wi-Fi");
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_identity_screen(kiosk::security::ProvisioningState state,
                                    const String& hardware_id, WifiIndicator wifi) {
  begin_screen("identity");
  using kiosk::security::ProvisioningState;
  String title, detail;
  switch (state) {
    case ProvisioningState::UNPROVISIONED:
    case ProvisioningState::PROVISIONING:
      title = "CHƯA CẤU HÌNH";
      detail = "Cần provision device_id";
      break;
    case ProvisioningState::SUSPENDED:
      title = "TẠM DỪNG";
      detail = "Liên hệ quản trị viên";
      break;
    case ProvisioningState::REVOKED:
      title = "ĐÃ THU HỒI";
      detail = "Không thể kết nối backend";
      break;
    case ProvisioningState::ACTIVE:
      // Not expected to be called with ACTIVE (caller shows the normal
      // waiting screen instead) -- render something honest if it happens.
      title = "identity: ACTIVE";
      detail = "";
      break;
  }
  draw_title_line(title, 90, kColorWarn);
  if (detail.length() > 0) draw_fit_text(detail, 150, kColorFg, /*prefer_large=*/false);
  draw_fit_text(hardware_id, 190, kColorMuted, /*prefer_large=*/false);
  emit_component_text(centered_x("Giữ * 10s để cài Wi-Fi", kFontSmall), 236, "Giữ * 10s để cài Wi-Fi",
                      kColorMuted, kFontSmall);
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_business_state(const kiosk::protocol::StateSnapshot& s,
                                   const String& transient_message, bool is_error,
                                   WifiIndicator wifi) {
  using kiosk::protocol::BusinessState;

  switch (s.state) {
    case BusinessState::WAIT_EMPLOYEE:
      begin_screen("state_wait_employee");
      draw_title_2line("QUÉT THẺ", "NHÂN VIÊN", 130, kColorFg);
      break;

    case BusinessState::WAIT_OPERATION:
      begin_screen("state_wait_operation");
      draw_fit_text(s.view.has_employee_name ? String(s.view.employee_name.c_str()) : String("(nhân viên)"),
                   64, kColorAccent, /*prefer_large=*/false);
      draw_title_2line("QUÉT MÃ", "CÔNG ĐOẠN", 150, kColorFg);
      break;

    case BusinessState::SESSION_ACTIVE: {
      begin_screen("state_session_active");
      draw_title_line("ĐANG LÀM", 100, kColorAccent);
      String emp = s.view.has_employee_name ? String(s.view.employee_name.c_str()) : String("");
      String op = s.view.has_operation_name ? String(s.view.operation_name.c_str()) : String("");
      if (emp.length() > 0) draw_fit_text(emp, 160, kColorFg, /*prefer_large=*/false);
      if (op.length() > 0) draw_fit_text(op, 186, kColorMuted, /*prefer_large=*/false);
      emit_component_text(centered_x("Quét lại thẻ để kết thúc", kFontSmall), 230,
                          "Quét lại thẻ để kết thúc", kColorMuted, kFontSmall);
      break;
    }

    case BusinessState::DEVICE_DISABLED:
      begin_screen("state_device_disabled");
      draw_title_line("ĐÃ VÔ HIỆU HÓA", 130, kColorWarn);
      emit_component_text(centered_x("Liên hệ quản trị viên", kFontSmall), 190, "Liên hệ quản trị viên",
                          kColorFg, kFontSmall);
      break;

    case BusinessState::MAINTENANCE:
      begin_screen("state_maintenance");
      draw_title_line("ĐANG BẢO TRÌ", 140, kColorWarn);
      break;

    case BusinessState::QUANTITY_INPUT:
      // Not expected here -- caller uses draw_quantity_input_screen instead.
      // Render something honest rather than silently doing nothing.
      begin_screen("state_quantity_input_fallback");
      draw_title_line("THIẾU DỮ LIỆU", 140, kColorWarn);
      break;

    case BusinessState::UNSUPPORTED:
      begin_screen("state_unsupported");
      draw_title_line("KHÔNG HỖ TRỢ", 130, kColorWarn);
      emit_component_text(centered_x("Firmware có thể đã cũ", kFontSmall), 190,
                          "Firmware có thể đã cũ", kColorFg, kFontSmall);
      break;
  }

  if (transient_message.length() > 0) {
    draw_fit_text(transient_message, 254, is_error ? kColorWarn : kColorFg, /*prefer_large=*/false);
  }
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_quantity_input_screen(const kiosk::protocol::ViewModel& view,
                                          const String& local_digit_buffer,
                                          const String& transient_message, bool is_error,
                                          WifiIndicator wifi) {
  begin_screen("state_quantity_input");
  if (view.has_operation_name) {
    draw_fit_text(String(view.operation_name.c_str()), kContentTop + kSpacingSmall, kColorMuted,
                 /*prefer_large=*/false);
  }
  emit_component_text(centered_x("SỐ LƯỢNG TỐT", kFontSmall), 74, "SỐ LƯỢNG TỐT", kColorFg, kFontSmall);
  draw_value_giant(local_digit_buffer.length() > 0 ? local_digit_buffer : String("0"), 130, kColorAccent);
  if (transient_message.length() > 0) {
    draw_fit_text(transient_message, kFooterDividerY - kLineHeightSmall - kSpacingSmall,
                 is_error ? kColorWarn : kColorFg, /*prefer_large=*/false);
  }
  draw_footer("* XÓA", "# TIẾP");
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_quantity_defect_screen(const kiosk::protocol::ViewModel& view,
                                           const String& local_digit_buffer, int32_t good_so_far,
                                           const String& transient_message, bool is_error,
                                           WifiIndicator wifi) {
  begin_screen("state_quantity_defect");
  if (view.has_operation_name) {
    draw_fit_text(String(view.operation_name.c_str()), kContentTop + kSpacingSmall, kColorMuted,
                 /*prefer_large=*/false);
  }
  emit_component_text(centered_x("SỐ LƯỢNG LỖI", kFontSmall), 74, "SỐ LƯỢNG LỖI", kColorErr, kFontSmall);
  char ref[24];
  snprintf(ref, sizeof(ref), "Đạt: %ld", static_cast<long>(good_so_far));
  String ref_s(ref);
  emit_component_text(centered_x(ref_s, kFontSmall), 96, ref_s, kColorMuted, kFontSmall);

  String digits = local_digit_buffer.length() > 0 ? local_digit_buffer : String("0");
  draw_value_giant(digits, 140, kColorErr);

  if (transient_message.length() > 0) {
    draw_fit_text(transient_message, kFooterDividerY - kLineHeightSmall - kSpacingSmall,
                 is_error ? kColorErr : kColorFg, /*prefer_large=*/false);
  }
  draw_footer("* XÓA", "# TIẾP");
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_rework_decision_screen(const kiosk::protocol::ViewModel& view, int32_t good_so_far,
                                           int32_t defect_so_far, const String& transient_message,
                                           bool is_error, WifiIndicator wifi) {
  begin_screen("state_rework_decision");
  if (view.has_operation_name) {
    draw_fit_text(String(view.operation_name.c_str()), kContentTop + kSpacingSmall, kColorMuted,
                 /*prefer_large=*/false);
  }
  char ref[32];
  snprintf(ref, sizeof(ref), "Đạt: %ld   Lỗi: %ld", static_cast<long>(good_so_far),
           static_cast<long>(defect_so_far));
  String ref_s(ref);
  // Fixed-format "label: number  label: number" -- fits on one SMALL line
  // in every realistic case; draw_fit_text's own wrap policy still protects
  // an extreme value from clipping.
  draw_fit_text(ref_s, 60, kColorMuted, /*prefer_large=*/false);

  draw_title_2line("LỖI CÓ", "SỬA ĐƯỢC?", 128, kColorWarn);

  String opt1 = "1  CÓ";
  String opt2 = "2  KHÔNG";
  emit_component_text(centered_x(opt1, kFontSmall), 226, opt1, kColorAccent, kFontSmall);
  emit_component_text(centered_x(opt2, kFontSmall), 250, opt2, kColorFg, kFontSmall);

  if (transient_message.length() > 0) {
    draw_fit_text(transient_message, kFooterDividerY - kLineHeightSmall - kSpacingSmall,
                 is_error ? kColorErr : kColorFg, /*prefer_large=*/false);
  }
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_quantity_rework_screen(const kiosk::protocol::ViewModel& view,
                                           const String& local_digit_buffer, int32_t defect_so_far,
                                           const String& transient_message, bool is_error,
                                           WifiIndicator wifi) {
  begin_screen("state_quantity_rework");
  if (view.has_operation_name) {
    draw_fit_text(String(view.operation_name.c_str()), kContentTop + kSpacingSmall, kColorMuted,
                 /*prefer_large=*/false);
  }
  emit_component_text(centered_x("SỐ LƯỢNG SỬA", kFontSmall), 74, "SỐ LƯỢNG SỬA", kColorAccent, kFontSmall);
  char ref[32];
  snprintf(ref, sizeof(ref), "Tối đa: %ld", static_cast<long>(defect_so_far));
  String ref_s(ref);
  emit_component_text(centered_x(ref_s, kFontSmall), 96, ref_s, kColorMuted, kFontSmall);

  String digits = local_digit_buffer.length() > 0 ? local_digit_buffer : String("0");
  draw_value_giant(digits, 140, kColorAccent);

  if (transient_message.length() > 0) {
    draw_fit_text(transient_message, kFooterDividerY - kLineHeightSmall - kSpacingSmall,
                 is_error ? kColorErr : kColorFg, /*prefer_large=*/false);
  }
  draw_footer("* XÓA", "# TIẾP");
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_from_bundle(const kiosk::protocol::UiScreen& screen,
                                const kiosk::protocol::ViewModel& view, const String& local_digit_buffer,
                                const String& transient_message, bool is_error, WifiIndicator wifi) {
  using kiosk::protocol::UiComponentType;

  begin_screen(screen.screen_id.c_str());
  for (const auto& comp : screen.components) {
    uint16_t color = parse_hex_color(comp.color);
    switch (comp.type) {
      case UiComponentType::TEXT: {
        std::string resolved =
            kiosk::protocol::resolve_ui_tokens(comp.text, view, std::string(local_digit_buffer.c_str()));
        String resolved_str(resolved.c_str());
        // Bundle content keeps its own font_size field (backward-compatible
        // wire contract -- see select_vn_font()'s own comment) with the
        // same auto-shrink-to-fit-when-centered behavior as before.
        uint8_t effective_size = comp.font_size == 0 ? 1 : comp.font_size;
        const int16_t max_w = display_.width() - 8;
        if (comp.align == "center") {
          while (effective_size > 1 &&
                 measure_text_width(resolved_str, effective_size) > static_cast<uint16_t>(max_w)) {
            --effective_size;
          }
        }
        // 2026-08-25 bundle-text-clip fix: shrinking alone can still leave
        // centered text wider than the screen even at the smallest size
        // (found live: a long server-pushed operation name silently clipped
        // mid-word on state_session_active, bundle v10 -- the bundle JSON
        // itself was correct, font_size:2/align:center, this shrink-only
        // loop just had no floor). Same wrap-then-ellipsize policy
        // draw_fit_text() already uses for hardcoded screens, applied here
        // at whatever effective_size the shrink loop landed on.
        if (comp.align == "center" &&
            measure_text_width(resolved_str, effective_size) > static_cast<uint16_t>(max_w)) {
          String line1, line2;
          wrap_and_ellipsize_two_lines(resolved_str, effective_size, max_w, &line1, &line2);
          emit_component_text(centered_x(line1, effective_size), comp.y, line1, color, effective_size);
          if (line2.length() > 0) {
            emit_component_text(centered_x(line2, effective_size), comp.y + kLineHeightSmall, line2, color,
                                effective_size);
          }
          break;
        }
        int16_t x = comp.x;
        if (comp.align == "center") {
          x = centered_x(resolved_str, effective_size);
        } else if (comp.align == "right") {
          uint16_t w = measure_text_width(resolved_str, effective_size);
          x = static_cast<int16_t>(display_.width() - static_cast<int>(w));
          if (x < 0) x = 0;
        }
        emit_component_text(x, comp.y, resolved_str, color, effective_size);
        break;
      }
      case UiComponentType::RECT:
        display_.fillRect(comp.x, comp.y, comp.w, comp.h, color);
        break;
      case UiComponentType::LINE:
        display_.drawFastHLine(comp.x, comp.y, comp.w, color);
        break;
      case UiComponentType::UNSUPPORTED:
        break;  // already logged/dropped at parse time (ui_bundle.cpp)
    }
  }
  if (transient_message.length() > 0) {
    emit_line(6, transient_message, is_error ? kColorWarn : kColorFg);
  }
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_error_view(const String& message, bool is_network_error, WifiIndicator wifi) {
  begin_screen(is_network_error ? "error_view_network" : "error_view_business");

  // Full-width colored header band -- the real visual weight legacy's own
  // drawError() has. Short, curated title only (§16 of the task: no raw
  // backend text in the title) -- the (already short, curated Vietnamese)
  // business message itself goes in the body below via the shared wrap
  // policy, never a raw stack trace/technical string (kiosk_v2.py's own
  // rejection messages are already short human sentences, e.g. "Nhân viên
  // không hợp lệ" -- this renderer trusts that contract, it doesn't
  // re-validate it).
  display_.fillRect(0, 0, kScreenW, kHeaderH + 8, kColorErr);
  emit_component_text(kMarginX, kSpacingSmall, is_network_error ? "MẤT KẾT NỐI" : "LỖI", ILI9341_WHITE,
                      kFontSmall);

  draw_fit_text(message, 90, kColorErr, /*prefer_large=*/false);

  draw_footer("* QUAY LẠI", "");
  draw_status_bar(wifi);
  end_screen();
}

void Renderer::draw_resyncing_screen(WifiIndicator wifi) {
  begin_screen("resyncing");
  draw_title_line("ĐANG ĐỒNG BỘ", 140, kColorWarn);
  draw_status_bar(wifi);
  end_screen();
}

}  // namespace kiosk::ui
