#!/usr/bin/env python3
"""Generate compact monochrome Vietnamese-capable bitmap fonts for kiosk_runtime_v2.

Ported from mesflow/esp-kiosk's tools/generate_vietnamese_font.py (read-only
reference, not copied verbatim) -- the idea reused here is precomposed-
Vietnamese charset + per-glyph tight bbox + 1bpp packed bitmap + binary-
searchable sorted-by-codepoint glyph table.

2026-08-25 rewrite (font audit task): V1 generates THREE separate glyph
sets at three NATIVE pixel sizes (12/16/24px) and never scales between
them. V2 used to generate ONE base glyph set at 7px and reach every other
size via nearest-neighbor block-scaling (renderer.cpp's draw_vn_text()) --
that block-scaling is what actually made V2's Vietnamese text look
blocky/ugly (confirmed by reading the renderer, not guessed): thin
diacritic marks that are 1-2px in a 7px source become chunky disconnected
squares when scaled 2x-7x. This script now ports V1's actual strategy:
three native sizes, no scaling. Sizes chosen to match V1's exactly
(12/16/24px) so the visual proportions carry over.

Font choice: V1 uses actual Arial Bold (Windows-only, not available on
this Linux dev machine -- V1's own generator script needed a Windows font
path). Liberation Sans Bold is used instead: it's metric- and shape-
compatible with Arial by design (Red Hat's explicit Arial substitute), a
much closer match than DejaVu Sans Bold (V2's previous choice, picked only
because it happened to be installed, not for visual match) or Noto Sans.
Confirmed present on this machine with full Vietnamese coverage.

Output: firmware/kiosk_runtime_v2/src/ui/vn_font_data.h with THREE named
VnFontData instances (kVnFontSmall/kVnFontBody/kVnFontLarge, 12/16/24px)
plus kVnFont as a backward-compatible alias to kVnFontBody (existing code/
tests that only knew about one table keep working unchanged).

Usage:
  python3 tools/generate_vn_font.py

Requires Pillow and a Vietnamese-capable TrueType font (tries a short list
of common Linux paths below; pass --font /path/to/font.ttf to override,
applied to all three sizes).
"""
import argparse
import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO_ROOT = Path(__file__).resolve().parent.parent
OUT = REPO_ROOT / "firmware" / "kiosk_runtime_v2" / "src" / "ui" / "vn_font_data.h"

# (table_name, pixel_size) -- matches V1's mesflow_vietnamese_font.h exactly
# (MF_FONT_12/MF_FONT_16/MF_FONT_24) so the same three-role strategy (small
# label / body-and-questions / large-but-still-crisp) carries over.
SIZES = [("Small", 12), ("Body", 16), ("Large", 24)]

# In preference order -- Liberation Sans Bold first (Arial-metric-
# compatible, the closest available match to V1's real Arial Bold),
# falling back to other confirmed-Vietnamese-complete bold sans fonts.
CANDIDATE_FONTS = [
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
]

VI = ("ăâêôơưđĂÂÊÔƠƯĐáàảãạấầẩẫậắằẳẵặéèẻẽẹếềểễệíìỉĩịóòỏõọốồổỗộớờởỡợúùủũụứừửữựýỳỷỹỵ"
      "ÁÀẢÃẠẤẦẨẪẬẮẰẲẴẶÉÈẺẼẸẾỀỂỄỆÍÌỈĨỊÓÒỎÕỌỐỒỔỖỘỚỜỞỠỢÚÙỦŨỤỨỪỬỮỰÝỲỶỸỴ")
CHARS = sorted(set(chr(i) for i in range(32, 127)) | set(VI), key=ord)


def resolve_font(explicit):
    if explicit:
        if not os.path.exists(explicit):
            raise SystemExit(f"--font path does not exist: {explicit}")
        return explicit
    for path in CANDIDATE_FONTS:
        if os.path.exists(path):
            return path
    raise SystemExit(
        "No candidate Vietnamese-capable font found. Install one of: "
        "fonts-liberation, fonts-dejavu-core, fonts-noto-core, fonts-freefont-ttf "
        "-- or pass --font /path/to/font.ttf explicitly."
    )


def build(font_path, px_size):
    font = ImageFont.truetype(font_path, px_size)
    ascent, descent = font.getmetrics()
    bitmap, glyphs = [], []
    for ch in CHARS:
        cp = ord(ch)
        bbox = font.getbbox(ch, anchor="ls")
        x0, y0, x1, y1 = bbox
        width, height = max(0, x1 - x0), max(0, y1 - y0)
        offset = len(bitmap)
        if width and height:
            image = Image.new("1", (width, height), 0)
            ImageDraw.Draw(image).text((-x0, -y0), ch, font=font, fill=1, anchor="ls")
            bits = list(image.getdata())
            for start in range(0, len(bits), 8):
                chunk = bits[start:start + 8]
                value = 0
                for bit in chunk:
                    value = (value << 1) | bit
                value <<= max(0, 8 - len(chunk))
                bitmap.append(value)
        advance = max(1, round(font.getlength(ch)))
        # Clamp to the field widths in vn_font_core.h: width/height/advance
        # are uint8_t, xOffset/yOffset int8_t. 24px is still well within
        # range (v1's own 24px table proves it); an assertion failure here
        # means a future size grew too large for the struct, not a
        # silently-truncated glyph on-device.
        assert 0 <= width <= 255 and 0 <= height <= 255 and 0 <= advance <= 255
        assert -128 <= x0 <= 127 and -128 <= y0 <= 127
        glyphs.append((cp, offset, width, height, advance, x0, y0))
    return ascent, descent, bitmap, glyphs


def emit_table(lines, name, ascent, descent, bitmap, glyphs):
    lines.append(f"static const uint8_t kVnFontBitmap{name}[] = {{")
    for i in range(0, len(bitmap), 20):
        lines.append("  " + ",".join(f"0x{b:02X}" for b in bitmap[i:i + 20]) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"static const kiosk::protocol::VnGlyph kVnFontGlyphs{name}[] = {{")
    for cp, off, w, h, adv, x0, y0 in glyphs:
        lines.append(f"  {{{cp}u, {off}u, {w}, {h}, {adv}, {x0}, {y0}}},")
    lines.append("};")
    lines.append("")
    lines.append(
        f"static const kiosk::protocol::VnFontData kVnFont{name} = "
        f"{{kVnFontBitmap{name}, kVnFontGlyphs{name}, {len(glyphs)}, {ascent}, {descent}}};")
    lines.append("")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", default=None, help="Explicit TTF/OTF path (overrides auto-detection, applied to all sizes)")
    args = parser.parse_args()

    font_path = resolve_font(args.font)

    lines = [
        "// GENERATED FILE -- do not hand-edit.",
        "// Produced by tools/generate_vn_font.py from:",
        f"//   {font_path}",
        "// Three NATIVE sizes (12/16/24px, matching v1's mesflow_vietnamese_font.h",
        "// strategy exactly) -- no runtime scaling between them, see that file's",
        "// module docstring for why. charset = ASCII 32-126 + precomposed Vietnamese.",
        "// Regenerate with: python3 tools/generate_vn_font.py",
        "#pragma once",
        "",
        '#include "../protocol/vn_font_core.h"',
        "",
        "namespace kiosk::ui {",
        "",
    ]
    total_glyphs = 0
    for name, px in SIZES:
        ascent, descent, bitmap, glyphs = build(font_path, px)
        emit_table(lines, name, ascent, descent, bitmap, glyphs)
        total_glyphs += len(glyphs)

    lines.append(
        "// Backward-compatible alias -- code/tests written before the three-size")
    lines.append(
        "// split only knew about one table; Body (16px) is the closest match to")
    lines.append("// what they were tuned against.")
    lines.append("static const kiosk::protocol::VnFontData& kVnFont = kVnFontBody;")
    lines.append("")
    lines.append("}  // namespace kiosk::ui")
    lines.append("")

    OUT.write_text("\n".join(lines))
    print(f"Wrote {OUT} ({total_glyphs} glyphs across {len(SIZES)} sizes, font={font_path})")


if __name__ == "__main__":
    main()
