// LCD SWAP DISPLAY-ONLY TEST -- standalone, isolated sketch.
//
// Purpose: determine whether the NEW physically-swapped LCD works, on
// whichever single ESP32-S3 board is currently connected. Deliberately NOT
// part of firmware/kiosk_runtime_v2 (no WiFi, no business/session state
// machine, no device identity/provisioning, no MESFlow backend, no E2E) --
// this sketch only drives the TFT and reports over Serial.
//
// Pin values are re-declared here (not #included from kiosk_runtime_v2/src/
// config/hardware_pins.h) so this test has zero build/compile coupling to
// the runtime tree -- see that file / docs/HARDWARE.md for the source of
// these numbers, which describe the DEV BENCH WIRING (SPI bus + backlight
// GPIO), not any business identity. A board swap only changes which panel
// is on the other end of these same wires.
//
// PIN_TFT_RST is -1 (not wired on this bench) -- there is no GPIO to pulse
// for a true hardware reset. Only a SOFTWARE reset (SWRESET over SPI,
// issued inside Adafruit_ILI9341::begin()) is possible; this is reported
// honestly over Serial rather than silently skipped or faked.

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <esp_heap_caps.h>

// --- Display: ILI9341 240x320, SPI (see docs/HARDWARE.md) ---
#define PIN_TFT_MISO 13
#define PIN_TFT_MOSI 11
#define PIN_TFT_SCLK 12
#define PIN_TFT_CS   10
#define PIN_TFT_DC   46
#define PIN_TFT_RST  -1  // not wired
#define PIN_TFT_BL   45  // backlight, active HIGH

SPIClass g_spi(HSPI);
Adafruit_ILI9341 g_tft(&g_spi, PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_RST);

// Shadow framebuffer: mirrors only the full-screen solid-color fills below
// (not individual text glyphs) -- enough to prove "we wrote known pixel
// data into a real PSRAM buffer and can read it back", which is ALL this
// step claims. Per instructions, framebuffer success is never treated as
// proof the physical panel actually lit up -- that judgment comes only
// from a human looking at the real LCD.
uint16_t* g_shadow_fb = nullptr;
size_t g_shadow_fb_words = 0;
bool g_shadow_fb_last_fill_verified = false;

void log_json(const char* msg) { Serial.println(msg); }

void mirror_fill(uint16_t color) {
  if (g_shadow_fb == nullptr) return;
  for (size_t i = 0; i < g_shadow_fb_words; ++i) g_shadow_fb[i] = color;
}

bool shadow_fb_readback_matches(uint16_t expected) {
  if (g_shadow_fb == nullptr || g_shadow_fb_words == 0) return false;
  // Sample corners + center rather than the full buffer -- cheap, and
  // sufficient to catch "buffer never actually written"/"wrong stride".
  size_t last = g_shadow_fb_words - 1;
  size_t mid = g_shadow_fb_words / 2;
  return g_shadow_fb[0] == expected && g_shadow_fb[last] == expected && g_shadow_fb[mid] == expected;
}

void centered_text(const char* text, uint8_t size, int16_t y, uint16_t color, uint16_t bg) {
  int16_t x1, y1;
  uint16_t w, h;
  g_tft.setTextSize(size);
  g_tft.setTextColor(color, bg);
  g_tft.setTextWrap(false);
  g_tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int16_t x = (g_tft.width() - static_cast<int16_t>(w)) / 2;
  if (x < 0) x = 0;
  g_tft.setCursor(x, y);
  g_tft.print(text);
}

void fill_phase(const char* name, uint16_t color, unsigned long hold_ms) {
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"level\":\"INFO\",\"code\":\"LCD_SWAP_TEST_PHASE\",\"phase\":\"%s\",\"hold_ms\":%lu}",
           name, hold_ms);
  log_json(buf);
  g_tft.fillScreen(color);
  mirror_fill(color);
  g_shadow_fb_last_fill_verified = shadow_fb_readback_matches(color);
  delay(hold_ms);
}

void setup() {
  Serial.begin(115200);
  unsigned long serial_wait_start = millis();
  while (!Serial && millis() - serial_wait_start < 3000) { delay(10); }
  delay(300);

  log_json("{\"level\":\"INFO\",\"code\":\"LCD_SWAP_TEST_START\"}");

  // On-device identity cross-check against the host-side esptool eFuse MAC
  // read (step 2) -- both should report the same MAC for the same board.
  uint64_t mac = ESP.getEfuseMac();
  char mac_buf[64];
  snprintf(mac_buf, sizeof(mac_buf), "{\"level\":\"INFO\",\"code\":\"BOARD_EFUSE_MAC\",\"mac\":\"%04X%08X\"}",
           static_cast<uint16_t>(mac >> 32), static_cast<uint32_t>(mac));
  log_json(mac_buf);

  // 1) Force backlight ON.
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);
  log_json("{\"level\":\"INFO\",\"code\":\"LCD_SWAP_TEST_STEP\",\"step\":\"backlight_on\"}");

  // 2) Hardware reset TFT (honest about the -1/not-wired limitation).
#if PIN_TFT_RST >= 0
  log_json("{\"level\":\"INFO\",\"code\":\"LCD_SWAP_TEST_STEP\",\"step\":\"hardware_reset_pulse\"}");
  pinMode(PIN_TFT_RST, OUTPUT);
  digitalWrite(PIN_TFT_RST, LOW);
  delay(20);
  digitalWrite(PIN_TFT_RST, HIGH);
  delay(150);
#else
  log_json("{\"level\":\"WARN\",\"code\":\"LCD_SWAP_TEST_STEP\",\"step\":\"hardware_reset_pulse\","
           "\"message\":\"SKIPPED -- PIN_TFT_RST is -1 (not wired on this bench); only a software "
           "reset (SWRESET, issued inside begin() below) is possible\"}");
#endif

  // 3) Init SPI/TFT.
  log_json("{\"level\":\"INFO\",\"code\":\"LCD_SWAP_TEST_STEP\",\"step\":\"spi_tft_begin\"}");
  g_spi.begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);
  g_tft.begin();
  g_tft.setRotation(0);
  g_tft.invertDisplay(true);  // matches kiosk_runtime_v2's Display::init() for this panel family

  // Shadow framebuffer (PSRAM). Allocation success/failure is reported but
  // never gates the physical test sequence below.
  g_shadow_fb_words = static_cast<size_t>(g_tft.width()) * static_cast<size_t>(g_tft.height());
  g_shadow_fb = static_cast<uint16_t*>(heap_caps_malloc(g_shadow_fb_words * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
  if (g_shadow_fb != nullptr) {
    memset(g_shadow_fb, 0, g_shadow_fb_words * sizeof(uint16_t));
    log_json("{\"level\":\"INFO\",\"code\":\"SHADOW_FRAMEBUFFER\",\"result\":\"ALLOCATED\"}");
  } else {
    log_json("{\"level\":\"WARN\",\"code\":\"SHADOW_FRAMEBUFFER\",\"result\":\"ALLOC_FAILED\"}");
  }

  // 4) BLACK 2s, WHITE 3s, RED 3s, GREEN 3s, BLUE 3s.
  fill_phase("BLACK", ILI9341_BLACK, 2000);
  fill_phase("WHITE", ILI9341_WHITE, 3000);
  fill_phase("RED", ILI9341_RED, 3000);
  fill_phase("GREEN", ILI9341_GREEN, 3000);
  fill_phase("BLUE", ILI9341_BLUE, 3000);

  // 5) Final screen: black background, large white text, left on screen.
  log_json("{\"level\":\"INFO\",\"code\":\"LCD_SWAP_TEST_PHASE\",\"phase\":\"FINAL_TEXT\"}");
  g_tft.fillScreen(ILI9341_BLACK);
  mirror_fill(ILI9341_BLACK);
  centered_text("LCD TEST", 3, 90, ILI9341_WHITE, ILI9341_BLACK);
  centered_text("MESFLOW", 3, 150, ILI9341_WHITE, ILI9341_BLACK);
  centered_text("1234567890", 2, 210, ILI9341_WHITE, ILI9341_BLACK);

  // Shadow framebuffer report: only claims what it can prove (a PSRAM
  // buffer was allocated and the LAST SOLID FILL -- BLACK, just before the
  // text was drawn -- reads back correctly). It does NOT claim to mirror
  // the text glyphs pixel-for-pixel, and it is NOT evidence the physical
  // panel is lit -- that is a human visual judgment call only.
  bool shadow_ok = (g_shadow_fb != nullptr) && g_shadow_fb_last_fill_verified;
  char fb_buf[160];
  snprintf(fb_buf, sizeof(fb_buf),
           "{\"level\":\"INFO\",\"code\":\"SHADOW_FRAMEBUFFER_RESULT\",\"result\":\"%s\","
           "\"allocated\":%s,\"last_fill_readback_ok\":%s,\"note\":\"solid-fill mirror only, not glyph-exact; "
           "not proof of physical LCD\"}",
           shadow_ok ? "PASS" : "FAIL",
           (g_shadow_fb != nullptr) ? "true" : "false",
           g_shadow_fb_last_fill_verified ? "true" : "false");
  log_json(fb_buf);

  log_json("{\"level\":\"INFO\",\"code\":\"LCD_SWAP_TEST_DONE\",\"message\":\"Final text screen left on -- "
           "no further redraw. Judge BACKLIGHT/WHITE/RED/GREEN/BLUE/TEXT by eye on the physical panel.\"}");
}

void loop() {
  // Intentionally idle -- the final text screen is left exactly as drawn.
  // No periodic redraw, no network, no business/session state, no E2E.
  delay(1000);
}
