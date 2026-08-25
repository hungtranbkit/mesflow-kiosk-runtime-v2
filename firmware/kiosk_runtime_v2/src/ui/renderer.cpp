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
  // 2026-08-24: draws through the same Vietnamese-capable glyph font as
  // emit_component_text() now (draw_vn_text()/measure_text_width()) instead
  // of Display::draw_line()'s Adafruit_GFX print() -- one font engine for
  // the whole renderer, not two. Every hardcoded/developer-authored string
  // in this file (boot screen, Wi-Fi recovery, error views, business
  // states) was also rewritten with real diacritics this same pass, now
  // that there's a font that can actually render them -- see
  // docs/VIETNAMESE_FONT.md.
  //
  // Real user feedback (2026-08-24, same day): still too small to read at
  // scale 1 (kVnFont's base glyph is ~7px tall, chosen to match the OLD
  // GFX font's proportions -- a deliberate baseline, not a readability
  // target). Bumped to scale 2 (~18px glyph cell: ascent 7*2=14 + descent
  // 2*2=4) -- line_height widened from 18 to 20px so two consecutive lines
  // at that height don't touch with zero margin. A handful of the longer
  // hardcoded status/rejection strings measure wider than the physical
  // screen at scale 2 (checked against every literal in this file before
  // this shipped -- e.g. "SO LUONG SUA KHONG DUOC LON HON SO LUONG LOI"),
  // so this auto-shrinks to scale 1 for just those, same
  // try-the-preferred-size-then-step-down pattern draw_from_bundle()'s
  // TEXT case already uses for bundle-driven text -- never a silently
  // clipped/overflowing line.
  const int line_height = 20;
  uint8_t line_font_size = 2;
  if (text.length() > 0 && measure_text_width(text, line_font_size) > display_.width() - 8) {
    line_font_size = 1;
  }
  draw_vn_text(4, 4 + row * line_height, text, color, line_font_size);

  if (row < 0 || row >= kMaxLines) return;

  // §7: measure with the SAME font metrics used to draw it and report/log
  // overflow -- never just silently let it clip with nobody told.
  //
  // A REAL bug caught live via the Serial Visual Debug Fallback tooling
  // (Phase 4 V1 Visual Parity pass, QUANTITY_INPUT before any digit is
  // typed): Adafruit_GFX's getTextBounds() on an EMPTY string returns a
  // garbage width on this font/library version (observed: 58452, nowhere
  // near a real measurement) instead of 0 -- producing a false
  // UI_TEXT_OVERFLOW warning on every screen with a blank row, even though
  // nothing is actually drawn or clipped. Skip the measurement entirely for
  // an empty string; there is nothing to overflow. measure_text_width()
  // (the Vietnamese-font path) has the same empty-string guard built in.
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

namespace {
// Selects a NATIVE glyph table (12/16/24px, matching v1's proven three-size
// strategy -- see vn_font_data.h's module comment for why this replaced a
// single 7px base scaled by nearest-neighbor block-fill, which is what
// made Vietnamese diacritics look blocky/broken) for a given bundle-
// declared font_size, plus the small integer block-scale still applied ON
// TOP of that native table for the (currently ASCII-digit-only) large
// sizes -- font_size stays whatever bundle content already specifies
// (backward-compatible wire/bundle contract, unchanged), only the
// font-LAYER's internal rendering strategy changed.
//
// font_size 1  -> Small (12px), scale 1 -- was 7px x1 = 7px
// font_size 2  -> Body  (16px), scale 1 -- was 7px x2 = 14px (the dominant
//                 real-Vietnamese-text size: labels, questions, names)
// font_size >=3 -> Large (24px), scale = round(font_size * 7 / 24), floor 1
//                 -- keeps existing large sizes (quantity digits at
//                 font_size=7 -> scale 2 -> 48px, vs the old blocky
//                 7px x7 = 49px) at very close to their PREVIOUS absolute
//                 pixel size, so existing screen layouts don't need
//                 recomputing, while the SOURCE glyph is now the clean
//                 24px table instead of a 7px one stretched 7x.
void select_vn_font(uint8_t font_size, const kiosk::protocol::VnFontData** out_font, uint8_t* out_scale) {
  const uint8_t size = font_size == 0 ? 1 : font_size;
  if (size == 1) {
    *out_font = &kiosk::ui::kVnFontSmall;
    *out_scale = 1;
  } else if (size == 2) {
    *out_font = &kiosk::ui::kVnFontBody;
    *out_scale = 1;
  } else {
    *out_font = &kiosk::ui::kVnFontLarge;
    uint16_t scaled = static_cast<uint16_t>((size * 7 + 12) / 24);  // round, not truncate
    *out_scale = scaled == 0 ? 1 : static_cast<uint8_t>(scaled);
  }
}
}  // namespace

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
      // fallback exactly, so drawn and measured width never disagree (a
      // real bug class this project has hit before with a second,
      // independently-drifting measurement).
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
  uint8_t size = 2;
  if (measure_text_width(text, size) > display_.width() - x - 4) {
    size = 1;
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
  // Safety bound (§7 of the Phase 4 stabilization task): never let an
  // out-of-bounds component corrupt the screen or wrap into undefined
  // territory. A bundle author's typo produces a dropped component (logged),
  // not garbage on screen -- same "never guess" philosophy as
  // ui_bundle.cpp's own malformed-component handling.
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

  // §7: measure with the SAME size (and now the same font engine) used to
  // draw it -- see emit_line()'s own comment on why an empty string must
  // skip measurement entirely (a real bug found live: Adafruit_GFX's
  // getTextBounds() on "" returned garbage, not 0). measure_text_width()
  // now goes through the Vietnamese glyph font (measure_vn_text()) instead
  // of GFX's own getTextBounds() -- using getTextBounds() here would
  // silently disagree with what draw_vn_text() actually drew, exactly the
  // kind of independently-drifting-measurement bug this project has hit
  // before.
  bool overflow = false;
  uint16_t w = 0, h = 0;
  if (text.length() > 0) {
    w = measure_text_width(text, size);
    h = (static_cast<uint16_t>(kVnFont.ascent) + kVnFont.descent) * size;
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

void Renderer::end_screen() {
  display_.bump_frame_id();
}

void Renderer::draw_wifi_indicator(WifiIndicator wifi) {
  // A mid-screen "WiFi: OK" text line used to duplicate the top-right icon
  // below -- real user feedback (2026-08-24): once the icon existed, the
  // text was pure redundant clutter breaking V1-style even distribution.
  // The icon alone already fully encodes all 4 WifiIndicator states (bar
  // count + the DISCONNECTED slash below), so the text line is dropped
  // entirely rather than kept for some states -- one indicator, not two.
  // qa_active_ observability (previously appended to this text line) now
  // has nowhere to attach on-screen; it still reflects in /debug/ui-state
  // via other means, so this is a presentation-only loss, not a data loss.

  // Graphical signal-bar icon, top-right corner -- real user feedback from
  // the true-geometry stabilization pass: "no wifi icon top-right, text
  // only". Three bars of increasing height (mirrors legacy's own NET_LED
  // bar icon), filled = on. This WifiIndicator enum has no per-dBm signal
  // data at this layer (unlike legacy's RSSI-driven bar count), so it's a
  // coarse approximation: CONNECTED=3 bars, CONNECTING=1 bar, DISCONNECTED/
  // UNKNOWN=0 bars (outline only) -- honest about what's actually known
  // here, not a fabricated signal strength.
  int bars_on = wifi == WifiIndicator::CONNECTED ? 3 : wifi == WifiIndicator::CONNECTING ? 1 : 0;
  uint16_t bar_on_color = wifi == WifiIndicator::CONNECTED ? kColorAccent : kColorWarn;
  constexpr int16_t kIconRight = 234;   // right edge, matches the display's own right margin
  constexpr int16_t kIconBaseY = 18;    // baseline every bar sits on
  constexpr int16_t kBarW = 5;
  constexpr int16_t kBarGapX = 7;
  for (int i = 0; i < 3; ++i) {
    int16_t bar_h = 4 + i * 4;  // 4, 8, 12px tall -- shortest bar first (left)
    int16_t x = kIconRight - (3 - i) * kBarGapX;
    int16_t y = kIconBaseY - bar_h;
    if (i < bars_on) {
      display_.fillRect(x, y, kBarW, bar_h, bar_on_color);
    } else {
      display_.drawRect(x, y, kBarW, bar_h, kColorMuted);
    }
  }
  if (wifi == WifiIndicator::DISCONNECTED) {
    // A diagonal slash through the icon -- "no signal" must be
    // unmistakable at a glance, not just three empty outlines that could
    // also just mean "haven't measured yet".
    display_.drawLine(kIconRight - 3 * kBarGapX, kIconBaseY - 12, kIconRight, kIconBaseY, kColorWarn);
  }

  // SSID prefix (Phase 3B task, real user need: multiple times this
  // session a device connected to the WRONG/unreliable network with no way
  // to tell at a glance which one it was on without pulling device-state
  // over serial). First 4 PRINTABLE characters only (§3 of the task -- a
  // deliberate privacy/space bound, never the full SSID on the operator
  // screen). "----" when disconnected/empty -- never a stale cached name
  // from before a disconnect (WiFi.SSID() itself already returns "" once
  // disconnected, so reading it fresh here is enough; no extra state to
  // track). Read directly via WiFi.SSID() rather than threading a new
  // parameter through every draw_*() call site (this is the one place
  // that already draws the icon every frame, so it's also the natural one
  // place to draw this).
  {
    String ssid = wifi == WifiIndicator::DISCONNECTED || wifi == WifiIndicator::UNKNOWN
                      ? String("") : WiFi.SSID();
    String prefix = ssid.length() > 0 ? ssid.substring(0, ssid.length() < 4 ? ssid.length() : 4)
                                       : String("----");
    uint16_t w = measure_text_width(prefix, 1);
    int16_t leftmost_bar_x = kIconRight - 3 * kBarGapX;
    int16_t x = leftmost_bar_x - 4 - static_cast<int>(w);
    if (x < 0) x = 0;
    emit_component_text(x, kIconBaseY - 12, prefix, kColorMuted, 1);
  }
}

void Renderer::draw_boot_screen(const kiosk::runtime::BootDiagnostics& d,
                                 bool scanner_ok, bool keypad_ok) {
  begin_screen("boot");
  char line[64];

  emit_line(0, "MESFlow Kiosk Runtime v2", kColorAccent);

  snprintf(line, sizeof(line), "fw %s (%s)", d.fw_version.c_str(), d.build_id.c_str());
  emit_line(1, line, kColorFg);

  snprintf(line, sizeof(line), "%s x%d, flash %uMB", d.chip_model.c_str(),
           d.chip_cores, d.flash_mb);
  emit_line(2, line, kColorFg);

  snprintf(line, sizeof(line), "psram %u/%u KB free", d.psram_free_bytes / 1024,
           d.psram_total_bytes / 1024);
  emit_line(3, line, kColorFg);

  snprintf(line, sizeof(line), "heap %u KB, block %u KB", d.free_heap_bytes / 1024,
           d.largest_free_block_bytes / 1024);
  emit_line(4, line, kColorFg);

  snprintf(line, sizeof(line), "reset: %s", d.reset_reason.c_str());
  emit_line(5, line, kColorFg);

  snprintf(line, sizeof(line), "scanner: %s  keypad: %s",
           scanner_ok ? "OK" : "FAIL", keypad_ok ? "OK" : "DEGRADED");
  emit_line(6, line, scanner_ok ? kColorAccent : kColorWarn);

  end_screen();
}

void Renderer::draw_waiting_screen(WifiIndicator wifi) {
  begin_screen("waiting");
  emit_line(0, "MESFlow Kiosk Runtime v2", kColorAccent);
  emit_line(2, "Sẵn sàng quét mã", kColorFg);
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_scan_received(const String& raw_code, WifiIndicator wifi) {
  begin_screen("scan_received");
  emit_line(0, "Đã nhận mã", kColorAccent);
  emit_line(2, raw_code, kColorFg);
  emit_line(4, "Đang kiểm tra...", kColorFg);
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_scan_result(const String& raw_code, const String& status_text,
                                const String& error_code, WifiIndicator wifi) {
  begin_screen("scan_result");
  emit_line(0, raw_code, kColorFg);
  emit_line(2, status_text, kColorAccent);
  if (error_code.length() > 0) {
    emit_line(4, "Mã lỗi: " + error_code, kColorWarn);
  }
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_keypad_calibration_prompt(char key, uint8_t index, uint8_t total) {
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
  // 2026-08-25 universal-escape follow-up: this is called ONLY for the 3s-5s
  // countdown BEFORE the recovery menu opens (WifiRecoveryController::tick()
  // returns as soon as the menu itself is showing, at the 5s threshold) --
  // continuing to hold past 5s toward the 10s Wi-Fi-setup threshold shows
  // the recovery menu screen instead (its own footer line documents that
  // holding longer opens Wi-Fi setup), not a live countdown over it, per
  // the task's own "keep it simple, no fancy layout work".
  if (seconds_held <= 0) return;  // caller controls when to start showing this
  begin_screen("wifi_hold_progress");
  emit_line(0, "Tiếp tục giữ *", kColorFg);
  emit_line(1, "để mở menu khôi phục", kColorFg);
  end_screen();
}

void Renderer::draw_wifi_portal_active(const String& ssid) {
  begin_screen("wifi_portal_active");
  emit_line(0, "CÀI ĐẶT WI-FI", kColorAccent);
  emit_line(2, "Kết nối:", kColorFg);
  emit_line(3, ssid, kColorFg);
  emit_line(4, "Không mật khẩu", kColorFg);
  emit_line(6, "Mở: 192.168.4.1", kColorFg);
  end_screen();
}

void Renderer::draw_wifi_portal_testing(const String& ssid) {
  begin_screen("wifi_portal_testing");
  emit_line(0, "Đang kiểm tra kết nối", kColorFg);
  emit_line(2, ssid, kColorAccent);
  emit_line(4, "Vui lòng đợi...", kColorFg);
  end_screen();
}

void Renderer::draw_wifi_portal_failed(const String& message) {
  begin_screen("wifi_portal_failed");
  emit_line(0, "Kết nối thất bại", kColorWarn);
  emit_line(2, message, kColorFg);
  emit_line(5, "Mở 192.168.4.1 để thử lại", kColorFg);
  end_screen();
}

void Renderer::draw_safe_mode_screen(const String& reason_code, WifiIndicator wifi) {
  begin_screen("safe_mode");
  emit_line(0, "MESFLOW - KHÔI PHỤC", kColorErr);
  emit_line(2, "Lý do:", kColorFg);
  emit_line(3, reason_code.length() > 0 ? reason_code : String("Không xác định"), kColorWarn);
  emit_line(4, "TRẠNG THÁI: SAFE MODE", kColorAccent);
  emit_line(6, "Đang tự thử lại...", kColorFg);
  emit_line(7, "* giữ: menu | * giữ lâu: Wi-Fi", kColorMuted);
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_recovery_menu(WifiIndicator wifi) {
  begin_screen("recovery_menu");
  emit_line(0, "MENU KHÔI PHỤC", kColorAccent);
  emit_line(2, "1 Thử lại mạng", kColorFg);
  emit_line(3, "2 Đồng bộ lại", kColorFg);
  emit_line(4, "3 Cài đặt Wi-Fi", kColorFg);
  emit_line(5, "4 Quay lại", kColorFg);
  emit_line(6, "5 Khởi động lại", kColorFg);
  emit_line(7, "* giữ lâu hơn: Wi-Fi", kColorMuted);
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_identity_screen(kiosk::security::ProvisioningState state,
                                    const String& hardware_id, WifiIndicator wifi) {
  begin_screen("identity");
  using kiosk::security::ProvisioningState;
  switch (state) {
    case ProvisioningState::UNPROVISIONED:
    case ProvisioningState::PROVISIONING:
      emit_line(0, "THIẾT BỊ CHƯA CẤU HÌNH", kColorWarn);
      emit_line(2, "Cần provision device_id", kColorFg);
      break;
    case ProvisioningState::SUSPENDED:
      emit_line(0, "THIẾT BỊ TẠM DỪNG", kColorWarn);
      emit_line(2, "Liên hệ quản trị viên", kColorFg);
      break;
    case ProvisioningState::REVOKED:
      emit_line(0, "THIẾT BỊ ĐÃ BỊ THU HỒI", kColorWarn);
      emit_line(2, "Không thể kết nối backend", kColorFg);
      break;
    case ProvisioningState::ACTIVE:
      // Not expected to be called with ACTIVE (caller shows the normal
      // waiting screen instead) -- render something honest if it happens.
      emit_line(0, "identity: ACTIVE", kColorAccent);
      break;
  }
  emit_line(4, hardware_id, kColorFg);
  emit_line(6, "Giữ * 10s để cài Wi-Fi", kColorFg);
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_business_state(const kiosk::protocol::StateSnapshot& s,
                                   const String& transient_message, bool is_error,
                                   WifiIndicator wifi) {
  using kiosk::protocol::BusinessState;
  char line[48];

  switch (s.state) {
    case BusinessState::WAIT_EMPLOYEE:
      begin_screen("state_wait_employee");
      emit_line(0, "MESFlow Kiosk Runtime v2", kColorAccent);
      emit_line(2, "Sẵn sàng quét thẻ nhân viên", kColorFg);
      break;

    case BusinessState::WAIT_OPERATION:
      begin_screen("state_wait_operation");
      emit_line(0,
                s.view.has_employee_name ? String(s.view.employee_name.c_str()) : String("(nhan vien)"),
                kColorAccent);
      emit_line(2, "Quét mã công đoạn", kColorFg);
      break;

    case BusinessState::SESSION_ACTIVE:
      begin_screen("state_session_active");
      emit_line(0, s.view.has_employee_name ? String(s.view.employee_name.c_str()) : String(""),
                kColorAccent);
      if (s.view.has_operation_name) emit_line(1, String(s.view.operation_name.c_str()), kColorFg);
      if (s.view.has_target_qty) {
        snprintf(line, sizeof(line), "Mục tiêu: %ld", static_cast<long>(s.view.target_qty));
        emit_line(3, line, kColorFg);
      }
      emit_line(5, "Bấm # để kết thúc", kColorFg);
      break;

    case BusinessState::DEVICE_DISABLED:
      begin_screen("state_device_disabled");
      emit_line(0, "THIẾT BỊ ĐÃ BỊ VÔ HIỆU HÓA", kColorWarn);
      emit_line(2, "Liên hệ quản trị viên", kColorFg);
      break;

    case BusinessState::MAINTENANCE:
      begin_screen("state_maintenance");
      emit_line(0, "THIẾT BỊ ĐANG BẢO TRÌ", kColorWarn);
      break;

    case BusinessState::QUANTITY_INPUT:
      // Not expected here -- caller uses draw_quantity_input_screen instead.
      // Render something honest rather than silently doing nothing.
      begin_screen("state_quantity_input_fallback");
      emit_line(0, "QUANTITY_INPUT (thieu view)", kColorWarn);
      break;

    case BusinessState::UNSUPPORTED:
      begin_screen("state_unsupported");
      emit_line(0, "TRẠNG THÁI KHÔNG HỖ TRỢ", kColorWarn);
      emit_line(2, "Firmware có thể đã cũ", kColorFg);
      break;
  }

  if (transient_message.length() > 0) {
    emit_line(6, transient_message, is_error ? kColorWarn : kColorFg);
  }
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_quantity_input_screen(const kiosk::protocol::ViewModel& view,
                                          const String& local_digit_buffer,
                                          const String& transient_message, bool is_error,
                                          WifiIndicator wifi) {
  begin_screen("state_quantity_input");
  emit_line(0, view.has_operation_name ? String(view.operation_name.c_str()) : String(""), kColorAccent);
  emit_line(2, "Nhập số lượng đạt, # để gửi:", kColorFg);
  emit_line(4, local_digit_buffer.length() > 0 ? local_digit_buffer : String("_"), kColorAccent);
  if (transient_message.length() > 0) {
    emit_line(6, transient_message, is_error ? kColorWarn : kColorFg);
  }
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_quantity_defect_screen(const kiosk::protocol::ViewModel& view,
                                           const String& local_digit_buffer, int32_t good_so_far,
                                           const String& transient_message, bool is_error,
                                           WifiIndicator wifi) {
  begin_screen("state_quantity_defect");
  // top status area (y 0..35): small context line, WiFi icon (below)
  emit_component_text(4, 8, view.has_operation_name ? String(view.operation_name.c_str()) : String(""),
                      kColorMuted, 1);

  // main content area (y 40..265): centered title + reference + giant digits
  String title = "SẢN PHẨM LỖI";
  emit_component_text(centered_x(title, 2), 44, title, kColorErr, 2);

  // Real user feedback (2026-08-24): "mo ta nho qua" -- reference/secondary
  // lines across these quantity screens were font_size=1, hard to read on
  // the physical display. Bumped to 2 -- safe here (unlike employee_name/
  // operation_name elsewhere) because these are short, fixed-format
  // "label: number" strings, never real variable-length data.
  char ref[24];
  snprintf(ref, sizeof(ref), "Đạt: %ld", static_cast<long>(good_so_far));
  String ref_s(ref);
  emit_component_text(centered_x(ref_s, 2), 76, ref_s, kColorMuted, 2);

  String digits = local_digit_buffer.length() > 0 ? local_digit_buffer : String("0");
  emit_component_text(centered_x(digits, 7), 130, digits, kColorErr, 7);
  display_.drawFastHLine(20, 216, display_.width() - 40, kColorMuted);

  if (transient_message.length() > 0) {
    emit_transient_message(4, 226, transient_message, is_error ? kColorErr : kColorFg);
  }

  // footer action area (y 270..319): left/right aligned, matches v1 two-column style
  emit_component_text(4, 290, "* XÓA", kColorMuted, 2);
  String tiep = "# TIẾP";
  emit_component_text(display_.width() - measure_text_width(tiep, 2) - 4, 290, tiep, kColorMuted, 2);

  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_rework_decision_screen(const kiosk::protocol::ViewModel& view, int32_t good_so_far,
                                           int32_t defect_so_far, const String& transient_message,
                                           bool is_error, WifiIndicator wifi) {
  begin_screen("state_rework_decision");
  emit_component_text(4, 8, view.has_operation_name ? String(view.operation_name.c_str()) : String(""),
                      kColorMuted, 1);

  // Split into two lines at size 2 (real user feedback: too small at size
  // 1) rather than one combined line -- "Dat: 999999   Loi: 999999" could
  // overflow at size 2 on one line; two short "label: number" lines can't.
  char ref_good[16], ref_defect[16];
  snprintf(ref_good, sizeof(ref_good), "Đạt: %ld", static_cast<long>(good_so_far));
  snprintf(ref_defect, sizeof(ref_defect), "Lỗi: %ld", static_cast<long>(defect_so_far));
  String ref_good_s(ref_good), ref_defect_s(ref_defect);
  emit_component_text(centered_x(ref_good_s, 2), 40, ref_good_s, kColorMuted, 2);
  emit_component_text(centered_x(ref_defect_s, 2), 64, ref_defect_s, kColorMuted, 2);

  // The question is long -- font_size=1 measured, not guessed, so it never
  // clips regardless of exact glyph metrics (§3 of the task: variable/long
  // text must not be forced into a giant size that overflows).
  String q1 = "LỖI CÓ SỬA ĐƯỢC";
  String q2 = "KHÔNG?";
  emit_component_text(centered_x(q1, 2), 90, q1, kColorWarn, 2);
  emit_component_text(centered_x(q2, 2), 114, q2, kColorWarn, 2);

  String opt1 = "1   CÓ";
  String opt2 = "2   KHÔNG";
  emit_component_text(centered_x(opt1, 2), 170, opt1, kColorAccent, 2);
  emit_component_text(centered_x(opt2, 2), 200, opt2, kColorFg, 2);

  if (transient_message.length() > 0) {
    emit_transient_message(4, 240, transient_message, is_error ? kColorErr : kColorFg);
  }

  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_quantity_rework_screen(const kiosk::protocol::ViewModel& view,
                                           const String& local_digit_buffer, int32_t defect_so_far,
                                           const String& transient_message, bool is_error,
                                           WifiIndicator wifi) {
  begin_screen("state_quantity_rework");
  emit_component_text(4, 8, view.has_operation_name ? String(view.operation_name.c_str()) : String(""),
                      kColorMuted, 1);

  String title = "SỐ LƯỢNG CẦN SỬA";
  emit_component_text(centered_x(title, 2), 44, title, kColorAccent, 2);

  char ref[32];
  snprintf(ref, sizeof(ref), "Tối đa: %ld", static_cast<long>(defect_so_far));
  String ref_s(ref);
  emit_component_text(centered_x(ref_s, 2), 76, ref_s, kColorMuted, 2);

  String digits = local_digit_buffer.length() > 0 ? local_digit_buffer : String("0");
  emit_component_text(centered_x(digits, 7), 130, digits, kColorAccent, 7);
  display_.drawFastHLine(20, 216, display_.width() - 40, kColorMuted);

  if (transient_message.length() > 0) {
    emit_transient_message(4, 226, transient_message, is_error ? kColorErr : kColorFg);
  }

  emit_component_text(4, 290, "* XÓA", kColorMuted, 2);
  String tiep = "# TIẾP";
  emit_component_text(display_.width() - measure_text_width(tiep, 2) - 4, 290, tiep, kColorMuted, 2);

  draw_wifi_indicator(wifi);
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
        // True x/y/font_size geometry (Phase 4 stabilization task) --
        // previously quantized to one of 10 fixed 18px rows regardless of
        // the bundle's own y, and font_size was silently ignored. Bundle
        // authors can now genuinely reproduce legacy's real layout/font
        // hierarchy instead of an approximation forced into a row grid.
        std::string resolved =
            kiosk::protocol::resolve_ui_tokens(comp.text, view, std::string(local_digit_buffer.c_str()));
        String resolved_str(resolved.c_str());
        // UI centering (Phase 4.1): align=="center"/"right" computes real x
        // from the RESOLVED text's MEASURED width (getTextBounds(), below)
        // -- correct for variable-length real data (employee_name/
        // operation_name) where a bundle author cannot know the final
        // string length at authoring time. align=="left" (default, and
        // every bundle authored before this field existed) keeps comp.x
        // exactly as before -- no behavior change for existing content.
        // Font-size auto-fallback (real user feedback, 2026-08-24: "ten
        // nguoi/cong doan to len... nho qua khong doc duoc" -- employee_name/
        // operation_name were kept at a small fixed size specifically to
        // avoid a real clipping bug found earlier with long operation names
        // at a bigger size). Only for centered text: try the bundle's
        // requested font_size first (now genuinely bigger/more readable for
        // the common case), and if the RESOLVED text is too wide to fit at
        // that size, step down one size at a time (never below 1) until it
        // fits -- so short names render big, long real names never clip,
        // and no bundle content change risks a regression either way.
        uint8_t effective_size = comp.font_size == 0 ? 1 : comp.font_size;
        if (comp.align == "center") {
          int16_t max_w = display_.width() - 8;  // small margin each side
          while (effective_size > 1 &&
                 measure_text_width(resolved_str, effective_size) > static_cast<uint16_t>(max_w)) {
            --effective_size;
          }
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
        // Horizontal only for this first implementation (every current
        // screen only ever uses horizontal divider lines) -- a bundle
        // author needing a vertical line is a real gap, not silently
        // guessed at; document as a known limitation rather than draw
        // something that might be wrong.
        display_.drawFastHLine(comp.x, comp.y, comp.w, color);
        break;
      case UiComponentType::UNSUPPORTED:
        break;  // already logged/dropped at parse time (ui_bundle.cpp)
    }
  }
  if (transient_message.length() > 0) {
    emit_line(6, transient_message, is_error ? kColorWarn : kColorFg);
  }
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_error_view(const String& message, bool is_network_error, WifiIndicator wifi) {
  begin_screen(is_network_error ? "error_view_network" : "error_view_business");

  // Full-width red header band -- the real visual weight legacy's own
  // drawError() has (a whole colored header, not one line buried in the
  // normal layout). Title text distinguishes network/backend failures from
  // business rejections at a glance, not just in the message wording below
  // (§3/§9 of the Phase 4.1 closure task: the two classes must stay
  // visually AND textually distinct).
  constexpr int16_t kHeaderH = 32;
  display_.fillRect(0, 0, display_.width(), kHeaderH, kColorErr);
  emit_component_text(4, 8, is_network_error ? "MẤT KẾT NỐI" : "LỖI", ILI9341_WHITE, 2);

  // Message body: word-wrapped across multiple lines (each measured, never
  // guessed, to fit the display width) -- real bug found live (2026-08-24,
  // REWORK_EXCEEDS_DEFECT's message): even at size 1, a single
  // emit_component_text() call with no wrapping still clipped off the right
  // edge past ~40 chars. Now that wrapping handles arbitrary length safely,
  // size 2 (real user feedback: "mo ta nho qua" -- error text was too small
  // to read) is safe too -- it was never the size itself causing clipping,
  // it was the missing wrap.
  {
    constexpr int16_t kMsgX = 4;
    constexpr int16_t kMsgY0 = 60;
    constexpr int16_t kLineH = 26;
    constexpr uint8_t kMsgSize = 2;
    int16_t max_w = display_.width() - kMsgX - 4;
    String remaining = message;
    int16_t y = kMsgY0;
    // Bounded: never draw past the footer hint, and never loop forever on
    // a single unbreakable "word" wider than the whole screen.
    while (remaining.length() > 0 && y < 270) {
      int space_at = -1;
      String line = remaining;
      // Find the longest prefix (breaking only at spaces) that still fits.
      for (unsigned int i = 0; i < remaining.length(); ++i) {
        if (remaining[i] == ' ') {
          String candidate = remaining.substring(0, i);
          if (measure_text_width(candidate, kMsgSize) <= static_cast<uint16_t>(max_w)) {
            space_at = static_cast<int>(i);
          } else {
            break;
          }
        }
      }
      if (measure_text_width(remaining, kMsgSize) <= static_cast<uint16_t>(max_w)) {
        line = remaining;
        remaining = "";
      } else if (space_at >= 0) {
        line = remaining.substring(0, space_at);
        remaining = remaining.substring(space_at + 1);
      } else {
        // No space found that fits -- an unbreakable long token (or the
        // whole message has no spaces at all); draw what's there rather
        // than loop forever, accepting this one line may still clip.
        remaining = "";
      }
      emit_component_text(kMsgX, y, line, kColorErr, kMsgSize);
      y += kLineH;
    }
  }

  emit_component_text(4, 284, "* QUAY LẠI", kColorMuted, 2);
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_resyncing_screen(WifiIndicator wifi) {
  begin_screen("resyncing");
  emit_line(0, "Đang đồng bộ trạng thái...", kColorWarn);
  emit_line(2, "(STATE_CONFLICT -> RESYNC)", kColorFg);
  draw_wifi_indicator(wifi);
  end_screen();
}

}  // namespace kiosk::ui
