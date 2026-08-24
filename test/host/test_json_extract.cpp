// Host test: plain C++, no Arduino.
#include <cstdio>

#include "../../firmware/kiosk_runtime_v2/src/protocol/json_extract.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

int main() {
  using namespace kiosk::protocol;
  std::printf("test_json_extract\n");

  const std::string sample =
      R"({"accepted":true,"event_id":"e1","server_seq":5012,)"
      R"("state":{"name":"WAIT_OPERATION","version":103},)"
      R"("workflow":{"version":1},)"
      R"("view":{"employee_name":"Nguyễn Văn A","operation_code":null})"
      R"(})";

  check(json_extract_bool(sample, "accepted", false) == true, "top-level bool");
  check(json_extract_string(sample, "event_id") == "e1", "top-level string");
  check(json_extract_int(sample, "server_seq", -1) == 5012, "top-level int");

  std::string state_obj = json_extract_object(sample, "state");
  check(!state_obj.empty(), "nested object extracted (state)");
  check(json_extract_string(state_obj, "name") == "WAIT_OPERATION", "nested string (state.name)");
  check(json_extract_int(state_obj, "version", -1) == 103, "nested int (state.version)");

  std::string workflow_obj = json_extract_object(sample, "workflow");
  check(json_extract_int(workflow_obj, "version", -1) == 1, "sibling nested object doesn't collide (workflow.version)");

  std::string view_obj = json_extract_object(sample, "view");
  check(json_has_key(view_obj, "operation_code"), "operation_code key present");
  check(json_is_null(view_obj, "operation_code"), "operation_code is null, detected as such");
  check(!json_has_key(view_obj, "session_id"), "a field never sent at all -> not present (has_key false)");

  // Escaping
  {
    std::string escaped = R"({"message":"line1\nline2 say \"hi\""})";
    check(json_extract_string(escaped, "message") == "line1\nline2 say \"hi\"", "escapes unescaped correctly");
  }

  // Negative numbers, missing keys, malformed
  check(json_extract_int(sample, "does_not_exist", -7) == -7, "missing int key returns default");
  check(json_extract_string(sample, "does_not_exist", "fallback") == "fallback", "missing string key returns default");
  check(json_extract_int(R"({"n":-42})", "n", 0) == -42, "negative integer parses correctly");

  check(json_extract_object(R"({"broken":{"a":1)", "broken").empty(), "unbalanced braces -> empty, not a crash/garbage substring");

  // Array-of-objects extraction (UI bundle screens/components)
  {
    std::string arr_json =
        R"({"screens":[{"id":"a","components":[{"type":"text","x":1}]},)"
        R"({"id":"b","components":[]}],"other":1})";
    auto screens = json_extract_object_array(arr_json, "screens");
    check(screens.size() == 2, "array of 2 objects extracted");
    check(json_extract_string(screens[0], "id") == "a", "first element's field readable");
    check(json_extract_string(screens[1], "id") == "b", "second element's field readable");

    auto components = json_extract_object_array(screens[0], "components");
    check(components.size() == 1, "nested array inside an array element works");
    check(json_extract_string(components[0], "type") == "text", "nested array element field readable");

    check(json_extract_object_array(arr_json, "missing_key").empty(), "missing array key -> empty vector");
    check(json_extract_object_array(R"({"x":"not an array"})", "x").empty(), "non-array value -> empty vector");
    check(json_extract_object_array(R"({"x":[]})", "x").empty(), "empty array -> empty vector, no crash");
  }

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
