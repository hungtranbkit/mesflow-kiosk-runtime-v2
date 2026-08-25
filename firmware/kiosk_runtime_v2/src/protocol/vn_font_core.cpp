#include "vn_font_core.h"

namespace kiosk::protocol {

uint32_t utf8_decode_next(const char** cursor) {
  const uint8_t a = static_cast<uint8_t>(**cursor);
  if (a == 0) return 0;
  ++*cursor;

  if (a < 0x80) return a;

  auto continuation_byte = [&]() -> int {
    const uint8_t b = static_cast<uint8_t>(**cursor);
    if ((b & 0xC0) != 0x80 || b == 0) return -1;  // not a valid continuation byte (or end of string)
    ++*cursor;
    return b & 0x3F;
  };

  if ((a & 0xE0) == 0xC0) {
    int b = continuation_byte();
    if (b < 0) return 0xFFFD;
    return static_cast<uint32_t>(((a & 0x1F) << 6) | b);
  }
  if ((a & 0xF0) == 0xE0) {
    int b = continuation_byte();
    if (b < 0) return 0xFFFD;
    int c = continuation_byte();
    if (c < 0) return 0xFFFD;
    return static_cast<uint32_t>(((a & 0x0F) << 12) | (b << 6) | c);
  }
  if ((a & 0xF8) == 0xF0) {
    int b = continuation_byte();
    if (b < 0) return 0xFFFD;
    int c = continuation_byte();
    if (c < 0) return 0xFFFD;
    int d = continuation_byte();
    if (d < 0) return 0xFFFD;
    return static_cast<uint32_t>(((a & 0x07) << 18) | (b << 12) | (c << 6) | d);
  }
  // Invalid lead byte (0x80-0xBF stray continuation, or 0xF8-0xFF) --
  // already consumed exactly one byte above, matches the "advance by 1 on
  // garbage" contract.
  return 0xFFFD;
}

bool find_glyph(const VnFontData& font, uint32_t codepoint, VnGlyph* out) {
  int lo = 0, hi = static_cast<int>(font.glyph_count) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const VnGlyph& candidate = font.glyphs[mid];
    if (candidate.codepoint == codepoint) {
      if (out) *out = candidate;
      return true;
    }
    if (candidate.codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return false;
}

int32_t measure_vn_text(const VnFontData& font, const char* text, uint8_t font_size) {
  if (!text) return 0;
  const uint8_t scale = font_size == 0 ? 1 : font_size;
  int32_t width = 0;
  const char* cursor = text;
  while (*cursor) {
    const uint32_t cp = utf8_decode_next(&cursor);
    if (cp == 0) break;
    VnGlyph glyph;
    const int32_t advance = find_glyph(font, cp, &glyph) ? glyph.advance : font.ascent / 2;
    width += advance * scale;
  }
  return width;
}

}  // namespace kiosk::protocol
