#pragma once

// SPI.h must come before Adafruit_ILI9341.h (which pulls in Adafruit_BusIO):
// Adafruit_BusIO's Adafruit_SPIDevice.h refers to the core's `BitOrder`
// enum, which only exists once SPI.h has been included first.
#include <SPI.h>
#include <Adafruit_ILI9341.h>

#include "../config/hardware_pins.h"

namespace kiosk::hardware {

// Display now INHERITS Adafruit_ILI9341 (rather than wrapping it as a
// member) specifically so it can override Adafruit_GFX's virtual
// pixel-level primitives (docs/VISUAL_DEBUG.md invariant 7/8: a display/UI
// change isn't verified until the rendered output can be observed). Every
// override mirrors into a PSRAM-backed shadow framebuffer -- the SAME pixel
// source used to drive the physical TFT, not a separate "screenshot mock".
//
// IMPORTANT constructor-order note: the SPIClass instance must NOT be a
// data member of this class. A base class subobject (Adafruit_ILI9341) is
// always constructed before any derived-class data members, regardless of
// declaration order -- so a member `SPIClass spi_` would still be
// uninitialized at the moment the base constructor captures its address.
// The caller owns the SPIClass instance and must keep it alive at least as
// long as Display (see kiosk_runtime_v2.ino, which declares it in the same
// translation unit, right before the Display instance, so the standard's
// same-TU declaration-order guarantee applies). This is the same reason
// legacy's equivalent class takes an external SPIClass rather than owning
// one as a member.
class Display : public Adafruit_ILI9341 {
 public:
  // `spi_` is a plain pointer COPY of the caller's SPIClass* -- storing the
  // value (not its address) as a data member here is fine even though the
  // base class subobject above it is constructed first; the ordering
  // hazard documented above is only about taking a data member's ADDRESS
  // in the base constructor call, which this doesn't do.
  explicit Display(SPIClass* spi)
      : Adafruit_ILI9341(spi, PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_RST), spi_(spi) {}

  // Returns true if the ILI9341 responded to init AND the shadow
  // framebuffer was successfully allocated in PSRAM. Adafruit_ILI9341's
  // begin() doesn't itself report failure distinctly (no panel readback
  // verification here in Phase 0) — this reflects "we called begin(), drove
  // the backlight, and got a framebuffer", not a verified round-trip read
  // from the panel. Documented limitation, see hardware_selftest.
  bool init();

  void clear(uint16_t color);
  void draw_line(int line_index, const char* text, uint16_t color);

  // --- Visual debug subsystem (docs/VISUAL_DEBUG.md) ---

  // Raw RGB565 pixels, row-major, exactly `width() * height()` uint16_t
  // values. May be null if PSRAM allocation failed at init() — callers MUST
  // check framebuffer_ok() first.
  const uint16_t* framebuffer() const { return framebuffer_; }
  bool framebuffer_ok() const { return framebuffer_ != nullptr; }
  size_t framebuffer_bytes() const {
    return static_cast<size_t>(width()) * static_cast<size_t>(height()) * sizeof(uint16_t);
  }

  // Bumped once per logical screen redraw (called by Renderer, not by every
  // individual pixel op) so a debug capture can tell whether the screen
  // changed between two requests (§27/§28: correlate screenshot + ui-state).
  uint32_t frame_id() const { return frame_id_; }
  void bump_frame_id() { ++frame_id_; }

  // Adafruit_GFX's full virtual pixel-primitive surface. All of them must
  // be overridden, not just drawPixel: Adafruit_ILI9341/Adafruit_GFX route
  // different calls (fillScreen, print, fillRect...) through different fast
  // paths, and missing even one would leave the shadow framebuffer silently
  // incomplete -- exactly the kind of gap this feature exists to prevent.
  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void writePixel(int16_t x, int16_t y, uint16_t color) override;
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
  void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
  void fillScreen(uint16_t color) override;

 private:
  SPIClass* spi_ = nullptr;
  uint16_t* framebuffer_ = nullptr;
  uint32_t frame_id_ = 0;

  void mirror_pixel(int16_t x, int16_t y, uint16_t color);
  void mirror_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
};

}  // namespace kiosk::hardware
