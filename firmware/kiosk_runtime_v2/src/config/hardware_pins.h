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
// Compile-time DEFAULT only -- the GM65's own stored baud is a setting
// inside the module itself (its EEPROM, set via a factory-supplied setup
// barcode), not something this firmware controls, and it does NOT
// necessarily match across physical units. Found live 2026-08-30 on one
// unit: edge-capture + linear regression against a USB-Virtual-Serial-Port
// ground-truth readback of "WF|EMP|NV002" measured its module at 115200,
// not the GM65 datasheet's factory default of 9600 -- 9600 produced zero
// bytes on that unit's real hardware (silent, no scan events at all, no
// error either, since a one-way RX UART has no failure signal to report).
// A second unit in the fleet is confirmed still at the factory 9600.
//
// One firmware build must work for both without a recompile per unit, so
// this macro is only the fallback when a device has never been told
// otherwise: the actual value used at runtime is
// ConfigStore::scanner_baud() (persisted in NVS, per physical device,
// defaults to 0 = "use this macro"), settable live via the DEV serial
// command `scanner-baud:<baud>` (kiosk_runtime_v2.ino) -- no rebuild/
// reflash needed to provision a unit whose module runs at a different
// baud, and no reboot needed either (unlike wifi:/api-endpoint:, this
// just re-attaches the UART).
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
