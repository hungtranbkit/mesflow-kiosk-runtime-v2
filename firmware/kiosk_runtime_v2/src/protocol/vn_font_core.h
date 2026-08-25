#pragma once

#include <cstdint>

namespace kiosk::protocol {

// Vietnamese-capable bitmap font -- pure C++ core (UTF-8 decode, glyph
// lookup, measurement), no Arduino/display dependency, so it's host-
// testable exactly like the rest of src/protocol/. The actual pixel blit
// (display_.drawPixel(), Arduino-dependent) lives in src/ui/renderer.cpp.
//
// Design note (2026-08-24, replacing the stock ASCII-only Adafruit_GFX
// font): V1 (mesflow/esp-kiosk, read-only reference) proved a custom 1bpp
// bitmap font with a {codepoint, bitmapOffset, width, height, advance,
// xOffset, yOffset} per-glyph table, binary-searched by codepoint, works
// well for this. V2 keeps that same shape (see VnGlyph below) but as ONE
// base-size glyph set rather than V1's three separate fixed-pixel-size
// sets (12/16/24px) -- V2's font_size is an integer SCALE factor
// (Adafruit_GFX's own setTextSize(N) semantics: font_size=1 is the base
// glyph, font_size=N draws each source pixel as an NxN block), used
// uniformly everywhere already (bundle-declared per-component font_size,
// the auto-shrink-to-fit loop in draw_from_bundle(), the quantity screen's
// literal font_size=7) -- a single glyph set that scales, rather than a
// second maintained glyph set per fixed pixel size, is what keeps all of
// that working unchanged.
//
// ESP32 note: unlike classic AVR Arduino, ESP32's flash is memory-mapped
// and byte-addressable like normal RAM -- no PROGMEM/pgm_read_byte needed
// to read `bitmap`/`glyphs` below, which is also exactly why this data can
// be a plain `static const` array usable directly in a host unit test
// (see src/ui/vn_font_data.h, generated) with no on-device-only accessor.
struct VnGlyph {
  uint32_t codepoint;
  uint32_t bitmap_offset;  // BYTE offset into VnFontData::bitmap where this glyph's packed rows start
  uint8_t width;
  uint8_t height;
  uint8_t advance;   // base-scale (font_size=1) horizontal advance, pixels
  int8_t x_offset;   // base-scale offset from pen x to the glyph's left edge
  int8_t y_offset;   // base-scale offset from the text baseline to the glyph's top edge (usually negative)
};

struct VnFontData {
  const uint8_t* bitmap;      // packed 1bpp rows, MSB-first, row-major per glyph (see vn_font_data.h)
  const VnGlyph* glyphs;      // sorted ascending by codepoint -- find_glyph() binary-searches this
  uint16_t glyph_count;
  uint8_t ascent;   // base-scale (font_size=1) baseline-to-top distance
  uint8_t descent;  // base-scale baseline-to-bottom distance
};

// Decodes one UTF-8 codepoint starting at `*cursor` and advances `*cursor`
// past it. Malformed/truncated sequences decode as U+FFFD (replacement
// character) and advance by exactly 1 byte, so a corrupt string can never
// read past its own end or loop forever -- same "never guess, never crash"
// posture as the rest of this project's parsers. Returns 0 (and does not
// advance further) if `**cursor == '\0'` -- callers loop `while (*cursor)`.
uint32_t utf8_decode_next(const char** cursor);

// Binary-searches `font.glyphs` (must be sorted ascending by codepoint,
// which the generator guarantees) for `codepoint`. Returns true and fills
// `*out` on a hit; returns false (leaving `*out` untouched) on a miss --
// callers fall back to a fixed advance (never silently draw nothing at the
// wrong cursor position, matching V1's own missing-glyph handling).
bool find_glyph(const VnFontData& font, uint32_t codepoint, VnGlyph* out);

// Sums each character's `advance` (falling back to `font.ascent / 2` for a
// codepoint with no glyph, matching V1's exact fallback) across the whole
// UTF-8 string, scaled by `font_size` (an integer >=1 multiply-scale,
// never 0 -- callers should clamp before calling). Pure/host-testable: this
// is what backs Renderer::measure_text_width() once wired in.
int32_t measure_vn_text(const VnFontData& font, const char* text, uint8_t font_size);

}  // namespace kiosk::protocol
