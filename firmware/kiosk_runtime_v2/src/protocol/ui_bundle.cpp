#include "ui_bundle.h"

#include "json_extract.h"

namespace kiosk::protocol {

const char* ui_component_type_to_string(UiComponentType t) {
  switch (t) {
    case UiComponentType::TEXT: return "text";
    case UiComponentType::RECT: return "rect";
    case UiComponentType::LINE: return "line";
    case UiComponentType::UNSUPPORTED: return "unsupported";
  }
  return "unsupported";
}

UiComponentType ui_component_type_from_string(const std::string& s) {
  if (s == "text") return UiComponentType::TEXT;
  if (s == "rect") return UiComponentType::RECT;
  if (s == "line") return UiComponentType::LINE;
  // §11: unknown component type is data, not code -- never guessed into a
  // fallback shape; the renderer skips components it doesn't recognize
  // rather than misrendering something plausible-looking.
  return UiComponentType::UNSUPPORTED;
}

bool UiComponent::operator==(const UiComponent& o) const {
  return type == o.type && x == o.x && y == o.y && w == o.w && h == o.h &&
         font_size == o.font_size && color == o.color && text == o.text && align == o.align;
}

bool UiScreen::operator==(const UiScreen& o) const {
  return screen_id == o.screen_id && components == o.components;
}

bool UiBundleManifest::operator==(const UiBundleManifest& o) const {
  return version == o.version && sha256 == o.sha256 && schema_version == o.schema_version &&
         min_runtime_version == o.min_runtime_version;
}

namespace {

bool parse_component(const std::string& json, UiComponent& out) {
  std::string type_str = json_extract_string(json, "type");
  out.type = ui_component_type_from_string(type_str);
  if (out.type == UiComponentType::UNSUPPORTED) return false;  // caller skips, doesn't add a broken entry

  out.x = static_cast<int16_t>(json_extract_int(json, "x", 0));
  out.y = static_cast<int16_t>(json_extract_int(json, "y", 0));
  out.w = static_cast<int16_t>(json_extract_int(json, "w", 0));
  out.h = static_cast<int16_t>(json_extract_int(json, "h", 0));
  out.font_size = static_cast<uint8_t>(json_extract_int(json, "font_size", 1));
  out.color = json_extract_string(json, "color", "#FFFFFF");
  out.text = json_extract_string(json, "text", "");
  out.align = json_extract_string(json, "align", "left");
  return true;
}

bool parse_screen(const std::string& json, UiScreen& out) {
  out.screen_id = json_extract_string(json, "id", "");
  if (out.screen_id.empty()) return false;  // a screen with no id is unusable -- reject, don't guess one

  out.components.clear();
  for (const auto& comp_json : json_extract_object_array(json, "components")) {
    UiComponent comp;
    if (parse_component(comp_json, comp)) {
      out.components.push_back(comp);
    }
    // else: an unrecognized/malformed component is silently dropped from
    // THIS screen (not a reason to reject the whole bundle) -- matches
    // docs/UI_SCHEMA.md's whitelist philosophy: skip what you don't know,
    // don't guess it into something that might misrender.
  }
  return true;
}

}  // namespace

bool parse_ui_bundle_json(const std::string& json, UiBundle& out) {
  out = UiBundle{};

  std::string manifest_json = json_extract_object(json, "manifest");
  if (manifest_json.empty()) return false;  // no manifest at all -- malformed, reject outright

  if (!json_has_key(manifest_json, "version") || !json_has_key(manifest_json, "schema_version")) {
    return false;  // required manifest fields missing -- reject, don't default them
  }
  out.manifest.version = static_cast<uint32_t>(json_extract_int(manifest_json, "version", 0));
  out.manifest.schema_version = static_cast<uint32_t>(json_extract_int(manifest_json, "schema_version", 0));
  out.manifest.sha256 = json_extract_string(manifest_json, "sha256", "");
  out.manifest.min_runtime_version = json_extract_string(manifest_json, "min_runtime_version", "");

  auto screen_jsons = json_extract_object_array(json, "screens");
  if (screen_jsons.empty()) return false;  // a bundle with zero screens is unusable

  for (const auto& screen_json : screen_jsons) {
    UiScreen screen;
    if (parse_screen(screen_json, screen)) {
      out.screens.push_back(screen);
    }
    // A malformed individual screen (no id) is dropped, same reasoning as
    // components above -- but the bundle as a WHOLE is only rejected if
    // literally nothing usable came out of it (checked below).
  }
  if (out.screens.empty()) return false;

  return true;
}

const UiScreen* find_ui_screen(const UiBundle& bundle, const std::string& screen_id) {
  for (const auto& screen : bundle.screens) {
    if (screen.screen_id == screen_id) return &screen;
  }
  return nullptr;
}

std::string resolve_ui_tokens(const std::string& text_template, const ViewModel& view,
                              const std::string& local_digit_buffer) {
  std::string out;
  out.reserve(text_template.size());
  size_t i = 0;
  while (i < text_template.size()) {
    if (text_template[i] == '{' && i + 1 < text_template.size() && text_template[i + 1] == '{') {
      size_t end = text_template.find("}}", i + 2);
      if (end == std::string::npos) {
        // Unterminated "{{" -- honest fallback: emit the rest verbatim
        // rather than losing it silently or scanning out of bounds.
        out += text_template.substr(i);
        break;
      }
      std::string token = text_template.substr(i + 2, end - (i + 2));
      if (token == "employee_name") {
        out += view.has_employee_name ? view.employee_name : "";
      } else if (token == "operation_code") {
        out += view.has_operation_code ? view.operation_code : "";
      } else if (token == "operation_name") {
        out += view.has_operation_name ? view.operation_name : "";
      } else if (token == "session_id") {
        out += view.has_session_id ? view.session_id : "";
      } else if (token == "started_at") {
        out += view.has_started_at ? view.started_at : "";
      } else if (token == "target_qty") {
        if (view.has_target_qty) out += std::to_string(view.target_qty);
      } else if (token == "produced_qty") {
        if (view.has_produced_qty) out += std::to_string(view.produced_qty);
      } else if (token == "local_digit_buffer") {
        out += local_digit_buffer;
      }
      // else: unrecognized token name -> resolves to "" (never left as
      // literal "{{...}}" text on a real screen).
      i = end + 2;
    } else {
      out += text_template[i];
      ++i;
    }
  }
  return out;
}

bool ui_bundle_needs_sync(uint32_t local_version, const std::string& local_hash,
                          uint32_t desired_version, const std::string& desired_hash) {
  // §7/§9: "server didn't say" (0 / empty) must never be treated as a
  // match -- that would silently skip a real update the very first time a
  // response happens to omit these fields.
  if (desired_version == 0 && desired_hash.empty()) return false;  // nothing to compare against; stay put
  if (local_version != desired_version) return true;
  if (!desired_hash.empty() && local_hash != desired_hash) return true;
  return false;
}

}  // namespace kiosk::protocol
