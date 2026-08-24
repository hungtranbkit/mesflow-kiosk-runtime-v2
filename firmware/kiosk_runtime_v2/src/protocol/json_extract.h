// Plain C++, no Arduino.h — host-testable (see test/host/). Hand-rolled,
// consistent with this project's choice to keep src/protocol/ free of
// ArduinoJson (see protocol_codec.h). NOT a general-purpose JSON parser --
// only handles the flat/one-nested-level shapes this project's own
// endpoints actually produce (docs/PROTOCOL.md). Fine because both ends
// (device and mock_backend.py) are controlled by this same project.
#pragma once

#include <string>
#include <vector>

namespace kiosk::protocol {

// Returns the substring holding the object value of `key` in `json`
// (`{"key": {...}, ...}` -> `{...}`, braces included), using brace-depth
// counting so it doesn't get confused by nested objects. "" if not found
// or if the value isn't an object.
std::string json_extract_object(const std::string& json, const std::string& key);

// Flat field extraction: `{"key":"value", ...}` -> "value". Searches only
// at the top level of the given string (call json_extract_object first to
// scope into a nested object). Unescapes \" \\ \n \r \t.
std::string json_extract_string(const std::string& json, const std::string& key,
                                const std::string& default_value = "");

bool json_extract_bool(const std::string& json, const std::string& key, bool default_value);

int64_t json_extract_int(const std::string& json, const std::string& key, int64_t default_value);

// True if `"key":` appears at the top level of `json` at all (regardless
// of value, including null) -- lets a caller distinguish "field present as
// null" from "field entirely absent".
bool json_has_key(const std::string& json, const std::string& key);

// True if `"key":null` specifically.
bool json_is_null(const std::string& json, const std::string& key);

// Returns the substring of each object element in the array value of `key`
// (`{"key":[{...},{...}]}` -> ["{...}", "{...}"]), brace-depth-aware so
// nested objects inside each element don't confuse the split. Non-object
// array elements are skipped (this project's bundles only ever put objects
// in arrays -- screens/components -- never bare scalars). "" / not-an-array
// / missing key all return an empty vector, never a partial/guessed result.
std::vector<std::string> json_extract_object_array(const std::string& json, const std::string& key);

}  // namespace kiosk::protocol
