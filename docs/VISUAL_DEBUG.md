# Remote Visual Debug

**Status: implemented and verified on real hardware.** This closes the
"AI/tool can't see the actual screen" gap — a display/UI/keypad-visual/
Wi-Fi-setup-UI change is no longer considered verified until a real
screenshot has been captured and inspected (see invariants 7-9 in
`docs/ARCHITECTURE.md`).

## Architecture

```text
UI Renderer (renderer.cpp)
    v
Display : public Adafruit_ILI9341   <- inherits, not wraps, so it can
    |                                  override every Adafruit_GFX virtual
    |                                  pixel primitive (drawPixel, writePixel,
    |                                  fillRect, fillScreen, ...)
    +--> physical ILI9341 TFT (via the normal Adafruit_ILI9341 base methods)
    +--> shadow framebuffer, RGB565, PSRAM (heap_caps_malloc, MALLOC_CAP_SPIRAM)
              |
              v
         DebugServer (src/debug/debug_server.*)
              |
    GET  /debug/screenshot     <- serves the SAME buffer, not a separate mock
    GET  /debug/ui-state
    GET  /debug/device-state
    POST /debug/input          <- publishes to the SAME EventBus as hardware
```

Framebuffer: 240 (w) x 320 (h) x 2 bytes (RGB565) = 153,600 bytes (~150KB)
in PSRAM. Confirmed via `docs/HARDWARE.md`: the panel actually runs
**portrait 240x320** (rotation 0), matching legacy's forced orientation —
not the illustrative 320x240 landscape example in `docs/UI_SCHEMA.md`
(that document is Phase 4 design, not a statement about Phase 0's current
orientation).

Every Adafruit_GFX virtual pixel primitive is overridden
(`drawPixel`/`writePixel`/`drawFastHLine`/`drawFastVLine`/`writeFastHLine`/
`writeFastVLine`/`writeFillRect`/`fillRect`/`fillScreen`) — not just
`drawPixel` — because Adafruit_ILI9341 routes different calls (`fillScreen`,
`print`, block fills) through different fast paths. Missing any one of them
would leave the shadow framebuffer silently incomplete, exactly the kind of
gap this feature exists to prevent. This mirrors legacy's own
`MesflowDisplay` class (read, not copied) for the same structural reason —
it's dictated by Adafruit_GFX's API surface, not a design preference.

**Constructor-order note** (a real bug caught while building this): a base
class subobject is always constructed before any derived-class data
members, so `Display` cannot own its `SPIClass` as a member once it
inherits `Adafruit_ILI9341` — the base constructor would capture a pointer
to a not-yet-constructed object. The `SPIClass` instance is now declared in
`kiosk_runtime_v2.ino`, in the same translation unit, immediately before the
`Display` instance, so the standard's same-TU declaration-order guarantee
applies. See `display.h`'s doc comment.

## Frame consistency — why no mutex

`DebugServer::poll()` (which calls `WebServer::handleClient()`) and every
`draw_*` call both run on the single Arduino `loopTask`. They can never
execute concurrently, so a screenshot handler can never race a draw() call
mid-frame — no lock is needed in Phase 0's architecture. This stops being
true the moment either side moves to its own FreeRTOS task; whoever does
that next must add real synchronization around the framebuffer then. This
is also why `frame_id` (bumped once per logical screen, not per pixel) is
sufficient for detecting whether the screen changed between two requests —
see `tools/capture_screen.py`'s request-state/capture/request-state-again/
retry pattern.

## Endpoints (DEV/LAB profile only — `MESFLOW_DEBUG_API`)

All gated at compile time by `MESFLOW_DEBUG_API` (on by default in this
Phase-0-is-dev-only build; see `runtime_config.h`). A production build must
explicitly turn this off — Phase 0 does not implement that profile split
yet, since the whole project is currently DO-NOT-DEPLOY-TO-PRODUCTION
(README). Runs on port 8081 (`MESFLOW_DEBUG_API_PORT`), deliberately
separate from the Wi-Fi recovery portal's port 80, so it keeps working even
while that portal is active.

### `GET /debug/screenshot`

Custom binary format (not PNG — see "Why not PNG" below):

```text
offset  0  magic "MFSC" (4 bytes)
offset  4  format version (uint8) = 1
offset  5  pixel_format (uint8) = 0 (RGB565)
offset  6  rotation (uint8, Adafruit_GFX rotation value 0-3)
offset  7  screen_id_len (uint8)
offset  8  width (uint16 LE)
offset 10  height (uint16 LE)
offset 12  frame_id (uint32 LE)
offset 16  screen_id bytes (ASCII, screen_id_len bytes)
offset 16+screen_id_len   raw RGB565 pixel data, row-major, width*height*2 bytes
```

Rate-limited to ~2/sec (`MESFLOW_DEBUG_SCREENSHOT_MIN_INTERVAL_MS`); returns
429 with `DEBUG_RATE_LIMITED` if called faster. Returns 503
(`DEBUG_SCREENSHOT_FAIL`) if the shadow framebuffer failed to allocate at
boot.

### `GET /debug/ui-state`

```json
{
  "screen_id": "waiting",
  "frame_id": 20,
  "display": { "width": 240, "height": 320, "rotation": 0 },
  "runtime": { "firmware_version": "0.1.0", "build_id": "..." },
  "lines": [
    { "row": 0, "text": "...", "color565": 65535, "measured_w": 168, "overflow": false }
  ]
}
```

Deliberately **not** the full rect/font component schema from
`docs/UI_SCHEMA.md` — Phase 0's renderer has no rect/font model at all (it's
a fixed set of 18px text rows). This reports exactly what actually gets
drawn: each row's text, color, measured width (via `Display::getTextBounds`,
real font metrics, not a guessed chars-per-row), and whether it overflows
the display width. Honest about Phase 0's actual capability rather than
fabricating a schema the renderer doesn't have.

### `GET /debug/device-state`

Device/network/memory/hardware/input snapshot — device_id, boot_id,
firmware/build id, uptime, Wi-Fi (connected/SSID/RSSI/IP/last-scan-reachable
— **never the password**), heap/PSRAM/framebuffer memory, hardware selftest
(display/scanner/keypad), last key and last scan value.

`backend_reachable` reflects only the outcome of the **last** scan's
network attempt (`null` if nothing has been scanned yet this boot) — not a
continuous liveness probe. Honest about what Phase 0 actually knows.

### `POST /debug/input`

```json
{"type": "SCAN", "value": "WF|EMP|001"}
{"type": "KEY_DOWN", "key": "*"}
{"type": "KEY_UP", "key": "*"}
```

Publishes to the **same EventBus** hardware drivers publish to — this tests
the application/runtime/UI path, exactly like a real scan or keypress would
be handled. It does **not** prove scanner UART hardware, PCF8574 electrical
scan, physical key debounce, or touch controller work — physical hardware
smoke tests still matter and are separate (`docs/TEST_PLAN.md`).
`{"type":"TOUCH",...}` is accepted syntactically but rejected with 501
(`DEBUG_INPUT_REJECTED`) — Phase 0 has no touch driver at all, so silently
accepting it would misrepresent what was tested.

## Host capture tool

```bash
python3 tools/capture_screen.py <device-ip> [--port 8081] [--out artifacts/debug]
# or:
scripts/capture-screen.sh <device-ip>
```

Fetches ui-state, screenshot, ui-state again (retries if the screen changed
mid-capture — §28's correlation requirement), then device-state. Decodes
the raw RGB565 into a real PNG (via Pillow) rather than staying stuck on
getting the ESP to encode PNG itself. Writes:

```text
artifacts/debug/<timestamp>/
  screenshot.png
  ui-state.json
  device-state.json
  manifest.json
  logs.txt        <- honest placeholder; Phase 0 has no persisted device-side
                     log store yet, only live serial via scripts/monitor.sh
artifacts/debug/latest -> <timestamp>   (symlink, always the most recent)
```

**A tool/AI must actually open `screenshot.png` and inspect it** — an
endpoint returning 200 is not verification. This is exactly how a real
overflow bug was found and fixed during this feature's own development (see
"Bug found and fixed" below).

## Why not PNG on-device

Per the task's own guidance: don't get stuck proving the ESP32 can encode a
pretty PNG. Raw RGB565 + a host-side Python/Pillow converter was faster to
ship and is exactly as useful for the actual goal (a human/AI seeing the
real screen). Revisit only if raw-binary bandwidth or host-tooling
friction ever becomes the actual bottleneck.

## Bug found and fixed during this feature's own development

The very first non-trivial screenshot taken (a scan-result screen with a
long error-code line) showed real corruption: a stray character bled onto
the row below, colliding with the Wi-Fi indicator. Root cause: Adafruit_GFX's
default text auto-wrap treats "off the right edge" as "start a new line
at the same cursor_y + line height" — which, on this renderer's fixed
18px-per-row layout (no wrapping model at all), meant an over-length string
corrupted whatever the *next* row was drawing. Fixed by disabling wrap
(`setTextWrap(false)`) so Adafruit_GFX clips per-character at the exact
screen edge instead (confirmed via `Adafruit_GFX::drawChar`'s own bounds
check — verified in the library source before relying on it), and by adding
real overflow measurement/logging (`UI_TEXT_OVERFLOW`, `lines[].overflow`/
`measured_w` in `/debug/ui-state`) so an overflow is visible, never silent.
This is the concrete case for why "serial success does not imply visual
success" (invariant 8) — nothing in the serial logs would ever have
revealed this.

## Wi-Fi recovery portal coexistence

Verified live: `/debug/input` KEY_DOWN '*' held past the 10s threshold
correctly triggers `WifiSetupPortal`, and `/debug/screenshot` +
`/debug/ui-state` kept working through the AP+STA mode transition — after a
transient ~5-10s connectivity blip during the actual Wi-Fi mode switch
(`WiFi.mode(WIFI_AP_STA)`/back to `WIFI_STA`), which is a real ESP32
radio-reconfiguration cost, not a debug-server bug. Both the countdown
screen and the portal-active screen (SSID + IP, **no password shown**) were
captured and visually confirmed correct. Cancelling the portal
(`POST /cancel` on port 80) and the device fully self-recovers back to the
normal waiting screen with Wi-Fi reconnected.

## Known limitations

- **No Vietnamese glyph support at all in Phase 0.** The renderer uses
  Adafruit_GFX's stock built-in 5x7 bitmap font, which has no diacritic
  glyphs — every on-screen string in this codebase is written without
  diacritics (`"San sang quet ma"`, not `"Sẵn sàng quét mã"`) specifically
  because of this. `docs/UI_SCHEMA.md`'s Vietnamese glyph coverage/
  validation requirements apply to the future Phase 4 bundle-compiled font
  assets, not to anything Phase 0 can render today. Do not report
  "Vietnamese font visual check: PASS" — it doesn't apply yet.
- **No host UI simulator** (`tools/ui-simulator/`) — out of scope per the
  task's own priority order (real screenshot first).
- **No visual regression / golden-image framework** (`test/visual/`) — same
  reason; not built.
- **No debug overlay** (component bounds/IDs toggle) — not built.
- **Serial fallback capture** (`scripts/capture-screen-serial.sh`) — not
  built; HTTP is the only capture path today. If HTTP is unreachable (e.g.
  device on a network this host can't join), there is currently no
  screenshot fallback, only live `scripts/monitor.sh` logs.
- **Production profile split** — `MESFLOW_DEBUG_API` is a single compile
  flag, currently always on. A real production build needs this gated by
  an actual build profile, not just "remember to flip the flag."
- **Frame/screen correlation is best-effort**, not cryptographically atomic
  across the 3 separate HTTP requests `capture_screen.py` makes — the
  retry-on-frame_id-change loop is the mitigation, not a hard guarantee.

## Structured error codes

```text
DEBUG_SCREENSHOT_FAIL       framebuffer not available (PSRAM alloc failed at boot)
DEBUG_RATE_LIMITED          screenshot requested faster than the min interval
DEBUG_INPUT_REJECTED        bad/unsupported /debug/input request
DEBUG_API_STARTED           (INFO, not an error) debug server came up
UI_TEXT_OVERFLOW            a drawn line's measured width exceeds the display width
```

`DEBUG_FRAME_LOCK_TIMEOUT`/`DEBUG_IMAGE_ENCODE_FAIL`/`DEBUG_UI_STATE_FAIL`
from the task's suggested list are not reachable in Phase 0's architecture
(no frame lock exists because none is needed; no on-device image encoding
happens at all) — not implemented as dead code.

## REMOTE_VISUAL_DEBUG_PASS gate

```text
[x] framebuffer/shadow framebuffer available       -- PSRAM, 153,600 bytes
[x] physical TFT renders correctly                  -- unchanged rendering path
[x] screenshot works                                -- verified via capture_screen.py
[x] Claude/tool can open resulting image             -- opened and inspected, found a real bug
[x] screenshot == physical orientation              -- 240x320 portrait, matches docs/HARDWARE.md
[x] ui-state works                                  -- verified, includes overflow diagnostics
[x] device-state works                              -- verified, no secrets present
[x] debug capture script works                      -- scripts/capture-screen.sh / tools/capture_screen.py
[x] input injection works in DEV                    -- SCAN and KEY_DOWN/KEY_UP verified live
[x] emergency screens can be captured                -- Wi-Fi recovery hold/portal captured live
[x] no secret leakage                                -- no Wi-Fi password in any endpoint/screenshot
[x] resource budget still PASS                       -- ~156KB PSRAM used, <2% of ~8MB total
```
