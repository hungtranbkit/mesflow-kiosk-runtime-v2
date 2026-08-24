// Host test: plain C++, no Arduino. Exercises the UI bundle parser/token
// resolver/version-negotiation logic in isolation from storage/network.
#include <cstdio>

#include "../../firmware/kiosk_runtime_v2/src/protocol/ui_bundle.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

int main() {
  using namespace kiosk::protocol;
  std::printf("test_ui_bundle\n");

  // --- Well-formed bundle parses ---
  {
    std::string json = R"({
      "manifest": {"version": 12, "sha256": "abc123", "schema_version": 1, "min_runtime_version": "0.4.0"},
      "screens": [
        {"id": "state_wait_employee", "components": [
          {"type": "text", "x": 4, "y": 4, "w": 200, "h": 20, "font_size": 1, "color": "#00FF00",
           "text": "MESFlow Kiosk Runtime v2"},
          {"type": "rect", "x": 0, "y": 0, "w": 240, "h": 320, "color": "#000000"}
        ]},
        {"id": "state_session_active", "components": [
          {"type": "text", "x": 4, "y": 4, "w": 200, "h": 20, "color": "#00FF00", "text": "{{employee_name}}"}
        ]}
      ]
    })";
    UiBundle bundle;
    bool ok = parse_ui_bundle_json(json, bundle);
    check(ok, "well-formed bundle parses");
    check(bundle.manifest.version == 12, "manifest.version");
    check(bundle.manifest.sha256 == "abc123", "manifest.sha256");
    check(bundle.manifest.schema_version == 1, "manifest.schema_version");
    check(bundle.manifest.min_runtime_version == "0.4.0", "manifest.min_runtime_version");
    check(bundle.screens.size() == 2, "2 screens parsed");

    const UiScreen* wait_emp = find_ui_screen(bundle, "state_wait_employee");
    check(wait_emp != nullptr, "find_ui_screen finds an existing screen");
    check(wait_emp->components.size() == 2, "screen's components parsed");
    check(wait_emp->components[0].type == UiComponentType::TEXT, "component[0] type == TEXT");
    check(wait_emp->components[0].text == "MESFlow Kiosk Runtime v2", "component[0] text");
    check(wait_emp->components[1].type == UiComponentType::RECT, "component[1] type == RECT");
    check(wait_emp->components[0].align == "left",
          "align defaults to 'left' when absent (UI centering task -- old bundles unaffected)");

    check(find_ui_screen(bundle, "does_not_exist") == nullptr, "find_ui_screen returns nullptr for unknown id");
  }

  // --- UI centering (Phase 4.1): explicit align field parses ---
  {
    std::string json = R"({
      "manifest": {"version": 1, "sha256": "x", "schema_version": 1, "min_runtime_version": "0.4.0"},
      "screens": [
        {"id": "s", "components": [
          {"type": "text", "x": 0, "y": 10, "font_size": 2, "color": "#FFFFFF", "text": "TITLE", "align": "center"},
          {"type": "text", "x": 0, "y": 30, "font_size": 1, "color": "#FFFFFF", "text": "RIGHT", "align": "right"}
        ]}
      ]
    })";
    UiBundle bundle;
    check(parse_ui_bundle_json(json, bundle), "bundle with align fields parses");
    const UiScreen* s = find_ui_screen(bundle, "s");
    check(s != nullptr, "screen found");
    check(s->components[0].align == "center", "align=\"center\" parsed correctly");
    check(s->components[1].align == "right", "align=\"right\" parsed correctly");
  }

  // --- Malformed bundles are rejected outright, not partially loaded ---
  {
    UiBundle bundle;
    check(!parse_ui_bundle_json(R"({"screens":[{"id":"a","components":[]}]})", bundle),
          "missing manifest entirely -> rejected");
    check(!parse_ui_bundle_json(R"({"manifest":{"version":1},"screens":[]})", bundle),
          "manifest missing schema_version -> rejected");
    check(!parse_ui_bundle_json(R"({"manifest":{"version":1,"schema_version":1}})", bundle),
          "no screens key at all -> rejected");
    check(!parse_ui_bundle_json(R"({"manifest":{"version":1,"schema_version":1},"screens":[]})", bundle),
          "empty screens array -> rejected (a bundle with zero usable screens is unusable)");
  }

  // --- Unknown component type is dropped, not guessed ---
  {
    std::string json = R"({
      "manifest": {"version": 1, "schema_version": 1},
      "screens": [{"id": "s1", "components": [
        {"type": "text", "text": "ok"},
        {"type": "some_future_widget", "text": "unknown"}
      ]}]
    })";
    UiBundle bundle;
    bool ok = parse_ui_bundle_json(json, bundle);
    check(ok, "bundle with one unknown component type still parses overall");
    check(bundle.screens[0].components.size() == 1, "unknown component type silently dropped, not added");
  }

  // --- A malformed individual screen (no id) is dropped, bundle survives if others are valid ---
  {
    std::string json = R"({
      "manifest": {"version": 1, "schema_version": 1},
      "screens": [
        {"components": []},
        {"id": "valid_screen", "components": []}
      ]
    })";
    UiBundle bundle;
    bool ok = parse_ui_bundle_json(json, bundle);
    check(ok, "bundle with one screen missing 'id' still parses (other screen valid)");
    check(bundle.screens.size() == 1, "screen with no id dropped, only valid one kept");
    check(bundle.screens[0].screen_id == "valid_screen", "surviving screen is the valid one");
  }

  // --- Token resolution ---
  {
    ViewModel view;
    view.has_employee_name = true;
    view.employee_name = "Nguyen Van A";
    view.has_target_qty = true;
    view.target_qty = 100;
    view.has_produced_qty = true;
    view.produced_qty = 0;  // KIOSK-092: 0 must still resolve, not be treated as absent

    check(resolve_ui_tokens("Xin chao {{employee_name}}", view) == "Xin chao Nguyen Van A",
          "known, present token resolves");
    check(resolve_ui_tokens("Target: {{target_qty}}", view) == "Target: 100", "int token resolves");
    check(resolve_ui_tokens("Done: {{produced_qty}}", view) == "Done: 0",
          "produced_qty=0 resolves as literal 0, not blank (KIOSK-092 parity)");
    check(resolve_ui_tokens("{{operation_name}}", view) == "",
          "a token whose ViewModel field is absent (has_X==false) resolves to empty string");
    check(resolve_ui_tokens("{{not_a_real_field}}", view) == "",
          "an unrecognized token name resolves to empty string, not left literal");
    check(resolve_ui_tokens("no tokens here", view) == "no tokens here", "plain text passes through unchanged");
    check(resolve_ui_tokens("unterminated {{oops", view) == "unterminated {{oops",
          "unterminated {{ doesn't crash or truncate -- emitted verbatim");
  }

  // --- Version/hash negotiation (pure decision function) ---
  {
    check(!ui_bundle_needs_sync(12, "abc", 12, "abc"), "same version+hash -> no sync needed");
    check(ui_bundle_needs_sync(12, "abc", 13, "def"), "different version -> sync needed");
    check(ui_bundle_needs_sync(12, "abc", 12, "different_hash"),
          "same version but different hash -> sync needed (defends against a version reused with new content)");
    check(!ui_bundle_needs_sync(12, "abc", 0, ""), "server said nothing (0/empty) -> treated as 'no info', not a match");
    check(!ui_bundle_needs_sync(12, "abc", 12, ""), "same version, server omitted hash -> trust version alone");
  }

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
