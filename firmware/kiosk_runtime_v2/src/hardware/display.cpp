#include "display.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

namespace kiosk::hardware {

bool Display::init() {
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);

  // REAL BUG, found and fixed live (2026-08-23): on ESP32-S3, custom TFT
  // pins must be explicitly bound via SPIClass::begin(sck, miso, mosi, cs)
  // BEFORE Adafruit_ILI9341::begin() -- otherwise Adafruit_SPITFT's
  // initSPI() calls SPI.begin() with no pin arguments internally, which
  // silently uses the default/unrouted bus rather than our wired GPIOs.
  // The physical TFT was completely black (backlight+SPI both "succeeding"
  // with zero software-visible error, shadow framebuffer still correct)
  // across two different boards and two different LCD panels until this
  // was added -- confirmed against the legacy esp-kiosk firmware, which
  // always made this same call explicitly before its own tft.begin().
  if (spi_ != nullptr) {
    spi_->begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);
  }
  begin();
  setRotation(0);  // TODO: confirm orientation empirically on real hardware
  invertDisplay(true);

  // Shadow framebuffer for the visual debug subsystem (docs/VISUAL_DEBUG.md).
  // Sized dynamically from width()/height() (post-rotation) rather than a
  // hardcoded 240x320/320x240, so this stays correct if rotation changes.
  size_t bytes = static_cast<size_t>(width()) * static_cast<size_t>(height()) * sizeof(uint16_t);
  framebuffer_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
  if (framebuffer_ == nullptr) {
    // No silent fallback (§58): if we can't get a shadow framebuffer, the
    // physical TFT still works (init() below already succeeded), but the
    // debug subsystem must know it has nothing to serve. hardware_selftest
    // reports this via framebuffer_ok().
    return false;
  }
  memset(framebuffer_, 0, bytes);

  fillScreen(ILI9341_BLACK);  // goes through the overridden fillScreen -> mirrors into framebuffer_ too

  return true;
}

void Display::clear(uint16_t color) {
  fillScreen(color);
}

void Display::draw_line(int line_index, const char* text, uint16_t color) {
  const int line_height = 18;
  setCursor(4, 4 + line_index * line_height);
  setTextColor(color, ILI9341_BLACK);
  setTextSize(1);
  // §7/UI_TEXT_OVERFLOW: this fixed layout has no line-wrapping model at
  // all (each row is exactly one line). Adafruit_GFX's default text WRAP
  // was letting an over-length string spill onto the next row and corrupt
  // whatever was drawn there (a real bug found via an actual screenshot,
  // not by inspection of this code) -- clip at the screen edge instead.
  // Renderer::emit_line is responsible for logging the overflow
  // diagnostic; this only prevents the corruption.
  setTextWrap(false);
  print(text);
}

void Display::mirror_pixel(int16_t x, int16_t y, uint16_t color) {
  if (framebuffer_ == nullptr) return;
  if (x < 0 || y < 0 || x >= width() || y >= height()) return;
  framebuffer_[static_cast<size_t>(y) * width() + x] = color;
}

void Display::mirror_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (framebuffer_ == nullptr) return;
  int16_t x0 = std::max<int16_t>(x, 0);
  int16_t y0 = std::max<int16_t>(y, 0);
  int16_t x1 = std::min<int16_t>(static_cast<int16_t>(x + w), width());
  int16_t y1 = std::min<int16_t>(static_cast<int16_t>(y + h), height());
  for (int16_t yy = y0; yy < y1; ++yy) {
    uint16_t* row = framebuffer_ + static_cast<size_t>(yy) * width();
    for (int16_t xx = x0; xx < x1; ++xx) row[xx] = color;
  }
}

void Display::drawPixel(int16_t x, int16_t y, uint16_t color) {
  Adafruit_ILI9341::drawPixel(x, y, color);
  mirror_pixel(x, y, color);
}

void Display::writePixel(int16_t x, int16_t y, uint16_t color) {
  Adafruit_ILI9341::writePixel(x, y, color);
  mirror_pixel(x, y, color);
}

void Display::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  Adafruit_ILI9341::drawFastHLine(x, y, w, color);
  mirror_rect(x, y, w, 1, color);
}

void Display::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  Adafruit_ILI9341::drawFastVLine(x, y, h, color);
  mirror_rect(x, y, 1, h, color);
}

void Display::writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  Adafruit_ILI9341::writeFastHLine(x, y, w, color);
  mirror_rect(x, y, w, 1, color);
}

void Display::writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  Adafruit_ILI9341::writeFastVLine(x, y, h, color);
  mirror_rect(x, y, 1, h, color);
}

void Display::writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  Adafruit_ILI9341::writeFillRect(x, y, w, h, color);
  mirror_rect(x, y, w, h, color);
}

void Display::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  Adafruit_ILI9341::fillRect(x, y, w, h, color);
  mirror_rect(x, y, w, h, color);
}

void Display::fillScreen(uint16_t color) {
  Adafruit_ILI9341::fillScreen(color);
  mirror_rect(0, 0, width(), height(), color);
}

}  // namespace kiosk::hardware
