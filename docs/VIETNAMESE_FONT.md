# Vietnamese diacritic font (2026-08-24, revised 2026-08-25)

## 2026-08-25 update: the single-base-size approach below was wrong

Everything under "What V2 does differently" describes the *first* attempt:
one 7px base glyph set, reached at other sizes via nearest-neighbor
block-scaling (`fillRect(px, py, scale, scale, color)` per source pixel).
**This is what made Vietnamese text look blocky/broken** -- confirmed on
real hardware, then fixed by actually porting v1's strategy this doc
originally described but didn't follow: `tools/generate_vn_font.py` now
generates **three native sizes (12/16/24px, matching v1's own
`MF_FONT_12`/`MF_FONT_16`/`MF_FONT_24` exactly)**, no scaling between them.
`src/ui/renderer.cpp`'s new `select_vn_font()` picks the matching native
table for a given `font_size` (1->Small, 2->Body, >=3->Large with a small
integer block-scale on top, chosen so existing large sizes like the
quantity screen's `font_size=7` land at very close to their old absolute
pixel size -- `48px` vs the old `49px` -- so no screen layout needed
recomputing).

Font source also changed: **Liberation Sans Bold**, not DejaVu Sans Bold.
V1's real typeface (Arial Bold) isn't installed on this Linux dev machine
(same reason v1's own generator script needed a Windows font path);
Liberation Sans is Red Hat's purpose-built Arial-metric-compatible
substitute, a much closer match than DejaVu (which was picked only because
it happened to be installed, not for visual similarity to v1).

**A second, independent bug was found and fixed in the same pass**: this
doc's own §"The backend encoding fix" (below) said
`_bundle_json_bytes()`/`ui_bundle()` intentionally kept using plain
`jsonify()` (which escapes non-ASCII to `\uXXXX`). That was fine for the
ASCII-only bundles that existed at the time, but wrong the moment any
bundle carries real Vietnamese text -- confirmed live: a bundle with
"QUÉT THẺ NHÂN VIÊN" arrived on-device as the literal text
"QUu00c9T THu1eba...". Fixed: `_bundle_json_bytes()` now serializes with
`ensure_ascii=False`, same fix `_json_response()` already had for
`/events`/`/bootstrap`/`/state`, just never applied here until now.

Both fixes verified together on the real KIOSK-LASER-01 board: real
diacritics (`Huỳnh Thị Mơ`, `SẢN PHẨM LỖI`, `QUÉT THẺ NHÂN VIÊN`) rendering
crisp and correct, including at `font_size=7` (the giant quantity digits --
previously the single worst case for blockiness). See git history for
screenshots taken during this pass (`artifacts/debug/`).

## Why this exists

Before this, the renderer drew everything through Adafruit_GFX's built-in
default font, which is ASCII-only — every hardcoded string and every
piece of server-driven text (employee names, operation names) had to avoid
diacritics entirely (`README.md`'s "No Vietnamese glyph support at all"
limitation). Real employee/operation names in the database have real
diacritics (`Lê Văn Lý`, not `Le Van Ly`); the backend was transliterating
them away at the API boundary (`kiosk_v2.py`'s old `_ascii_safe()`) purely
because the firmware couldn't render anything else.

## What V1 does (read-only reference, `mesflow/esp-kiosk`, not copied)

Read before designing this: a custom 1bpp bitmap font (`MesflowGlyph`/
`MesflowFont` structs — codepoint/bitmapOffset/width/height/advance/
xOffset/yOffset per glyph), generated from Arial Bold via a Pillow script
at three fixed pixel sizes (12/16/24px), binary-searched by codepoint, with
a hand-rolled UTF-8 decoder and a `toVietnameseAscii()` fallback used
elsewhere in that codebase. Reused here: the general shape (tight-bbox
1bpp glyphs, sorted-by-codepoint binary search, UTF-8 decode). Not reused:
V1's three-separate-fixed-pixel-size approach, and V1's font source
(Windows-only `arialbd.ttf` path).

## What V2 does differently

**One base glyph set, not three.** V2's `font_size` is an integer *scale*
factor (Adafruit_GFX's own `setTextSize(N)` semantics), already used
uniformly everywhere before this — bundle-declared per-component
`font_size`, the auto-shrink-to-fit loop in `draw_from_bundle()`'s TEXT
case, the quantity screen's literal `font_size=7`. Generating three
separate glyph sets per fixed pixel size (V1's approach) would mean
maintaining a second table any of that code would have to pick between;
instead there's exactly one base-size glyph set
(`src/ui/vn_font_data.h`, generated), and the renderer scales it by
nearest-neighbor blit at draw time — the exact same trick Adafruit_GFX
itself uses for its own bitmap font at `setTextSize(N)`.

**Pure core / Arduino-adapter split**, matching this project's established
convention (`event_journal_core.h` vs `storage/event_journal.h`):
- `src/protocol/vn_font_core.h/.cpp` — `VnGlyph`/`VnFontData` structs,
  `utf8_decode_next()`, `find_glyph()` (binary search), `measure_vn_text()`.
  Plain C++, no Arduino, no PROGMEM — host-tested in
  `test/host/test_vn_font_core.cpp`.
- `src/ui/vn_font_data.h` — **generated**, the actual glyph/bitmap arrays.
  Also plain C++ (ESP32's flash is memory-mapped/byte-addressable like
  normal RAM, unlike classic AVR Arduino, so no PROGMEM/pgm_read_byte is
  needed — the same array works unchanged on-device and in a host test).
- `src/ui/renderer.cpp`'s `draw_vn_text()` — the actual pixel blit
  (`display_.drawPixel()`/`fillRect()` for scale>1), Arduino/display-
  dependent, so it stays out of the pure core.

**Base size chosen for parity, not novelty.** DejaVu Sans Bold at TTF size
7 was picked specifically because it measures close to the OLD GFX
default font's proportions (ascent=7, descent=2, 'A' advance ~6px at
scale 1) — swapping the font engine without also silently changing every
hand-tuned x/y position throughout `renderer.cpp`.

**Regenerating the font**: `python3 tools/generate_vn_font.py` (needs
Pillow + a Vietnamese-capable TrueType font; tries DejaVu Sans Bold first,
falls back to Noto/Liberation/FreeSans Bold — see that script's own
`CANDIDATE_FONTS` list). Regenerates `src/ui/vn_font_data.h` in place.

## Base font size vs. readability — two separate decisions

The base glyph's *pixel size* (chosen for GFX-parity, above) is not the
same decision as what *scale* the renderer draws at. Real user feedback
the same day this shipped: scale 1 (~7px glyph height) was still too small
to read on the physical display. `emit_line()` (every hardcoded screen)
and the operator-facing "transient message" text
(`Renderer::emit_transient_message()`) now default to scale 2, **auto-
shrinking to scale 1 only if the string would overflow the screen width at
2** — the same try-the-preferred-size-then-step-down pattern
`draw_from_bundle()`'s TEXT case already used for bundle-driven text. A
handful of the longer hardcoded status/rejection strings (e.g. "SO LUONG
SUA KHONG DUOC LON HON SO LUONG LOI") measure wider than the screen at
scale 2 and fall back to scale 1 automatically — verified against every
hardcoded literal in `renderer.cpp` via a host-side measurement pass
before this shipped, not discovered later on the physical device.

## What got real diacritics restored, and what's staying ASCII on purpose

Restored (this pass, 2026-08-24):
- Every hardcoded/developer-authored string in `renderer.cpp` and
  `kiosk_runtime.cpp` (boot screen, Wi-Fi recovery, error views, business
  state screens, quantity flow).
- The Wi-Fi setup portal's HTML page (`wifi_setup_portal.cpp`) — this was
  never actually blocked by the kiosk's own font at all (a phone's browser
  renders full Unicode regardless), it just hadn't been done yet.
- The backend's operator-facing business messages
  (`kiosk_v2.py` — `_apply_event()`'s rejection strings, `_canonical_error()`)
  and `employee_name`/`operation_name` themselves (the old `_ascii_safe()`
  transliteration is gone — see below).

Left in English/technical form on purpose (matches this project's existing
convention of not translating genuinely technical strings):
- Structured log messages (`kiosk::health::log_structured(...)`) — always
  English, regardless of UI language, throughout this whole project.
- Firmware version/build/hardware diagnostic lines on the boot screen
  (`fw %s (%s)`, `heap %u KB`, etc.) and DEV-only debug labels
  (`identity: ACTIVE`) — technician-facing, not operator-facing.
- Wire-level enum/state names surfaced verbatim for debugging
  (`QUANTITY_INPUT (thieu view)`, `(STATE_CONFLICT -> RESYNC)`).

## The backend encoding fix this depended on

Restoring real diacritics in the *data* (removing `_ascii_safe()`) only
matters if the bytes actually survive the trip to the device. Two separate
things had to both be true:

1. **`json_extract.cpp` (this firmware) already passes raw UTF-8 through
   unchanged.** Verified by reading it: the string-value scanner only
   special-cases `\` and `"` (both ASCII); any byte ≥0x80 (every UTF-8
   continuation/lead byte) falls through the `else` branch and gets copied
   verbatim. No firmware change was needed for this part.
2. **Flask's `jsonify()` escapes non-ASCII to `\uXXXX` by default** — and
   this firmware's parser (deliberately not a full JSON library) never
   decoded `\uXXXX` escapes, which is the exact bug the OLD `_ascii_safe()`
   was working around ("Lê Văn Lý" arrived as the literal text
   "Lu00ea Vu0103n Lu00fd"). `kiosk_v2.py`'s new `_json_response()` helper
   serializes with `ensure_ascii=False` instead, scoped to just the kiosk_v2
   responses that can carry these fields (deliberately NOT a global
   `app.json.ensure_ascii=False` change — that would affect every other
   Flask blueprint sharing that app).

Both were required; either alone would still show garbled text on screen.

**2026-08-25 correction**: `_bundle_json_bytes()`/`ui_bundle()` were said
above to intentionally keep plain `jsonify()` (ensure_ascii=True) "as a
different concern" from the hash-consistency requirement. That was wrong
-- it's the exact same bug, just not caught yet because no bundle
contained Vietnamese text at the time. Fixed: `_bundle_json_bytes()` now
uses `json.dumps(..., ensure_ascii=False, sort_keys=True)` directly (still
the single function both the hash and the served bytes are derived from,
so the "never drift from what's actually served" property is unchanged --
just with the right escaping).
