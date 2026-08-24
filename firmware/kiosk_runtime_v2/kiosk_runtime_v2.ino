// MESFlow Kiosk Runtime v2 — entry point.
//
// This file only wires modules together (setup/loop). Business logic,
// protocol, hardware drivers, etc. all live under src/ — see
// docs/ARCHITECTURE.md for the module map and docs/ for the full design.
//
// DO NOT DEPLOY TO PRODUCTION — see README.md.

#include <Arduino.h>
#include <WiFi.h>

#include "src/config/hardware_pins.h"
#include "src/debug/debug_server.h"
#include "src/hardware/display.h"
#include "src/hardware/hardware_selftest.h"
#include "src/hardware/keypad_pcf8574.h"
#include "src/hardware/scanner_gm65.h"
#include "src/network/bootstrap_client.h"
#include "src/network/heartbeat_client.h"
#include "src/network/time_sync.h"
#include "src/network/wifi_manager.h"
#include "src/network/wifi_setup_portal.h"
#include "src/protocol/ids.h"
#include "src/runtime/boot_diagnostics.h"
#include "src/runtime/event_bus.h"
#include "src/runtime/kiosk_runtime.h"
#include "src/runtime/ui_sync_controller.h"
#include "src/runtime/wifi_recovery_controller.h"
#include "src/security/device_identity.h"
#include "src/storage/config_store.h"
#include "src/storage/ui_bundle_store.h"
#include "src/ui/renderer.h"

namespace {

kiosk::runtime::EventBus g_bus;
// Must be declared before g_display, in this same translation unit: Display
// now inherits Adafruit_ILI9341 (for shadow-framebuffer overrides, see
// display.h), and a base class is always constructed before any derived
// data members -- so the SPIClass instance cannot be owned by Display
// itself. Declaration order here is what guarantees it exists first.
SPIClass g_display_spi(HSPI);
kiosk::hardware::Display g_display(&g_display_spi);
kiosk::hardware::ScannerGm65 g_scanner(g_bus);
kiosk::hardware::KeypadPcf8574 g_keypad(g_bus);
kiosk::storage::ConfigStore g_config;
kiosk::security::DeviceIdentity g_identity;
kiosk::network::TimeSync g_time_sync;
kiosk::ui::Renderer g_renderer(g_display);
kiosk::network::WifiManager g_wifi(g_bus);
kiosk::network::WifiSetupPortal g_wifi_portal(g_bus, g_config, g_identity);
kiosk::storage::UiBundleStore g_ui_bundle_store;
kiosk::runtime::KioskRuntime g_runtime(g_bus, g_renderer, g_config, g_identity, g_time_sync, g_ui_bundle_store);
kiosk::runtime::UiSyncController g_ui_sync(g_ui_bundle_store);
kiosk::runtime::WifiRecoveryController g_wifi_recovery(g_bus, g_renderer, g_wifi_portal);
kiosk::network::BootstrapClient g_bootstrap;

kiosk::hardware::SelfTestResult g_selftest;
kiosk::runtime::BootDiagnostics g_diagnostics;

kiosk::network::HeartbeatClient g_heartbeat(g_identity, g_time_sync, g_runtime, g_keypad, g_selftest,
                                            g_diagnostics, g_bootstrap);

#if MESFLOW_DEBUG_API
// Constructed after g_selftest/g_diagnostics exist (it holds a reference to
// them) but only actually started in setup() once they've been populated.
kiosk::debug::DebugServer g_debug_server(g_bus, g_display, g_renderer, g_runtime, g_keypad,
                                          g_selftest, g_diagnostics, g_identity, g_time_sync,
                                          g_bootstrap, g_ui_bundle_store, g_ui_sync);
#endif

unsigned long g_last_diagnostics_print_ms = 0;
constexpr unsigned long kDiagnosticsPrintIntervalMs = 30000;

bool g_bootstrap_attempted = false;
bool g_time_sync_started = false;

void keypad_calibration_prompt(char key, uint8_t index, uint8_t total) {
  g_renderer.draw_keypad_calibration_prompt(key, index, total);
}

kiosk::ui::WifiIndicator current_wifi_indicator() {
  switch (g_wifi.state()) {
    case kiosk::network::WifiState::CONNECTED: return kiosk::ui::WifiIndicator::CONNECTED;
    case kiosk::network::WifiState::CONNECTING: return kiosk::ui::WifiIndicator::CONNECTING;
    case kiosk::network::WifiState::DISCONNECTED: return kiosk::ui::WifiIndicator::DISCONNECTED;
  }
  return kiosk::ui::WifiIndicator::UNKNOWN;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);  // let USB-serial enumerate before the first log line

  Serial.printf("{\"level\":\"INFO\",\"code\":\"PROFILE\",\"profile\":\"%s\"}\n",
                MESFLOW_PROFILE_NAME);

  g_config.init();
  g_identity.init();
  g_ui_bundle_store.init();  // loads last-known-good UI bundle from NVS, if any (§5/§16)

  String boot_id = kiosk::protocol::generate_random_hex_id(8).c_str();

  // SPI pin binding for the TFT (root cause of a real, since-fixed bug: see
  // display.cpp's Display::init()) now lives entirely inside Display::init()
  // itself, so display bring-up has exactly one owner.
  g_selftest.display_ok = g_display.init();
  g_selftest.scanner_ok = g_scanner.init();
  g_selftest.keypad_ok = g_keypad.init();  // false = DEGRADED, not fatal

  g_diagnostics = kiosk::runtime::collect_boot_diagnostics(boot_id);

  Serial.printf("{\"level\":\"INFO\",\"code\":\"BOOT\",\"boot_id\":\"%s\","
                "\"fw_version\":\"%s\",\"build_id\":\"%s\",\"chip\":\"%s\","
                "\"flash_mb\":%u,\"reset_reason\":\"%s\",\"hardware_id\":\"%s\","
                "\"device_id\":\"%s\",\"provisioning_state\":\"%s\"}\n",
                g_diagnostics.boot_id.c_str(), g_diagnostics.fw_version.c_str(),
                g_diagnostics.build_id.c_str(), g_diagnostics.chip_model.c_str(),
                g_diagnostics.flash_mb, g_diagnostics.reset_reason.c_str(),
                g_identity.hardware_id().c_str(), g_identity.device_id().c_str(),
                kiosk::security::provisioning_state_to_string(g_identity.state()));

  g_runtime.begin(boot_id);
  g_wifi_recovery.begin();
  g_renderer.draw_boot_screen(g_diagnostics, g_selftest.scanner_ok, g_selftest.keypad_ok);
  delay(2000);  // hold the boot/diagnostics screen briefly before going to waiting/identity state
  g_runtime.refresh_idle_screen();

  g_wifi.begin(g_config.wifi_ssid(), g_config.wifi_password());

#if MESFLOW_DEBUG_API
  // TEMPORARY diagnostic fix (real bug found live, 2026-08-23): on a
  // genuinely fresh/never-provisioned board (blank NVS, no Wi-Fi
  // credentials ever saved), WifiManager::begin() correctly short-circuits
  // without ever calling WiFi.mode()/WiFi.begin() -- but DebugServer::begin()
  // (WebServer::begin()) unconditionally assumes the WiFi/LWIP stack is
  // already initialized, and asserts (`xQueueSemaphoreTake queue.c:1709`)
  // almost immediately after setup() if it never was. This crash-loops
  // before loop() ever runs once, meaning it can't even be worked around
  // via a serial command. Confirmed: our provisioned kiosk board (which has
  // always had Wi-Fi credentials since Phase 0) never hits this path.
  // Calling WiFi.mode(WIFI_STA) unconditionally -- even with no SSID yet --
  // is enough to initialize the stack without actually attempting a
  // connection. Left here as a real fix candidate; revisit whether this
  // should be unconditional in WifiManager::begin() itself instead of only
  // guarded here.
  WiFi.mode(WIFI_STA);
  g_debug_server.begin();
#endif
}

namespace {

// DEV-ONLY provisioning/maintenance stub over the serial console (the same
// USB cable used for logs/flashing):
//
//   wifi:<ssid>,<password>    save Wi-Fi creds directly to NVS, then reboot
//   keypad-calibrate          run the guided 12-key calibration (blocking)
//   recovery-info             print this device's Wi-Fi recovery AP SSID/password
//                             (read-only; does not start the portal)
//   display-test              DEV-only physical display diagnostic: drives the
//                             REAL display hardware (not just the shadow
//                             framebuffer) through white/black/red/green/blue/
//                             checkerboard/border, ~2.5s each (blocking)
//   display-pins               prints the compiled BL/RST/CS/DC/SCLK/MOSI/MISO
//                             pin map and the ACTUAL current level read back
//                             off BL (CS/DC are SPI-library-managed, not held
//                             statically -- reported as n/a, not guessed)
//   display-reinit             backlight OFF -> reset (hardware pulse if wired,
//                             else honestly logs SKIPPED) -> begin()/rotation
//                             -> fill RED -> backlight ON. No reboot, no UI.
//   backlight-test              raw GPIO cycle only (LOW 2s / HIGH 5s / LOW 2s
//                             / HIGH) -- isolates the backlight pin/driver
//                             from all SPI/display code
//   lcd-physical-test          named screen sequence with real text (TEST LCD /
//                             WHITE SCREEN / RED / GREEN / BLUE / bordered
//                             MESFLOW+LCD OK+1234567890), ~3s each, then holds
//                             on "LCD TEST / IF YOU CAN READ THIS / DISPLAY IS
//                             WORKING" indefinitely. Real hardware calls only.
//   api-endpoint:<url>        validate + save api_endpoint to NVS, then reboot
//                             (KioskRuntime reads it once at boot, same as
//                             Wi-Fi creds -- so this reboots too, deliberately
//                             matching the wifi: command's behavior)
//   provision:<device_id>     assign device_id, move provisioning_state to
//                             ACTIVE, then reboot (§4/§5/§36 -- DEV-path
//                             provisioning skeleton; see docs/PROVISIONING.md)
//   suspend / revoke          DEV-only: force provisioning_state for testing
//                             §5's behavior without a real backend admin
//                             action (KIOSK-079/080)
//   debug-screenshot           Serial Visual Debug Fallback (MESFLOW_DEBUG_API
//                             builds only): writes the current framebuffer
//                             over Serial as a framed binary blob (magic
//                             "MFSB", header + CRC32) -- see
//                             DebugServer::write_screenshot_serial(). Use
//                             when the host can't reach /debug/screenshot
//                             over HTTP (different subnet). Host-side:
//                             tools/capture_screen_serial.py or
//                             scripts/capture-screen-serial.sh.
//   debug-ui-state              same idea as debug-screenshot but for
//                             GET /debug/ui-state's JSON, wrapped in a
//                             "##MFDBG-BEGIN ui-state##" / "##MFDBG-END##"
//                             marker pair.
//   debug-device-state          same idea for GET /debug/device-state's JSON.
//   debug-input:<json>          same SCAN/KEY_DOWN/KEY_UP dispatch as
//                             POST /debug/input, e.g.
//                             debug-input:{"type":"SCAN","value":"00152"} --
//                             drives real state transitions over serial when
//                             there's no HTTP path and no physical scanner/
//                             keypad access. Response wrapped the same way
//                             (##MFDBG-BEGIN debug-input##/##MFDBG-END##).
//
// The real, backend-independent recovery path for an operator on the floor
// is holding '*' for 10s (WifiRecoveryController) -- this serial interface
// is only for bring-up/dev convenience and initial keypad calibration,
// which nothing else can bootstrap (see docs/HARDWARE.md).
void poll_serial_provisioning() {
  static String line;
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (line.startsWith("wifi:")) {
        String rest = line.substring(5);
        int comma = rest.indexOf(',');
        if (comma > 0) {
          String ssid = rest.substring(0, comma);
          String pass = rest.substring(comma + 1);
          g_config.set_wifi_credentials(ssid, pass);
          Serial.printf("{\"level\":\"INFO\",\"code\":\"CONFIG_WIFI_SET\","
                        "\"module\":\"provisioning\",\"ssid\":\"%s\"}\n",
                        ssid.c_str());
          Serial.println("Rebooting to apply Wi-Fi credentials...");
          delay(200);
          ESP.restart();
        } else {
          Serial.println("Usage: wifi:<ssid>,<password>");
        }
      } else if (line == "keypad-calibrate") {
        Serial.println("Starting keypad calibration -- press and release each key shown on-screen.");
        bool ok = g_keypad.run_calibration(keypad_calibration_prompt);
        Serial.println(ok ? "Calibration OK." : "Calibration FAILED.");
        g_runtime.refresh_idle_screen();
      } else if (line.startsWith("api-endpoint:")) {
        String url = line.substring(13);
        url.trim();
        auto result = g_config.set_api_endpoint(url);
        if (result != kiosk::protocol::BackendUrlValidation::OK) {
          Serial.printf("Rejected: %s\n", kiosk::protocol::backend_url_validation_to_string(result));
        } else {
          Serial.printf("{\"level\":\"INFO\",\"code\":\"CONFIG_API_ENDPOINT_SET\","
                        "\"module\":\"provisioning\",\"url\":\"%s\"}\n",
                        url.c_str());
          Serial.println("Rebooting to apply api_endpoint...");
          delay(200);
          ESP.restart();
        }
      } else if (line.startsWith("provision:")) {
        String new_id = line.substring(10);
        new_id.trim();
        if (g_identity.provision(new_id)) {
          Serial.printf("Provisioned as %s. Rebooting...\n", new_id.c_str());
          delay(200);
          ESP.restart();
        } else {
          Serial.println("Usage: provision:<device_id>");
        }
      } else if (line == "suspend") {
        g_identity.set_state(kiosk::security::ProvisioningState::SUSPENDED);
        g_runtime.refresh_idle_screen();
      } else if (line == "revoke") {
        g_identity.set_state(kiosk::security::ProvisioningState::REVOKED);
        g_runtime.refresh_idle_screen();
      } else if (line == "recovery-info") {
        g_wifi_portal.compute_ap_credentials();
        Serial.printf("{\"level\":\"INFO\",\"code\":\"NET_WIFI_RECOVERY_INFO\","
                      "\"device_id\":\"%s\",\"hardware_id\":\"%s\",\"ap_ssid\":\"%s\","
                      "\"ap_password\":\"%s\"}\n",
                      g_identity.device_id().c_str(), g_identity.hardware_id().c_str(),
                      g_wifi_portal.ap_ssid().c_str(), g_wifi_portal.ap_password().c_str());
      } else if (line == "display-test") {
        // DEV-only physical display diagnostic: calls g_display's REAL
        // drawing entry points directly (the SAME overridden fillScreen/
        // fillRect that Display::init()/Renderer use -- see display.cpp,
        // every override calls Adafruit_ILI9341:: first, THEN mirrors into
        // the shadow framebuffer) -- this is not a shadow-framebuffer-only
        // test. Each phase is announced on serial AND held long enough to
        // observe physically, and /debug/screenshot can be captured during
        // any phase to compare "what the shadow framebuffer has" against
        // "what a person sees on the physical panel" (docs/VISUAL_DEBUG.md
        // invariant 8's whole point: serial/software success is not visual
        // proof).
        struct { const char* name; uint16_t color; } solids[] = {
            {"WHITE", ILI9341_WHITE}, {"BLACK", ILI9341_BLACK}, {"RED", ILI9341_RED},
            {"GREEN", ILI9341_GREEN}, {"BLUE", ILI9341_BLUE},
        };
        for (auto& phase : solids) {
          Serial.printf("{\"level\":\"INFO\",\"code\":\"DISPLAY_TEST_PHASE\",\"phase\":\"%s\"}\n", phase.name);
          g_display.fillScreen(phase.color);
          delay(2500);
        }
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_TEST_PHASE\",\"phase\":\"CHECKERBOARD\"}");
        {
          const int tile = 20;
          for (int y = 0; y < g_display.height(); y += tile) {
            for (int x = 0; x < g_display.width(); x += tile) {
              bool even = ((x / tile) + (y / tile)) % 2 == 0;
              g_display.fillRect(x, y, tile, tile, even ? ILI9341_WHITE : ILI9341_BLACK);
            }
          }
        }
        delay(2500);
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_TEST_PHASE\",\"phase\":\"BORDER\"}");
        g_display.fillScreen(ILI9341_BLACK);
        g_display.drawRect(0, 0, g_display.width(), g_display.height(), ILI9341_YELLOW);
        g_display.drawRect(4, 4, g_display.width() - 8, g_display.height() - 8, ILI9341_YELLOW);
        delay(2500);
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_TEST_DONE\"}");
        g_display.bump_frame_id();  // so a /debug/screenshot taken right after reflects the final phase
        g_runtime.refresh_idle_screen();
      } else if (line == "display-pins") {
        // Electrical diagnostic (§1/§3 of the display-fault investigation):
        // prints exactly what this build has compiled in, plus the ACTUAL
        // current level read back off each pin this code controls directly
        // -- never just "what we think we commanded". CS/DC are driven
        // internally by Adafruit_ILI9341/the SPI library on every transfer,
        // not held at a static level by our code, so their "current level"
        // is transient/meaningless between transfers -- reported as N/A
        // rather than a misleading snapshot.
        {
          String msg = "{\"level\":\"INFO\",\"code\":\"DISPLAY_PINS\",\"pins\":{";
          msg += "\"BL\":{\"gpio\":" + String(PIN_TFT_BL) + ",\"mode\":\"OUTPUT\",\"level\":" +
                 String(digitalRead(PIN_TFT_BL)) + "},";
          msg += "\"RST\":{\"gpio\":" + String(PIN_TFT_RST) + ",\"mode\":\"NOT_WIRED\",\"level\":\"n/a\"},";
          msg += "\"CS\":{\"gpio\":" + String(PIN_TFT_CS) +
                 ",\"mode\":\"SPI_LIBRARY_MANAGED\",\"level\":\"n/a (toggled per-transfer, "
                 "not held statically by this code)\"},";
          msg += "\"DC\":{\"gpio\":" + String(PIN_TFT_DC) +
                 ",\"mode\":\"SPI_LIBRARY_MANAGED\",\"level\":\"n/a (same as CS)\"},";
          msg += "\"SCLK\":" + String(PIN_TFT_SCLK) + ",\"MOSI\":" + String(PIN_TFT_MOSI) +
                 ",\"MISO\":" + String(PIN_TFT_MISO) + "}}";
          Serial.println(msg);
        }
      } else if (line == "display-reinit") {
        // DEV-only electrical re-init (§4): backlight OFF, (attempted)
        // hardware reset, SPI/display begin(), rotation/init, fill RED,
        // backlight ON. Deliberately does NOT re-run Display::init() (which
        // would re-allocate the PSRAM shadow framebuffer on top of the one
        // already allocated at boot) -- calls the same underlying
        // Adafruit_ILI9341 entry points directly instead. No reboot, no
        // business/UI code involved.
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"backlight_off\"}");
        digitalWrite(PIN_TFT_BL, LOW);
        delay(300);
#if PIN_TFT_RST >= 0
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"hardware_reset_pulse\"}");
        pinMode(PIN_TFT_RST, OUTPUT);
        digitalWrite(PIN_TFT_RST, LOW);
        delay(20);
        digitalWrite(PIN_TFT_RST, HIGH);
        delay(150);
#else
        // Honest limitation, not silently skipped: PIN_TFT_RST is -1 (not
        // wired) on this board -- there is no GPIO to pulse. Only a
        // SOFTWARE reset (SWRESET over SPI, inside begin() below) is
        // possible. If the panel controller itself is wedged in a state
        // only a real hardware reset can clear, this step cannot help.
        Serial.println("{\"level\":\"WARN\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"hardware_reset_pulse\","
                       "\"message\":\"SKIPPED -- PIN_TFT_RST is -1 (not wired), only software reset is possible\"}");
#endif
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"begin\"}");
        g_display.begin();
        g_display.setRotation(0);
        g_display.invertDisplay(true);
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"fill_red\"}");
        g_display.fillScreen(ILI9341_RED);
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_STEP\",\"step\":\"backlight_on\"}");
        digitalWrite(PIN_TFT_BL, HIGH);
        Serial.println("{\"level\":\"INFO\",\"code\":\"DISPLAY_REINIT_DONE\"}");
        g_display.bump_frame_id();
      } else if (line == "backlight-test") {
        // §5: isolates the backlight GPIO/driver path from everything else
        // -- no SPI/display calls at all in this command, just the raw pin.
        Serial.println("{\"level\":\"INFO\",\"code\":\"BACKLIGHT_TEST_STEP\",\"step\":\"LOW\",\"hold_s\":2}");
        digitalWrite(PIN_TFT_BL, LOW);
        delay(2000);
        Serial.println("{\"level\":\"INFO\",\"code\":\"BACKLIGHT_TEST_STEP\",\"step\":\"HIGH\",\"hold_s\":5}");
        digitalWrite(PIN_TFT_BL, HIGH);
        delay(5000);
        Serial.println("{\"level\":\"INFO\",\"code\":\"BACKLIGHT_TEST_STEP\",\"step\":\"LOW\",\"hold_s\":2}");
        digitalWrite(PIN_TFT_BL, LOW);
        delay(2000);
        Serial.println("{\"level\":\"INFO\",\"code\":\"BACKLIGHT_TEST_STEP\",\"step\":\"HIGH\",\"hold_s\":\"final\"}");
        digitalWrite(PIN_TFT_BL, HIGH);
        Serial.println("{\"level\":\"INFO\",\"code\":\"BACKLIGHT_TEST_DONE\"}");
      } else if (line == "lcd-physical-test") {
        // Requested physical LCD verification sequence: every draw call
        // below goes through Display's overridden Adafruit_GFX methods,
        // which call the REAL Adafruit_ILI9341:: implementation first and
        // mirror into the shadow framebuffer second (see display.cpp) --
        // the shadow framebuffer is a side effect here, never the only
        // output. Backlight is forced HIGH before the sequence and left on.
        digitalWrite(PIN_TFT_BL, HIGH);

        auto centered_text = [](const char* text, uint8_t size, int16_t y, uint16_t color, uint16_t bg) {
          int16_t x1, y1;
          uint16_t w, h;
          g_display.setTextSize(size);
          g_display.setTextColor(color, bg);
          g_display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
          int16_t x = (g_display.width() - static_cast<int16_t>(w)) / 2;
          g_display.setCursor(x, y);
          g_display.print(text);
        };

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN1_TEST_LCD\"}");
        g_display.fillScreen(ILI9341_BLACK);
        centered_text("TEST LCD", 3, 140, ILI9341_WHITE, ILI9341_BLACK);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN2_WHITE\"}");
        g_display.fillScreen(ILI9341_WHITE);
        centered_text("WHITE SCREEN", 2, 140, ILI9341_BLACK, ILI9341_WHITE);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN3_RED\"}");
        g_display.fillScreen(ILI9341_RED);
        centered_text("RED", 3, 140, ILI9341_WHITE, ILI9341_RED);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN4_GREEN\"}");
        g_display.fillScreen(ILI9341_GREEN);
        centered_text("GREEN", 3, 140, ILI9341_BLACK, ILI9341_GREEN);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN5_BLUE\"}");
        g_display.fillScreen(ILI9341_BLUE);
        centered_text("BLUE", 3, 140, ILI9341_WHITE, ILI9341_BLUE);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"SCREEN6_BORDER\"}");
        g_display.fillScreen(ILI9341_BLACK);
        g_display.drawRect(0, 0, g_display.width(), g_display.height(), ILI9341_WHITE);
        g_display.drawRect(4, 4, g_display.width() - 8, g_display.height() - 8, ILI9341_WHITE);
        centered_text("MESFLOW", 2, 110, ILI9341_WHITE, ILI9341_BLACK);
        centered_text("LCD OK", 2, 140, ILI9341_WHITE, ILI9341_BLACK);
        centered_text("1234567890", 2, 170, ILI9341_WHITE, ILI9341_BLACK);
        delay(3000);

        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_PHASE\",\"phase\":\"FINAL_HOLD\"}");
        g_display.fillScreen(ILI9341_BLACK);
        centered_text("LCD TEST", 2, 100, ILI9341_GREEN, ILI9341_BLACK);
        centered_text("IF YOU CAN READ THIS", 1, 140, ILI9341_GREEN, ILI9341_BLACK);
        centered_text("DISPLAY IS WORKING", 1, 160, ILI9341_GREEN, ILI9341_BLACK);
        // Left on indefinitely -- no further delay/redraw here.

        g_display.bump_frame_id();
        Serial.println("{\"level\":\"INFO\",\"code\":\"LCD_PHYSICAL_TEST_DONE\"}");
      }
      // Serial Visual Debug Fallback: the exact same three payloads the
      // HTTP /debug/* endpoints serve, over USB serial -- for when the host
      // and the device aren't on the same LAN/subnet (HTTP unreachable) but
      // a USB cable still reaches the device. Gated by MESFLOW_DEBUG_API
      // (not compiled into PROD) because g_debug_server itself only exists
      // under that guard; the underlying data these read is always present
      // regardless of profile, but there is deliberately only ONE builder
      // for each payload (DebugServer::build_ui_state_json() /
      // build_device_state_json() / write_screenshot_serial()), reused by
      // both HTTP and serial -- not a second hand-maintained copy.
#if MESFLOW_DEBUG_API
      else if (line == "debug-screenshot") {
        g_debug_server.write_screenshot_serial(Serial);
      } else if (line == "debug-ui-state") {
        g_debug_server.write_ui_state_serial(Serial);
      } else if (line == "debug-device-state") {
        g_debug_server.write_device_state_serial(Serial);
      } else if (line.startsWith("debug-input:")) {
        // Same SCAN/KEY_DOWN/KEY_UP JSON body as POST /debug/input, e.g.
        // debug-input:{"type":"SCAN","value":"00152"} -- needed to drive
        // real state transitions for visual-parity testing when there's no
        // HTTP path to the device and no physical scanner/keypad access.
        g_debug_server.write_input_result_serial(std::string(line.substring(12).c_str()), Serial);
      }
#endif
      line = "";
    } else if (line.length() < 96) {
      line += c;
    }
  }
}

}  // namespace

void loop() {
  g_scanner.poll();
  g_keypad.poll();
  g_wifi_recovery.tick();
  // The recovery portal drives Wi-Fi itself (AP+STA) while active; letting
  // wifi_manager also call WiFi.begin() at the same time would fight over
  // the radio, so pause normal reconnect attempts during recovery.
  if (!g_wifi_recovery.portal_active()) {
    g_wifi.poll();
  }
  poll_serial_provisioning();
  g_runtime.poll();  // non-blocking: picks up completed async event sends (§23)
#if MESFLOW_DEBUG_API
  g_debug_server.poll();
#endif

  if (g_wifi.state() == kiosk::network::WifiState::CONNECTED) {
    if (!g_time_sync_started) {
      g_time_sync_started = true;
      g_time_sync.begin();
    }
    g_time_sync.poll();

    if (!g_bootstrap_attempted) {
      g_bootstrap_attempted = true;
      auto bootstrap_result =
          g_bootstrap.attempt(g_config.api_endpoint(), g_identity.device_id(), g_identity.hardware_id(),
                              g_diagnostics.boot_id, static_cast<uint32_t>(g_runtime.device_seq()));
      // Phase 2: seed StateProjection from the SAME bootstrap response --
      // invariant 15, the device never restores a business state locally
      // across reboot, it always starts from server authority this boot.
      g_runtime.on_bootstrap_result(bootstrap_result);
      // Phase 4: check whether the server wants a different UI bundle than
      // whatever's currently active -- read once at boot (heartbeat could
      // also carry this for faster propagation without a reboot; not
      // implemented yet, see the Phase 4 report's Known Gaps).
      g_ui_sync.check_desired(g_config.api_endpoint(), bootstrap_result.ui_bundle_version,
                             std::string(bootstrap_result.ui_bundle_hash.c_str()));
    }
    g_heartbeat.poll(g_config.api_endpoint());
  }
  // non-blocking: picks up a completed bundle download/verify/activate
  // (§23/§24). A REAL bug caught live via the Serial Visual Debug Fallback
  // tooling: activating a new bundle alone does NOT redraw the currently
  // displayed screen -- nothing else triggers a redraw for a bundle switch
  // with no underlying business-state transition, so the operator would
  // keep looking at stale content until some unrelated event (scan/resync/
  // keypress) happened to redraw. refresh_idle_screen() when poll()
  // reports an activation closes that gap.
  if (g_ui_sync.poll()) {
    g_runtime.refresh_idle_screen();
  }

  unsigned long now = millis();
  if (now - g_last_diagnostics_print_ms >= kDiagnosticsPrintIntervalMs) {
    g_last_diagnostics_print_ms = now;
    kiosk::runtime::refresh_memory_fields(g_diagnostics);
    Serial.printf("{\"level\":\"INFO\",\"code\":\"HEARTBEAT_LOCAL\",\"uptime_ms\":%lu,"
                  "\"free_heap\":%u,\"largest_block\":%u,\"psram_free\":%u,"
                  "\"psram_low\":%s}\n",
                  now, g_diagnostics.free_heap_bytes,
                  g_diagnostics.largest_free_block_bytes,
                  g_diagnostics.psram_free_bytes,
                  kiosk::runtime::is_psram_headroom_low(g_diagnostics) ? "true" : "false");
  }
}
