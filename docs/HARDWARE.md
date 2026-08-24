# Hardware Reference

Source: read-only inspection of `mesflow/esp-kiosk/esp/mesflow_app.cpp`
(legacy firmware, FW_VERSION 5.5.7 at time of reading) and its
`docs/hardware/JC4827W543_TEST_PROFILE.md`. This file records facts only;
no legacy code was copied. Pin definitions are re-declared clean-room in
`firmware/kiosk_runtime_v2/src/config/hardware_pins.h`.

Board identity itself (exact vendor board name) was not re-confirmed against
new documentation — treat "JC4827W543-family ESP32-S3 kiosk board" as
inherited context, not a verified fact for v2. If it's wrong, only
`hardware_pins.h` needs correcting.

## Chip / build target

| Item | Value | Source |
|---|---|---|
| Chip | ESP32-S3 | Arduino FQBN `esp32:esp32:esp32s3` |
| Flash | 16 MB | `FlashSize=16M` |
| PSRAM | octal (opi) | `PSRAM=opi` — exact MB count TODO, confirm via v2 boot diagnostics on real hardware |
| Partition scheme | `default_8MB` (Arduino core's scheme name, not literal PSRAM size) | legacy `build.options.json` |
| PSRAM confirmed | 8MB, `AP_3v3` | `esptool` chip probe on real hardware during Phase 0 flash |

## Console (Serial) routing — a deliberate v2 deviation from legacy's FQBN

The esp32-arduino-core `esp32s3` board profile defaults `CDCOnBoot` to
**Disabled** (`cdc_on_boot=0`), which routes Arduino `Serial` to physical
UART0 (GPIO43 TX / GPIO44 RX) rather than the chip's native
USB-Serial/JTAG peripheral (what shows up as `/dev/ttyACM0` when only one
USB cable is connected). The legacy firmware's captured FQBN doesn't set
`CDCOnBoot` either, so it inherits the same default — legacy's board likely
has a *second*, separate UART-to-USB bridge wired to those pins for console
access, which this Phase 0 bring-up bench setup does not have connected.

v2's `scripts/build.sh`/`flash.sh` explicitly set `CDCOnBoot=cdc` so
`Serial` output is observable over the single native-USB cable
(`/dev/ttyACM0`) without needing a second adapter. This is safe for v2
because nothing in this codebase uses UART0/GPIO43-44 for anything else.
Confirmed working: after enabling this, boot diagnostics print over
`/dev/ttyACM0` as expected (see README "Hardware test").

## Display — ILI9341 240x320, SPI

| Signal | Pin |
|---|---|
| MISO | GPIO13 |
| MOSI | GPIO11 |
| SCLK | GPIO12 |
| CS | GPIO10 |
| DC | GPIO46 |
| RST | -1 (not wired / tied, not driven) |
| Backlight | GPIO45 (active HIGH) |

Legacy runs it in portrait, rotation forced, with an inversion setting ON.
v2 re-derives orientation/inversion empirically at bring-up rather than
trusting this blindly — treat as a starting point, not gospel.

## Scanner — GM65, UART, one-way

| Signal | Pin |
|---|---|
| RX (device receives scanner data) | GPIO44 |
| TX | -1 (not used — GM65 wired one-way into the ESP32) |
| Baud | 9600 8N1 |

Legacy pins RX with `INPUT_PULLUP` before `Serial.begin`. Framing in legacy
is line-oriented ASCII. v2's `scanner_gm65` driver only does UART framing,
timeout, max-length, and physical duplicate-scan suppression — it does not
parse business meaning from the payload (§26 of the task spec).

## Touch — FT6336G, I2C (capacitive)

| Signal | Pin |
|---|---|
| SDA | GPIO16 |
| SCL | GPIO15 |
| RST | GPIO18 |
| INT | GPIO17 |
| I2C address | 0x38 |

**Not yet driven in v2** — Phase 0 milestone only requires display, scanner,
keypad, event bus, wifi, and one mock API POST. Touch driver is a documented
gap (see README "Known limitations"), not an oversight.

## Keypad — PCF8574T over I2C, 3x4 matrix

Shares the same I2C bus as touch (SDA=GPIO16, SCL=GPIO15). Legacy scans
address range 0x20–0x27 to find the expander.

### Electrical scheme (read from legacy, reimplemented clean-room)

This is **not** a diode matrix scanned row-by-row in the usual sense. Each
of the 12 keys, when pressed, directly shorts two of the PCF8574's 8 I/O
lines together. Detecting a press means actively driving one line low at a
time (PCF8574 quasi-bidirectional output: write `~(1<<n)`) and reading back
which other line also reads low — that `(pinA, pinB)` pair identifies the
key. A **passive** single-byte read (v2's very first Phase 0 attempt)
never detects anything, because nothing is ever driven low without this
active scan — that was a real bug, not a simplification, and has been
fixed (`keypad_pcf8574.cpp`'s `scan_pair()`).

Because the 7 keypad wires can land on P0..P7 in any order per unit,
which electrical pair maps to which labeled key requires a one-time
calibration (`run_calibration()`, triggered via the serial command
`keypad-calibrate` in Phase 0 — see `docs/WIFI_RECOVERY.md` for why this
matters beyond just numeric input). A valid calibration must form a real
4-row x 3-column matrix shape (4 pins each appearing in exactly 3 pairs, 3
pins each appearing in exactly 4 pairs, 1 pin unused) — legacy validates
the same structural property, and v2 does too (`validate_matrix_shape()`),
because it's a fact about the physical wiring, not an arbitrary check.

Key layout (12 keys, standard phone-style, read off the physical keypad):
`1 2 3 4 5 6 7 8 9 * 0 #`.

## Speaker / buzzer

No speaker or buzzer pin was found anywhere in the legacy source. Treated as
**not present** on this hardware revision. `docs/ARCHITECTURE.md`'s health
module has no audio feedback dependency because of this.

## What v2 does NOT inherit from legacy

Updated: the Wi-Fi captive portal and keypad calibration concepts **are**
now reimplemented clean-room in v2 (see `docs/WIFI_RECOVERY.md` and the
"Electrical scheme" section above) — deliberately, because the physical
`*`-hold recovery trigger must exist independent of the backend. What is
still NOT inherited:

- The full on-device web admin server beyond the recovery portal (`mDNS`,
  the always-on diagnostics/log web UI, remote command console over HTTP)
- OTA via `Update.h` (v2's OTA design, Phase 6, targets A/B partitions with
  boot validation — different partition table entirely)
- Any business/session/workflow state machine
- The exact legacy UX details not called out above (e.g. legacy's shared
  fixed AP password, legacy's save-then-restart-without-testing — v2
  deliberately does these differently, see `docs/WIFI_RECOVERY.md`)

These may come back later, reimplemented against the new architecture, but
are explicitly out of scope for Phase 0.
