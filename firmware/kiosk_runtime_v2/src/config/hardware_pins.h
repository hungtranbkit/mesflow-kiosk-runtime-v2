#pragma once

// Pin mapping re-declared clean-room from reading (not copying)
// mesflow/esp-kiosk/esp/mesflow_app.cpp — see docs/HARDWARE.md for the full
// citation and caveats. If a board revision differs, only this file needs
// to change.

// --- Display: ILI9341 240x320, SPI ---
#define PIN_TFT_MISO 13
#define PIN_TFT_MOSI 11
#define PIN_TFT_SCLK 12
#define PIN_TFT_CS   10
#define PIN_TFT_DC   46
#define PIN_TFT_RST  -1  // not wired
#define PIN_TFT_BL   45  // backlight, active HIGH

// --- Scanner: GM65, UART, one-way (RX only) ---
#define PIN_SCANNER_RX 44
#define PIN_SCANNER_TX -1  // GM65 wired one-way; TX not used
#define SCANNER_BAUD 9600

// --- Touch: FT6336G, I2C --- (not driven in Phase 0, see docs/HARDWARE.md)
#define PIN_TOUCH_SDA 16
#define PIN_TOUCH_SCL 15
#define PIN_TOUCH_RST 18
#define PIN_TOUCH_INT 17
#define TOUCH_I2C_ADDR 0x38

// --- Keypad: PCF8574T over I2C, shares bus with touch ---
#define PIN_KEYPAD_SDA PIN_TOUCH_SDA
#define PIN_KEYPAD_SCL PIN_TOUCH_SCL
// Legacy scans 0x20..0x27 to find the expander; v2 does the same scan
// rather than hardcoding one address, since wiring can vary by unit.
#define KEYPAD_I2C_ADDR_MIN 0x20
#define KEYPAD_I2C_ADDR_MAX 0x27

// Speaker/buzzer: no pin found in legacy source — treated as ABSENT on this
// hardware revision (docs/HARDWARE.md). No PIN_SPEAKER_* defined on purpose;
// a health/UX module that assumes a speaker exists should fail to compile
// against this header, not silently no-op.
