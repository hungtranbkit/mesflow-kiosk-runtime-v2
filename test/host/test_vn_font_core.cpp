// Host test: plain C++, no Arduino. Covers the Vietnamese bitmap font's
// pure core -- UTF-8 decode, glyph binary search, measurement -- ahead of
// wiring it into the (Arduino-dependent) renderer's actual pixel blit.
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../../firmware/kiosk_runtime_v2/src/protocol/vn_font_core.h"
#include "../../firmware/kiosk_runtime_v2/src/ui/vn_font_data.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

int main() {
  using namespace kiosk::protocol;
  using kiosk::ui::kVnFont;

  std::printf("test_vn_font_core\n");

  // --- utf8_decode_next(): pure ASCII ---
  {
    const char* text = "Hi";
    const char* cursor = text;
    uint32_t c1 = utf8_decode_next(&cursor);
    uint32_t c2 = utf8_decode_next(&cursor);
    uint32_t c3 = utf8_decode_next(&cursor);
    check(c1 == 'H' && c2 == 'i' && c3 == 0, "ASCII decodes byte-by-byte, then 0 at end of string");
  }

  // --- utf8_decode_next(): real Vietnamese text ---
  {
    const char* text = "Nguyễn Vũ Hùng";  // real 3-byte-sequence diacritics
    const char* cursor = text;
    uint32_t codepoints[32];
    int n = 0;
    while (*cursor && n < 32) {
      codepoints[n++] = utf8_decode_next(&cursor);
    }
    check(n > 0, "decodes a real Vietnamese name without hanging/crashing");
    bool saw_e_hook = false;
    for (int i = 0; i < n; ++i) {
      if (codepoints[i] == 0x1EC5) saw_e_hook = true;  // 'ễ' U+1EC5
    }
    check(saw_e_hook, "decodes U+1EC5 ('ễ') correctly from its 3-byte UTF-8 sequence");
  }

  // --- utf8_decode_next(): malformed input never hangs or reads past end ---
  {
    const char truncated[] = {static_cast<char>(0xE1), static_cast<char>(0xBB), '\0'};  // truncated 3-byte seq
    const char* cursor = truncated;
    uint32_t cp = utf8_decode_next(&cursor);
    check(cp == 0xFFFD, "truncated multi-byte sequence decodes as U+FFFD, not garbage");
    check(cursor == truncated + 2, "cursor stops at the embedded NUL, does not read past the buffer");
  }
  {
    const char stray[] = {static_cast<char>(0x80), 'X', '\0'};  // stray continuation byte as lead
    const char* cursor = stray;
    uint32_t cp = utf8_decode_next(&cursor);
    check(cp == 0xFFFD, "a stray continuation byte as a lead byte decodes as U+FFFD");
    check(cursor == stray + 1, "advances by exactly 1 byte on garbage, not 0 (no infinite loop) or more");
    uint32_t cp2 = utf8_decode_next(&cursor);
    check(cp2 == 'X', "decoding resumes correctly on the next real character after garbage");
  }

  // --- find_glyph(): real generated font data ---
  {
    VnGlyph g;
    bool found_A = find_glyph(kVnFont, 'A', &g);
    check(found_A && g.codepoint == 'A', "finds the glyph for plain ASCII 'A'");

    bool found_e_hook = find_glyph(kVnFont, 0x1EC5, &g);  // 'ễ'
    check(found_e_hook && g.codepoint == 0x1EC5, "finds the glyph for 'ễ' (U+1EC5)");

    bool found_dd = find_glyph(kVnFont, 0x0111, &g);  // 'đ'
    check(found_dd, "finds the glyph for 'đ' (U+0111, đ with stroke)");

    bool found_missing = find_glyph(kVnFont, 0x4E2D, &g);  // a CJK char, not in this font's subset
    check(!found_missing, "a codepoint outside the subset (e.g. CJK) is correctly reported as not found");
  }

  // --- find_glyph(): glyph table really is sorted (binary search precondition) ---
  {
    bool sorted = true;
    for (uint16_t i = 1; i < kVnFont.glyph_count; ++i) {
      if (kVnFont.glyphs[i - 1].codepoint >= kVnFont.glyphs[i].codepoint) {
        sorted = false;
        break;
      }
    }
    check(sorted, "generated glyph table is strictly sorted ascending by codepoint");
  }

  // --- measure_vn_text() ---
  {
    int32_t w_empty = measure_vn_text(kVnFont, "", 1);
    check(w_empty == 0, "measuring an empty string returns 0, not garbage (a real bug found once in the GFX path)");

    int32_t w_a = measure_vn_text(kVnFont, "A", 1);
    check(w_a > 0, "measuring a single real glyph returns a positive width at scale 1");

    int32_t w_a_scale2 = measure_vn_text(kVnFont, "A", 2);
    check(w_a_scale2 == w_a * 2, "font_size=2 scales the measured width by exactly 2x");

    int32_t w_a_scale0 = measure_vn_text(kVnFont, "A", 0);
    check(w_a_scale0 == w_a, "font_size=0 is treated the same as font_size=1, not zero-width");

    int32_t w_vn = measure_vn_text(kVnFont, "Nguyễn", 1);
    check(w_vn > w_a, "a real multi-character Vietnamese word measures wider than a single glyph");

    int32_t w_unknown = measure_vn_text(kVnFont, "\xE4\xB8\xAD", 1);  // U+4E2D, not in the subset
    check(w_unknown == kVnFont.ascent / 2, "an unrenderable codepoint falls back to ascent/2 width, matching V1's own fallback");
  }

  // --- Three native sizes (2026-08-25 font audit): each table sorted,
  // covers the same charset, and is genuinely a DIFFERENT (bigger) native
  // glyph as size increases -- not the same 7px source relabeled. This is
  // what host-tests the actual root-cause fix (a single 7px base scaled by
  // nearest-neighbor block-fill, replaced with v1's proven three-native-
  // size strategy); the table-SELECTION logic itself lives in
  // renderer.cpp (Arduino-dependent, not host-testable), verified on real
  // hardware instead -- see docs/VIETNAMESE_FONT.md.
  {
    using kiosk::ui::kVnFontSmall;
    using kiosk::ui::kVnFontBody;
    using kiosk::ui::kVnFontLarge;

    check(kVnFontSmall.ascent < kVnFontBody.ascent && kVnFontBody.ascent < kVnFontLarge.ascent,
          "Small < Body < Large ascent -- three genuinely different native sizes, not one relabeled");
    check(kVnFontSmall.glyph_count == kVnFontBody.glyph_count &&
          kVnFontBody.glyph_count == kVnFontLarge.glyph_count,
          "all three sizes cover the identical charset (no size-specific glyph gaps)");

    for (const auto* f : {&kVnFontSmall, &kVnFontBody, &kVnFontLarge}) {
      bool sorted = true;
      for (uint16_t i = 1; i < f->glyph_count; ++i) {
        if (f->glyphs[i - 1].codepoint >= f->glyphs[i].codepoint) { sorted = false; break; }
      }
      check(sorted, "native table is strictly sorted ascending by codepoint");

      VnGlyph g;
      check(find_glyph(*f, 0x0110, &g), "Đ (U+0110) present in this native table");
      check(find_glyph(*f, 0x1EC5, &g), "ễ (U+1EC5, a combining-tone-mark case) present in this native table");
    }

    // The actual complaint this task investigates: at V2's OLD strategy
    // (one 7px base, block-scaled), 'A' at font_size=2 measured EXACTLY
    // 2x its font_size=1 width (pure integer scaling, no real different
    // glyph metrics). With native tables, Body's own 'A' advance need not
    // be an exact multiple of Small's -- proving Body is a genuinely
    // separately-rendered glyph, not Small stretched.
    VnGlyph gs, gb;
    find_glyph(kVnFontSmall, 'A', &gs);
    find_glyph(kVnFontBody, 'A', &gb);
    check(gb.width > gs.width, "Body's 'A' glyph is natively wider/taller than Small's, not a 2x block-scale of it");
  }

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
