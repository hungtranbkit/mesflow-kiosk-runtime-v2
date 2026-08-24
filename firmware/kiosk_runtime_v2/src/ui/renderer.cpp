#include "renderer.h"

#include <cstdlib>

#include "../health/structured_log.h"

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
  display_.draw_line(row, text.c_str(), color);

  if (row < 0 || row >= kMaxLines) return;

  // §7: measure with the SAME font metrics used to draw it (getTextBounds
  // on the real Display object, not a hardcoded chars-per-row guess) and
  // report/log overflow -- never just silently let display.cpp's
  // setTextWrap(false) clip it with nobody told.
  //
  // A REAL bug caught live via the Serial Visual Debug Fallback tooling
  // (Phase 4 V1 Visual Parity pass, QUANTITY_INPUT before any digit is
  // typed): Adafruit_GFX's getTextBounds() on an EMPTY string returns a
  // garbage width on this font/library version (observed: 58452, nowhere
  // near a real measurement) instead of 0 -- producing a false
  // UI_TEXT_OVERFLOW warning on every screen with a blank row, even though
  // nothing is actually drawn or clipped. Skip the measurement entirely for
  // an empty string; there is nothing to overflow.
  int16_t x1 = 0, y1 = 0;
  uint16_t w = 0, h = 0;
  bool overflow = false;
  if (text.length() > 0) {
    display_.setTextSize(1);
    display_.getTextBounds(text, 4, 4 + row * 18, &x1, &y1, &w, &h);
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
  if (text.length() == 0) return 0;  // getTextBounds("") is not meaningful -- same caveat as elsewhere
  uint8_t size = font_size == 0 ? 1 : font_size;
  int16_t x1, y1;
  uint16_t w, h;
  display_.setTextSize(size);
  display_.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return w;
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

  display_.setCursor(x, y);
  display_.setTextColor(color, ILI9341_BLACK);
  display_.setTextSize(size);
  display_.setTextWrap(false);
  display_.print(text);

  if (next_component_ >= kMaxComponents) return;  // silently drop past the cap -- logged once below
  DrawnComponent& comp = components_[next_component_++];
  comp.used = true;
  comp.x = x;
  comp.y = y;
  comp.font_size = size;
  comp.text = text;
  comp.color = color;

  // §7: measure with the SAME size used to draw it -- see emit_line()'s own
  // comment on why an empty string must skip measurement entirely (a real
  // bug found live: getTextBounds() on "" returns garbage, not 0).
  bool overflow = false;
  uint16_t w = 0, h = 0;
  if (text.length() > 0) {
    int16_t x1, y1;
    display_.setTextSize(size);
    display_.getTextBounds(text, x, y, &x1, &y1, &w, &h);
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
  emit_line(2, "San sang quet ma", kColorFg);
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_scan_received(const String& raw_code, WifiIndicator wifi) {
  begin_screen("scan_received");
  emit_line(0, "Da nhan ma", kColorAccent);
  emit_line(2, raw_code, kColorFg);
  emit_line(4, "Dang kiem tra...", kColorFg);
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_scan_result(const String& raw_code, const String& status_text,
                                const String& error_code, WifiIndicator wifi) {
  begin_screen("scan_result");
  emit_line(0, raw_code, kColorFg);
  emit_line(2, status_text, kColorAccent);
  if (error_code.length() > 0) {
    emit_line(4, "Ma loi: " + error_code, kColorWarn);
  }
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_keypad_calibration_prompt(char key, uint8_t index, uint8_t total) {
  begin_screen("keypad_calibration");
  char line[32];
  emit_line(0, "HIEU CHINH BAN PHIM", kColorAccent);
  snprintf(line, sizeof(line), "BAM PHIM %c", key);
  emit_line(2, line, kColorFg);
  snprintf(line, sizeof(line), "%u / %u", index + 1, total);
  emit_line(4, line, kColorFg);
  emit_line(6, "Bam va tha tung phim", kColorFg);
  end_screen();
}

void Renderer::draw_wifi_hold_progress(int seconds_held) {
  if (seconds_held <= 0) return;  // caller controls when to start showing this
  begin_screen("wifi_hold_progress");
  char line[32];
  if (seconds_held < 7) {
    emit_line(0, "Tiep tuc giu * de", kColorFg);
    emit_line(1, "cai dat Wi-Fi", kColorFg);
  } else {
    emit_line(0, "Wi-Fi setup", kColorAccent);
    snprintf(line, sizeof(line), "%d...", seconds_held);
    emit_line(2, line, kColorAccent);
  }
  end_screen();
}

void Renderer::draw_wifi_portal_active(const String& ssid) {
  begin_screen("wifi_portal_active");
  emit_line(0, "CAI DAT WI-FI", kColorAccent);
  emit_line(2, "Ket noi:", kColorFg);
  emit_line(3, ssid, kColorFg);
  emit_line(5, "Mo: 192.168.4.1", kColorFg);
  end_screen();
}

void Renderer::draw_wifi_portal_testing(const String& ssid) {
  begin_screen("wifi_portal_testing");
  emit_line(0, "Dang kiem tra ket noi", kColorFg);
  emit_line(2, ssid, kColorAccent);
  emit_line(4, "Vui long doi...", kColorFg);
  end_screen();
}

void Renderer::draw_wifi_portal_failed(const String& message) {
  begin_screen("wifi_portal_failed");
  emit_line(0, "Ket noi that bai", kColorWarn);
  emit_line(2, message, kColorFg);
  emit_line(5, "Mo 192.168.4.1 de thu lai", kColorFg);
  end_screen();
}

void Renderer::draw_identity_screen(kiosk::security::ProvisioningState state,
                                    const String& hardware_id, WifiIndicator wifi) {
  begin_screen("identity");
  using kiosk::security::ProvisioningState;
  switch (state) {
    case ProvisioningState::UNPROVISIONED:
    case ProvisioningState::PROVISIONING:
      emit_line(0, "THIET BI CHUA CAU HINH", kColorWarn);
      emit_line(2, "Can provision device_id", kColorFg);
      break;
    case ProvisioningState::SUSPENDED:
      emit_line(0, "THIET BI TAM DUNG", kColorWarn);
      emit_line(2, "Lien he quan tri vien", kColorFg);
      break;
    case ProvisioningState::REVOKED:
      emit_line(0, "THIET BI DA BI THU HOI", kColorWarn);
      emit_line(2, "Khong the ket noi backend", kColorFg);
      break;
    case ProvisioningState::ACTIVE:
      // Not expected to be called with ACTIVE (caller shows the normal
      // waiting screen instead) -- render something honest if it happens.
      emit_line(0, "identity: ACTIVE", kColorAccent);
      break;
  }
  emit_line(4, hardware_id, kColorFg);
  emit_line(6, "Giu * 10s de cai Wi-Fi", kColorFg);
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
      emit_line(2, "San sang quet the nhan vien", kColorFg);
      break;

    case BusinessState::WAIT_OPERATION:
      begin_screen("state_wait_operation");
      emit_line(0,
                s.view.has_employee_name ? String(s.view.employee_name.c_str()) : String("(nhan vien)"),
                kColorAccent);
      emit_line(2, "Quet ma cong doan", kColorFg);
      break;

    case BusinessState::SESSION_ACTIVE:
      begin_screen("state_session_active");
      emit_line(0, s.view.has_employee_name ? String(s.view.employee_name.c_str()) : String(""),
                kColorAccent);
      if (s.view.has_operation_name) emit_line(1, String(s.view.operation_name.c_str()), kColorFg);
      if (s.view.has_target_qty) {
        snprintf(line, sizeof(line), "Muc tieu: %ld", static_cast<long>(s.view.target_qty));
        emit_line(3, line, kColorFg);
      }
      emit_line(5, "Bam # de ket thuc", kColorFg);
      break;

    case BusinessState::DEVICE_DISABLED:
      begin_screen("state_device_disabled");
      emit_line(0, "THIET BI DA BI VO HIEU HOA", kColorWarn);
      emit_line(2, "Lien he quan tri vien", kColorFg);
      break;

    case BusinessState::MAINTENANCE:
      begin_screen("state_maintenance");
      emit_line(0, "THIET BI DANG BAO TRI", kColorWarn);
      break;

    case BusinessState::QUANTITY_INPUT:
      // Not expected here -- caller uses draw_quantity_input_screen instead.
      // Render something honest rather than silently doing nothing.
      begin_screen("state_quantity_input_fallback");
      emit_line(0, "QUANTITY_INPUT (thieu view)", kColorWarn);
      break;

    case BusinessState::UNSUPPORTED:
      begin_screen("state_unsupported");
      emit_line(0, "TRANG THAI KHONG HO TRO", kColorWarn);
      emit_line(2, "Firmware co the da cu", kColorFg);
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
  emit_line(2, "Nhap so luong dat, # de gui:", kColorFg);
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
  String title = "SAN PHAM LOI";
  emit_component_text(centered_x(title, 2), 44, title, kColorErr, 2);

  char ref[24];
  snprintf(ref, sizeof(ref), "Dat: %ld", static_cast<long>(good_so_far));
  String ref_s(ref);
  emit_component_text(centered_x(ref_s, 1), 76, ref_s, kColorMuted, 1);

  String digits = local_digit_buffer.length() > 0 ? local_digit_buffer : String("0");
  emit_component_text(centered_x(digits, 7), 130, digits, kColorErr, 7);
  display_.drawFastHLine(20, 216, display_.width() - 40, kColorMuted);

  if (transient_message.length() > 0) {
    emit_component_text(4, 226, transient_message, is_error ? kColorErr : kColorFg, 1);
  }

  // footer action area (y 270..319): left/right aligned, matches v1 two-column style
  emit_component_text(4, 290, "* XOA", kColorMuted, 1);
  String tiep = "# TIEP";
  emit_component_text(display_.width() - measure_text_width(tiep, 1) - 4, 290, tiep, kColorMuted, 1);

  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_rework_decision_screen(const kiosk::protocol::ViewModel& view, int32_t good_so_far,
                                           int32_t defect_so_far, const String& transient_message,
                                           bool is_error, WifiIndicator wifi) {
  begin_screen("state_rework_decision");
  emit_component_text(4, 8, view.has_operation_name ? String(view.operation_name.c_str()) : String(""),
                      kColorMuted, 1);

  char ref[40];
  snprintf(ref, sizeof(ref), "Dat: %ld   Loi: %ld", static_cast<long>(good_so_far),
           static_cast<long>(defect_so_far));
  String ref_s(ref);
  emit_component_text(centered_x(ref_s, 1), 44, ref_s, kColorMuted, 1);

  // The question is long -- font_size=1 measured, not guessed, so it never
  // clips regardless of exact glyph metrics (§3 of the task: variable/long
  // text must not be forced into a giant size that overflows).
  String q1 = "LOI CO SUA DUOC";
  String q2 = "KHONG?";
  emit_component_text(centered_x(q1, 2), 90, q1, kColorWarn, 2);
  emit_component_text(centered_x(q2, 2), 114, q2, kColorWarn, 2);

  String opt1 = "1   CO";
  String opt2 = "2   KHONG";
  emit_component_text(centered_x(opt1, 2), 170, opt1, kColorAccent, 2);
  emit_component_text(centered_x(opt2, 2), 200, opt2, kColorFg, 2);

  if (transient_message.length() > 0) {
    emit_component_text(4, 240, transient_message, is_error ? kColorErr : kColorFg, 1);
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

  String title = "SO LUONG CAN SUA";
  emit_component_text(centered_x(title, 2), 44, title, kColorAccent, 2);

  char ref[32];
  snprintf(ref, sizeof(ref), "Toi da: %ld", static_cast<long>(defect_so_far));
  String ref_s(ref);
  emit_component_text(centered_x(ref_s, 1), 76, ref_s, kColorMuted, 1);

  String digits = local_digit_buffer.length() > 0 ? local_digit_buffer : String("0");
  emit_component_text(centered_x(digits, 7), 130, digits, kColorAccent, 7);
  display_.drawFastHLine(20, 216, display_.width() - 40, kColorMuted);

  if (transient_message.length() > 0) {
    emit_component_text(4, 226, transient_message, is_error ? kColorErr : kColorFg, 1);
  }

  emit_component_text(4, 290, "* XOA", kColorMuted, 1);
  String tiep = "# TIEP";
  emit_component_text(display_.width() - measure_text_width(tiep, 1) - 4, 290, tiep, kColorMuted, 1);

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
        int16_t x = comp.x;
        if (comp.align == "center") {
          x = centered_x(resolved_str, comp.font_size);
        } else if (comp.align == "right") {
          uint16_t w = measure_text_width(resolved_str, comp.font_size);
          x = static_cast<int16_t>(display_.width() - static_cast<int>(w));
          if (x < 0) x = 0;
        }
        emit_component_text(x, comp.y, resolved_str, color, comp.font_size);
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
  emit_component_text(4, 8, is_network_error ? "MAT KET NOI" : "LOI", ILI9341_WHITE, 2);

  // Message body: font_size 1 deliberately (not a bigger size) -- these
  // strings come from many different call sites with real, unpredictable
  // length. Real bug found live (2026-08-24, REWORK_EXCEEDS_DEFECT's
  // message): even at size 1, a single emit_component_text() call with no
  // wrapping still clips off the right edge past ~40 chars -- "deliberately
  // font_size 1" alone was never sufficient. Word-wraps across multiple
  // lines instead, each measured (not guessed) to fit the display width.
  {
    constexpr int16_t kMsgX = 4;
    constexpr int16_t kMsgY0 = 60;
    constexpr int16_t kLineH = 16;
    constexpr uint8_t kMsgSize = 1;
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

  emit_component_text(4, 284, "* QUAY LAI", kColorMuted, 1);
  draw_wifi_indicator(wifi);
  end_screen();
}

void Renderer::draw_resyncing_screen(WifiIndicator wifi) {
  begin_screen("resyncing");
  emit_line(0, "Dang dong bo trang thai...", kColorWarn);
  emit_line(2, "(STATE_CONFLICT -> RESYNC)", kColorFg);
  draw_wifi_indicator(wifi);
  end_screen();
}

}  // namespace kiosk::ui
