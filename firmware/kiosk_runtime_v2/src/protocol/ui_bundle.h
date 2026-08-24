// Plain C++, no Arduino.h — host-testable (see test/host/).
//
// Phase 4 (backend-managed, locally-cached, locally-rendered UI). This is
// the PARSER/DATA MODEL only -- deliberately kept portable and separate
// from storage (storage/ui_bundle_store.*, NVS-backed, Arduino-dependent)
// and download (network/ui_sync_client.*, Arduino-dependent). A component
// whitelist of exactly THREE types (text/rect/line) for this first real
// implementation -- enough to reproduce every screen this runtime
// currently draws by hand; icon/value/status/progress/keypad from
// docs/UI_SCHEMA.md's full aspirational component list are reserved names,
// NOT implemented yet (§ "Known Gaps" in the Phase report covers this).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "state_projection.h"  // ViewModel

namespace kiosk::protocol {

enum class UiComponentType { TEXT, RECT, LINE, UNSUPPORTED };

const char* ui_component_type_to_string(UiComponentType t);
UiComponentType ui_component_type_from_string(const std::string& s);

// A component is deliberately flat and small -- no nested containers, no
// layout modes (ABSOLUTE only), no alignment/wrap engine yet. This is the
// minimum needed to place fixed-position text/rects/lines, matching what
// the hand-coded Renderer screens already do today.
struct UiComponent {
  UiComponentType type = UiComponentType::UNSUPPORTED;
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  uint8_t font_size = 1;  // Display::setTextSize() argument; TEXT only
  std::string color;      // "#RRGGBB"; renderer (Arduino-side) converts to RGB565
  std::string text;       // TEXT only; may contain {{token}} placeholders
  // "left" (default, absent==left -- every bundle authored before this field
  // existed keeps rendering identically), "center", or "right". TEXT only.
  // Deliberately just ONE additive field, not a layout engine: when set to
  // "center"/"right" the renderer (Arduino-side, has real font metrics via
  // getTextBounds()) computes the actual x at render time from the RESOLVED
  // (token-substituted) text's measured width -- never a value guessed at
  // bundle-authoring time, which is the whole point for variable-length
  // fields like employee_name/operation_name (Phase 4.1 UI centering task).
  // `x` is still parsed/stored for LEFT alignment and as a harmless no-op
  // for CENTER/RIGHT (never removed -- keeps old bundles/tests valid).
  std::string align;

  bool operator==(const UiComponent& o) const;
  bool operator!=(const UiComponent& o) const { return !(*this == o); }
};

struct UiScreen {
  std::string screen_id;  // matches this runtime's existing screen ids, e.g. "state_wait_employee"
  std::vector<UiComponent> components;

  bool operator==(const UiScreen& o) const;
  bool operator!=(const UiScreen& o) const { return !(*this == o); }
};

struct UiBundleManifest {
  uint32_t version = 0;
  std::string sha256;
  uint32_t schema_version = 0;
  std::string min_runtime_version;

  bool operator==(const UiBundleManifest& o) const;
  bool operator!=(const UiBundleManifest& o) const { return !(*this == o); }
};

struct UiBundle {
  UiBundleManifest manifest;
  std::vector<UiScreen> screens;
};

// Parses `{"manifest":{...},"screens":[{"id":..,"components":[...]},...]}`.
// Returns false (and leaves `out` default-constructed, never half-filled)
// if the manifest is missing required fields or `screens` isn't a real
// array -- malformed input is rejected outright, never guessed through
// (docs/UI_SCHEMA.md's "no partial load" rule).
bool parse_ui_bundle_json(const std::string& json, UiBundle& out);

// nullptr if no screen with this id exists in the bundle -- caller decides
// what "no matching screen" means (fall back to the built-in hardcoded
// screen for that state; never render nothing).
const UiScreen* find_ui_screen(const UiBundle& bundle, const std::string& screen_id);

// Replaces every {{field_name}} in `text_template` with the corresponding
// ViewModel field's current value. An absent field (has_X == false) or an
// unrecognized token name resolves to "" -- never left as literal
// "{{...}}" text on screen, and malformed/unterminated "{{" never crashes
// or reads out of bounds. `local_digit_buffer` is NOT part of the server's
// ViewModel (invariant 14: it's local/transient UI state, see
// kiosk_runtime.cpp) -- passed separately so {{local_digit_buffer}} can
// still be a normal bundle token without conflating the two data sources.
std::string resolve_ui_tokens(const std::string& text_template, const ViewModel& view,
                              const std::string& local_digit_buffer = "");

// Pure decision function, host-testable in isolation from HTTP/storage:
// true if the device should fetch a new bundle. Empty desired_hash/version
// 0 means "server didn't say" -- never treated as "matches", since that
// would silently skip a real update.
bool ui_bundle_needs_sync(uint32_t local_version, const std::string& local_hash,
                          uint32_t desired_version, const std::string& desired_hash);

}  // namespace kiosk::protocol
