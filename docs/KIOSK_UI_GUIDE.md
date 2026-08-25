# Kiosk V2 UI Guide

**Status: current design, in effect since the 2026-08-25 UI Consistency
Cleanup.** This is the one place any screen-level layout/typography rule is
documented — if a `draw_*screen()` function in `src/ui/renderer.cpp`
disagrees with this doc, the doc is stale; fix whichever one is wrong,
don't let a third convention grow in a comment somewhere else.

## Why this exists

Before this cleanup, screen code picked its own font size and Y coordinate
ad hoc per screen (some `setTextSize(1)`, some `(2)`, some raw pixel
math), Vietnamese text rendered blocky (single 7px base font,
nearest-neighbor-scaled — see `docs/VIETNAMESE_FONT.md`), and long
employee/operation names could silently clip off the 240×320 screen. This
guide is the result of fixing all three in one pass: one small set of named
constants, one shared set of render helpers, applied uniformly.

## The rule: exactly two font roles

```
FONT_SMALL = kFontSmall = 2   -> kVnFontBody,  16px native, scale 1
FONT_LARGE = kFontLarge = 3   -> kVnFontLarge, 24px native, scale 1
```

No `draw_*screen()` function picks a third size, and none calls
`setTextSize()`/`emit_component_text()` with a raw numeric font size
directly — every screen goes through the shared helpers below, which are
the only code in the renderer allowed to reference `kFontSmall`/
`kFontLarge` for hardcoded screens.

There is a third *native* glyph table (`kVnFontSmall`, 12px) and a
`kFontValueScale` (Large glyphs block-scaled ×2, ~48px) — see "The one
exception" below for why those aren't a third or fourth typographic role.

**Where the two sizes come from**: `select_vn_font()` in `renderer.cpp`
maps a `font_size` byte onto one of the three *native* glyph tables in
`src/ui/vn_font_data.h` (12/16/24px — ported from v1's proven multi-size
strategy, see `docs/VIETNAMESE_FONT.md`). Screen-level code only ever asks
for 2 or 3; `font_size` 1 and ≥4 still decode (backward-compatible wire
format for UI bundles — see "Server-pushed UI bundles" below) but no
hardcoded screen uses them.

## The one exception: the giant quantity digit

`kFontValueScale = 6` selects the Large (24px) table block-scaled ×2
(~48px) — used by exactly one helper, `draw_value_giant()`, for the single
digit-entry emphasis case (`QUANTITY_INPUT` and its defect/rework
variants). This is not a third typographic *role*: it's still
conceptually "Large," just drawn with extra visual weight for the one
screen where an operator needs to read a running count from arm's length.
No screen calls `select_vn_font(6)` or an equivalent raw value directly —
only `draw_value_giant()` does.

## Fixed screen geometry

Every normal screen has the same three zones; only the content zone's
content changes:

```
y=0   ┌─────────────────────────────┐
      │  status bar (24px): signal   │  <- draw_status_bar()
y=24  │  bars + 4-char SSID prefix   │
      ├─────────────────────────────┤
      │                               │
      │        content zone           │  <- draw_title_2line() /
      │      (title / body text)      │     draw_fit_text() / etc.
      │                               │
y=290 ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┤  <- kFooterDividerY (286)
      │  footer (30px): hint text     │  <- draw_footer()
y=320 └─────────────────────────────┘
```

```cpp
kScreenW = 240,  kScreenH = 320,  kMarginX = 8
kHeaderH = 24,   kFooterH = 30
kContentTop = 24,  kContentBottom = 290
kFooterDividerY = 286,  kFooterY = 298
```

These never move between screens (§7/§25 of the original task). A screen
that doesn't need a footer just doesn't call `draw_footer()` — the content
zone doesn't expand to fill the gap, keeping every screen's vertical
rhythm predictable.

## Spacing and line-height scale

```cpp
kSpacingSmall = 4,  kSpacingNormal = 8,  kSpacingLarge = 16
kLineHeightSmall = 22,   // FONT_SMALL line pitch
kLineHeightLarge = 34    // FONT_LARGE line pitch (title_2line's 2nd line offset)
```

## Shared render helpers (`src/ui/renderer.h`/`.cpp`)

Every hardcoded screen is built from these plus `begin_screen()`/
`end_screen()`/`draw_status_bar()` — nothing else.

| Helper | Use for |
|---|---|
| `draw_status_bar(wifi)` | Top-right signal bars + 4-char SSID prefix. Called by (almost) every screen. |
| `draw_title_line(text, y, color)` | ONE known-short static literal, FONT_LARGE, centered. Only for text already chosen to fit — never variable-length data. |
| `draw_title_2line(line1, line2, y_top, color)` | Two fixed FONT_LARGE lines as a centered block (`kLineHeightLarge` apart) — the "SẴN SÀNG / QUÉT MÃ" pattern every wait-state screen uses. |
| `draw_fit_text(text, y_top, color, prefer_large)` | **The one policy for variable-length content** (employee/operation names, server error messages). See ladder below. |
| `draw_value_giant(text, y, color)` | The one emphasized-digit case (quantity entry). Not for anything else. |
| `draw_footer(left, right)` | Bottom hint row + divider line. Either side may be `""` to skip it. |

### `draw_fit_text()`'s fit ladder

For text whose length isn't known ahead of time, try in order, first one
that fits wins:

1. **LARGE, one line** (only if `prefer_large=true` — most callers pass
   `false` since the text is a detail line under a title, not the title
   itself).
2. **SMALL, one line.**
3. **SMALL, word-wrapped to 2 lines** (breaks at the last space that still
   keeps line 1 within the screen width — never mid-glyph).
4. **Ellipsis** — applied to *both* line 1 and line 2 independently if
   either still doesn't fit after wrapping. This covers a single
   unbreakable token wider than the screen (e.g. a `RecoveryCode` like
   `RECOVERY_TASK_CREATE_FAILED` with no spaces at all) — found live on
   real hardware during this cleanup (the SAFE_MODE screen's reason code
   overflowed by 51px before this was added; see git history for the
   `ellipsize_if_needed` fix).

Never a third font size, never a silent clip past the display edge.

### Left-alignment: the one sanctioned exception

`draw_recovery_menu()`'s 5-item option list (`1 Thử lại mạng` … `5 Khởi
động lại`) is left-aligned at a fixed `x=24`, not centered — a numbered
menu list reads better left-aligned, and the title above it (`MENU KHÔI
PHỤC`) is still centered via `centered_x()`. This is the only hardcoded
screen that intentionally breaks center-first alignment; any new
menu/list-style screen should follow the same pattern rather than
inventing a new one.

## Server-pushed UI bundles

A backend-authored bundle (`kiosk_v2_ui_bundles`, synced at bootstrap —
see `docs/UI_SCHEMA.md`'s corrected status note) can override any
`state_wait_employee`/`state_wait_operation`/`state_session_active`/
`state_device_disabled`/`state_maintenance` screen_id via
`draw_from_bundle()`. Bundle `TEXT` components declare their own
`font_size` (backward-compatible wire contract, any 1-N still decodes) and
may set `"align":"center"`, which the renderer honors by computing `x`
itself and auto-shrinking `font_size` on overflow (same `centered_x()`
path the hardcoded screens use). `tools/mock_backend/mock_backend.py`'s
`UI_BUNDLE_CONTENT[3]` (the default bundle version) mirrors the current
2-size/centered hardcoded design exactly, via its own `_c()` helper —
that's the reference to copy from when authoring a new bundle screen.

`QUANTITY_INPUT` and its defect/rework variants are **not** bundle-
overridable: `draw_value_giant()`'s emphasis has no bundle-schema
equivalent, so those screen_ids are absent from every bundle manifest and
the device transparently falls back to its own hardcoded
`draw_quantity_input_screen()`/etc.

## Adding a new screen

1. Does it show known-short static text only? Use `draw_title_line()` or
   `draw_title_2line()`.
2. Does it show variable-length data (a name, a server message, a scanned
   code)? Use `draw_fit_text()` — do not hand-roll a new wrap/shrink policy.
3. Does it need a footer hint? Call `draw_footer()` — don't draw a divider
   line or hint text manually.
4. Call `draw_status_bar(wifi)` unless there's a specific reason not to
   (portal/setup screens that don't yet have a WiFi state to show skip it).
5. Never call `setTextSize()`/pass a raw font-size int from screen-level
   code — go through the helpers, which alone reference `kFontSmall`/
   `kFontLarge`/`kFontValueScale`.
6. Screenshot it for real (`scripts/capture-screen.sh` over HTTP or
   `scripts/capture-screen-serial.sh` when the debug HTTP server isn't
   reachable — e.g. `SAFE_MODE` disables it) and eyeball it on the actual
   240×320 panel before calling it done. A host unit test can verify
   `measure_vn_text()`'s numbers; it cannot tell you a layout looks right.

## What's deliberately NOT part of this system

- `emit_line()` (the legacy row-grid path, `4 + row*22` positioning) is
  still used by a handful of ESP32-built-in emergency/technical screens
  (`draw_boot_screen()`, `draw_keypad_calibration_prompt()`) that predate
  the fixed-zone layout. These are diagnostic screens for a technician
  holding the device, not part of the "normal operator UI" this guide's
  rules apply to — left alone deliberately, not an oversight.
- The 3rd native glyph table (`kVnFontSmall`, 12px) exists for bundle
  back-compat (`font_size=1`) and `emit_line()`'s last-resort shrink path;
  no hardcoded screen uses it as a chosen typographic role.
