# Backend-Controlled UI (Phase 4 design)

No browser, no React/Vue/HTML/CSS/JS runtime. Design:

```text
backend authoring (JSON) -> compile -> compact UI bundle -> device -> native renderer
```

**Status (corrected 2026-08-25): PARTIALLY IMPLEMENTED, not schema-only.**
This page's original framing ("nothing in this repository parses a bundle
today") predates Phase 4 actually landing and is now stale — a real bundle
parser/renderer (`src/protocol/ui_bundle.h/.cpp`,
`Renderer::draw_from_bundle()`), server-side bundle storage
(`kiosk_v2_ui_bundles`/`kiosk_v2_ui_desired`), and a bootstrap-time sync
controller are all implemented and exercised on real hardware (see
`docs/KIOSK_UI_GUIDE.md`). What's actually shipped is a real but
DELIBERATELY NARROWER subset of the full aspirational schema below: only
`TEXT`/`RECT`/`LINE` component types, `x`/`y`/`color`/`text`/`font_size`/
`align` fields (`align:"center"` auto-shrinks on overflow), `font_size`
2 (Body/16px) or 3 (Large/24px) for anything meant to match the current
two-size hardcoded design. `container`/`button`/`progress`/`spinner`/
`keypad`/`icon`/`divider` and the rest of the component whitelist below
remain unimplemented target design, same as before. Treat every section
below as the long-term target contract, not a description of what
`ui_bundle.h`'s parser accepts today.

## Component whitelist

```text
screen
container
label
value
button
status
progress
spinner
alert
keypad
icon
divider
```

Extend only when a real screen actually needs something not on this list.

## Typography and geometry — full component shape

A component is not just `{"type":"label","text":"..."}`. The schema must
carry enough for pixel-accurate rendering, e.g.:

```json
{
  "type": "label",
  "id": "instruction",
  "text": "QUÉT THẺ NHÂN VIÊN",
  "rect": { "x": 12, "y": 42, "w": 216, "h": 48 },
  "text_style": {
    "font": "title",
    "align_horizontal": "CENTER",
    "align_vertical": "MIDDLE",
    "line_height": 28,
    "letter_spacing": 0,
    "wrap": true,
    "max_lines": 2,
    "overflow": "ELLIPSIS"
  },
  "color": "#FFFFFF",
  "z": 10
}
```

Backend must be able to control, precisely:

```text
x, y, width, height
font family, font size, font weight
horizontal alignment, vertical alignment
line height, letter spacing
text wrapping, maximum lines, overflow behavior
padding, margin
foreground color, background color
border, radius
visibility, z-order
```

Full schema field list to reserve from v1 (not all effects need an
implementation immediately, but the schema/capability model must be
unambiguous from the start — see `docs/DEVICE_LIFECYCLE.md`'s compatibility
matrix for how a future field gets added without breaking older runtimes):

```text
rect: x, y, w, h
font, font_size, font_weight
horizontal_align, vertical_align, line_height, letter_spacing
wrap, max_lines, overflow
padding, margin
background, foreground, border, radius
z
visible_if, enabled_if
```

## Coordinate system and display profile

No pixel layout hardcoded ad hoc in firmware. Every screen targets a
specific viewport, declared once per device profile:

```json
{ "display": { "width": 320, "height": 240, "rotation": 1 } }
```

A UI bundle compiles for exactly one `display_profile`, e.g.
`ESP32_ILI9341_320x240_LANDSCAPE`. A bundle built for 240x320 must never be
sent to a device running 320x240 hoping the renderer "figures it out" — the
compatibility matrix (`docs/DEVICE_LIFECYCLE.md`) rejects that combination
outright, at bundle-selection time, not at render time.

### Safe area

Each display profile declares a safe area:

```json
{ "safe_area": { "top": 4, "left": 4, "right": 4, "bottom": 4 } }
```

Normal components may not render outside it. Exceptions: background,
fullscreen overlay, diagnostic screen.

## Layout modes

Support `ABSOLUTE`, `ROW`, `COLUMN`, `STACK` — no CSS engine. `ABSOLUTE` for
pixel-perfect kiosk screens (quantity input, numeric keypad, status
screen); `ROW`/`COLUMN` for simpler screens to reduce how many coordinates
an author has to enter by hand.

## Fonts are resource IDs, never arbitrary family names

Firmware only maps a font ID to a preloaded font resource:

```text
mesflow_regular_14
mesflow_regular_18
mesflow_medium_20
mesflow_bold_24
mesflow_bold_32
mesflow_number_48
```

The backend/bundle can never send `"font": "Arial"` or any family name the
firmware hasn't been built with — this is a whitelist, not a request.

### Font registry (bundle manifest)

```json
{
  "fonts": {
    "body": { "resource": "font_body_18", "size": 18 },
    "title": { "resource": "font_bold_24", "size": 24 },
    "number_large": { "resource": "font_bold_48", "size": 48 }
  }
}
```

Screens reference `"text_style": "title"` (or a safe override), not raw
font resources directly — so a theme change doesn't require editing every
screen.

### Font assets: build-time, not runtime

Never download an arbitrary TTF/OTF and parse it at runtime on the ESP32.
Flow:

```text
TTF/OTF source -> backend/build tool -> subset glyphs -> compile font asset -> bundle -> ESP renderer
```

**Vietnamese glyph coverage is mandatory, not optional.** Subset at minimum:
`ă â ê ô ơ ư`, `đ`, and the full tone-mark set (`á à ả ã ạ`, `ấ ầ ẩ ẫ ậ`,
etc. — every base vowel × every tone combination actually used in kiosk
copy). Testing only ASCII is not acceptable coverage.

**Bundle compiler must FAIL the publish**, not warn, if a screen's text
requires a glyph the selected font doesn't have (e.g. a screen containing
`"Sản lượng đạt"` against a font missing `ả`). Nobody should discover a
tofu/□ glyph for the first time on a kiosk on the shop floor.

## Deterministic text measurement

The backend's preview and the ESP renderer must use the *same* font
metrics, glyph sizes, line-height rules, and wrapping rules — otherwise a
backend preview looks fine while the device overflows. The bundle compiler
should pre-compute measured width/height/line-count per text component and
validate the screen against it before publish (§"Screen constraints"
below), rather than discovering overflow live on a device.

## Overflow policy

Every text component picks exactly one, explicitly — the renderer never
decides on its own:

```text
CLIP
ELLIPSIS
WRAP
SHRINK_TO_FIT
```

`SHRINK_TO_FIT` requires `min_font_size`:

```json
{ "overflow": "SHRINK_TO_FIT", "font_size": 28, "min_font_size": 20 }
```

Never shrink to something illegible (e.g. 8px) just to fit more text.

## Dynamic text must be tested at realistic lengths

Fields like `employee_name`, `operation_name`, `part_name`, `PO` vary in
length. Test with short/normal/maximum-supported values and Vietnamese
diacritics — e.g. `"Nguyễn Hoàng Minh Anh"` must not break the layout. The
bundle simulator (`tools/`, not yet built) should include long-text cases
for exactly this.

## Screen constraints (bundle compiler must reject)

```text
component outside screen bounds
negative width/height
overlap with a forbidden region (e.g. safe area exception list)
font not available on the target runtime
unsupported style property
text guaranteed to overflow given deterministic measurement
too many components (see component budget)
asset memory budget exceeded
```

## Z-order

Simple integer range, no compositor: `z: 0-99`.

## Pixel-perfect preview

The future backend Kiosk UI Editor must preview against the *actual*
device profile (e.g. ILI9341 320x240 rotation 1) — actual aspect ratio,
actual font metrics, actual safe area, actual wrapping. Not a generic
"responsive browser" preview.

## Theme system

Typography and spacing live mostly in a theme, so a fleet-wide style change
doesn't require editing every screen:

```json
{
  "theme": {
    "colors": {
      "background": "#101828", "primary": "#1570EF", "success": "#12B76A",
      "warning": "#F79009", "error": "#F04438", "text": "#FFFFFF"
    },
    "spacing": { "xs": 4, "sm": 8, "md": 12, "lg": 16, "xl": 24 },
    "typography": { "body": "body_18", "title": "bold_24", "number": "bold_48" }
  }
}
```

## Recommended type scale for a 2.8" kiosk display

Starting point, to be benchmarked against real hardware/viewing distance
before treating as final:

```text
Small/support     14-16 px
Body              18 px
Important         20-22 px
Title             24-28 px
Quantity          40-56 px
Critical alert    24-32 px
```

Never shrink text just to cram in more data — the goal is fast reading at
actual operating distance on the shop floor.

## Component / memory budgets (targets, not yet enforced by any compiler)

```text
max components/screen: 40
max nested depth: 6
max dynamic labels: 20
max text bytes/component: 256
max image assets/screen: 8
```

Font budget: prefer 1 font family, up to 3 weights (Regular/Medium/Bold),
a handful of fixed sizes — not ten different font families. Vietnamese
glyph sets are not free (flash/PSRAM cost); benchmark glyph count, font
asset bytes, PSRAM cache use, and render latency before adding a font.

These numbers must be benchmarked against Phase 0's real hardware
(`docs/ARCHITECTURE.md` resource targets) and adjusted, not treated as
final on paper.

## Safe conditional grammar

```json
{ "visible_if": { "field": "session.quantity_good", "op": ">", "value": 0 } }
```

Whitelisted ops: `== != > >= < <= exists in and or not`. Never: eval,
function call, loop, network expression, arbitrary regex. Fields read only
from `view_model`, `device_capabilities`, `feature_flags`. Conditions affect
presentation only — the backend still enforces authorization/business rules
independently; a hidden button is not an access control.

## View model

Backend responses should carry everything a screen needs to render in one
shot — the device should not need several API calls to draw one screen.

```json
{
  "state": "SESSION_ACTIVE",
  "state_version": 8832,
  "view": {
    "employee_name": "Nguyen Van A",
    "po": "PO-123",
    "part": "BRACKET",
    "operation": "Chan",
    "started_at": "13:42",
    "produced": 20,
    "target": 100
  }
}
```

## Runtime bundle format

JSON is authoring/debug only. Runtime format should be CBOR or a simple
custom binary — whichever is simpler while still safe. No generic DOM
engine. The bundle is indexed once after load, not parsed as a tree every
frame.

## Bundle contents & manifest

```text
manifest (bundle_version, schema_version, min/max_runtime_version,
          per-file hashes, bundle hash, signature, font registry)
screens
theme
assets
workflow references
```

Activation flow:

```text
download to staging -> verify hash -> verify signature -> schema validation
  -> resource limits -> runtime compatibility check -> Vietnamese glyph
  coverage check -> activate atomically
```

Any failure at any step -> stay on last-known-good bundle. No partial load.

## Emergency UI is out of scope for this document

Built-in emergency screens (boot, safe mode, Wi-Fi recovery, fatal error,
OTA recovery) are never sourced from a bundle — see
`docs/ARCHITECTURE.md` "Emergency UI priority" and `docs/WIFI_RECOVERY.md`.
This document only covers the backend-controlled *normal operation* UI.

## Relationship to Phase 0

Phase 0's `renderer.h/.cpp` draws a small fixed set of hardcoded screens
(boot diagnostics, "waiting for scan", scan feedback, keypad calibration,
Wi-Fi recovery) using the same component vocabulary above (label/value/
status) so that swapping in a real bundle-driven renderer later is
additive, not a rewrite.
