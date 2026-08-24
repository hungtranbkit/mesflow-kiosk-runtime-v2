#include "json_extract.h"

#include <cctype>
#include <cstdlib>

namespace kiosk::protocol {

namespace {

size_t skip_ws(const std::string& s, size_t pos) {
  while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
  return pos;
}

// Finds `"key":`, returns the position right after the colon (start of the
// value, possibly with leading whitespace still to skip), or npos if the
// key isn't present. Plain substring search, not brace-depth-aware -- fine
// for the flat extractors, which are meant to be called on an
// already-scoped object substring (see json_extract_object for the one
// case that DOES need depth awareness).
size_t find_value_start(const std::string& json, const std::string& key, bool& found) {
  std::string needle = "\"" + key + "\":";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) {
    found = false;
    return std::string::npos;
  }
  found = true;
  return pos + needle.length();
}

}  // namespace

std::string json_extract_object(const std::string& json, const std::string& key) {
  bool found = false;
  size_t pos = find_value_start(json, key, found);
  if (!found) return "";
  pos = skip_ws(json, pos);
  if (pos >= json.size() || json[pos] != '{') return "";

  size_t start = pos;
  int depth = 0;
  for (size_t i = start; i < json.size(); ++i) {
    if (json[i] == '{') {
      ++depth;
    } else if (json[i] == '}') {
      --depth;
      if (depth == 0) return json.substr(start, i - start + 1);
    }
  }
  return "";  // unbalanced braces -- malformed input
}

std::string json_extract_string(const std::string& json, const std::string& key,
                                const std::string& default_value) {
  bool found = false;
  size_t pos = find_value_start(json, key, found);
  if (!found) return default_value;
  pos = skip_ws(json, pos);
  if (pos >= json.size() || json[pos] != '"') return default_value;
  ++pos;  // skip opening quote

  std::string out;
  while (pos < json.size() && json[pos] != '"') {
    if (json[pos] == '\\' && pos + 1 < json.size()) {
      char esc = json[pos + 1];
      switch (esc) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        default: out += esc; break;
      }
      pos += 2;
    } else {
      out += json[pos];
      ++pos;
    }
  }
  return out;
}

bool json_extract_bool(const std::string& json, const std::string& key, bool default_value) {
  bool found = false;
  size_t pos = find_value_start(json, key, found);
  if (!found) return default_value;
  pos = skip_ws(json, pos);
  if (json.compare(pos, 4, "true") == 0) return true;
  if (json.compare(pos, 5, "false") == 0) return false;
  return default_value;
}

int64_t json_extract_int(const std::string& json, const std::string& key, int64_t default_value) {
  bool found = false;
  size_t pos = find_value_start(json, key, found);
  if (!found) return default_value;
  pos = skip_ws(json, pos);

  size_t start = pos;
  size_t end = pos;
  if (end < json.size() && json[end] == '-') ++end;
  size_t digits_start = end;
  while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) ++end;
  if (end == digits_start) return default_value;  // no digits found at all

  return std::strtoll(json.substr(start, end - start).c_str(), nullptr, 10);
}

bool json_has_key(const std::string& json, const std::string& key) {
  bool found = false;
  find_value_start(json, key, found);
  return found;
}

bool json_is_null(const std::string& json, const std::string& key) {
  bool found = false;
  size_t pos = find_value_start(json, key, found);
  if (!found) return false;
  pos = skip_ws(json, pos);
  return json.compare(pos, 4, "null") == 0;
}

std::vector<std::string> json_extract_object_array(const std::string& json, const std::string& key) {
  std::vector<std::string> out;
  bool found = false;
  size_t pos = find_value_start(json, key, found);
  if (!found) return out;
  pos = skip_ws(json, pos);
  if (pos >= json.size() || json[pos] != '[') return out;

  size_t i = pos + 1;
  while (i < json.size()) {
    i = skip_ws(json, i);
    if (i >= json.size() || json[i] == ']') break;  // end of array (or trailing comma before ']')
    if (json[i] != '{') break;  // non-object element -- not supported, stop rather than guess
    size_t start = i;
    int depth = 0;
    for (; i < json.size(); ++i) {
      if (json[i] == '{') {
        ++depth;
      } else if (json[i] == '}') {
        --depth;
        if (depth == 0) {
          out.push_back(json.substr(start, i - start + 1));
          ++i;
          break;
        }
      }
    }
    i = skip_ws(json, i);
    if (i < json.size() && json[i] == ',') ++i;
  }
  return out;
}

}  // namespace kiosk::protocol
