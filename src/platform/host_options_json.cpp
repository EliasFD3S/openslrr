#include "host_options_json.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace slrr::options_json {
namespace {

struct Parser {
  std::string_view s;
  size_t i = 0;
  std::map<std::string, Scalar>* out = nullptr;
  std::string error;
  int depth = 0;

  bool fail(const char* what) {
    if (error.empty()) error = std::string(what) + " at offset " + std::to_string(i);
    return false;
  }
  void space() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  bool literal(std::string_view word) {
    if (s.substr(i, word.size()) != word) return false;
    i += word.size();
    return true;
  }
  bool string(std::string& text) {
    if (i >= s.size() || s[i] != '"') return fail("expected a string");
    ++i;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
      if (c != '\\') {
        text.push_back(c);
        continue;
      }
      if (i >= s.size()) break;
      switch (c = s[i++]) {
        case '"': case '\\': case '/': text.push_back(c); break;
        case 'b': text.push_back('\b'); break;
        case 'f': text.push_back('\f'); break;
        case 'n': text.push_back('\n'); break;
        case 'r': text.push_back('\r'); break;
        case 't': text.push_back('\t'); break;
        case 'u': {
          unsigned code = 0;
          if (i + 4 > s.size() ||
              std::from_chars(s.data() + i, s.data() + i + 4, code, 16).ptr != s.data() + i + 4)
            return fail("bad \\u escape");
          i += 4;
          // Keys and values are ASCII; keep other code points as UTF-8.
          if (code < 0x80) {
            text.push_back(static_cast<char>(code));
          } else if (code < 0x800) {
            text.push_back(static_cast<char>(0xC0 | code >> 6));
            text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            text.push_back(static_cast<char>(0xE0 | code >> 12));
            text.push_back(static_cast<char>(0x80 | (code >> 6 & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default: return fail("bad escape");
      }
    }
    if (i >= s.size()) return fail("unterminated string");
    ++i;
    return true;
  }
  bool number(double& value) {
    const size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' ||
                            s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-'))
      ++i;
    const auto result = std::from_chars(s.data() + start, s.data() + i, value);
    if (result.ec != std::errc{} || result.ptr != s.data() + i || !std::isfinite(value))
      return fail("bad number");
    return true;
  }
  // path: where a scalar is stored; empty for an array element (skipped).
  bool value(const std::string& path) {
    space();
    if (i >= s.size()) return fail("expected a value");
    Scalar scalar;
    const char c = s[i];
    if (c == '{') return object(path);
    if (c == '[') return array();
    if (c == '"') {
      scalar.kind = Scalar::String;
      if (!string(scalar.text)) return false;
    } else if (literal("true")) {
      scalar.kind = Scalar::Bool;
      scalar.boolean = true;
    } else if (literal("false")) {
      scalar.kind = Scalar::Bool;
    } else if (literal("null")) {
      return true;
    } else if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      if (!number(scalar.number)) return false;
    } else {
      return fail("unexpected character");
    }
    if (!path.empty()) (*out)[path] = std::move(scalar);
    return true;
  }
  bool object(const std::string& path) {
    if (++depth > 16) return fail("nested too deep");
    ++i;  // '{'
    space();
    if (i < s.size() && s[i] == '}') {
      ++i;
      --depth;
      return true;
    }
    for (;;) {
      space();
      std::string key;
      if (!string(key)) return false;
      space();
      if (i >= s.size() || s[i] != ':') return fail("expected ':'");
      ++i;
      if (!value(path.empty() && depth == 1 ? key : path.empty() ? "" : path + "." + key))
        return false;
      space();
      if (i < s.size() && s[i] == ',') {
        ++i;
        continue;
      }
      if (i < s.size() && s[i] == '}') {
        ++i;
        --depth;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }
  bool array() {
    if (++depth > 16) return fail("nested too deep");
    ++i;  // '['
    space();
    if (i < s.size() && s[i] == ']') {
      ++i;
      --depth;
      return true;
    }
    for (;;) {
      if (!value("")) return false;
      space();
      if (i < s.size() && s[i] == ',') {
        ++i;
        continue;
      }
      if (i < s.size() && s[i] == ']') {
        ++i;
        --depth;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }
};

void quote(std::string& out, const std::string& text) {
  out.push_back('"');
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char escape[8];
      std::snprintf(escape, sizeof escape, "\\u%04x", static_cast<unsigned char>(c));
      out += escape;
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
}

void scalar(std::string& out, const Scalar& value) {
  switch (value.kind) {
    case Scalar::Bool: out += value.boolean ? "true" : "false"; break;
    case Scalar::String: quote(out, value.text); break;
    case Scalar::Number: {
      char text[32];
      const double n = std::isfinite(value.number) ? value.number : 0;
      if (n == std::floor(n) && std::abs(n) < 1e15) std::snprintf(text, sizeof text, "%.0f", n);
      else std::snprintf(text, sizeof text, "%.17g", n);
      out += text;
      break;
    }
  }
}

}  // namespace

bool parse(std::string_view text, std::map<std::string, Scalar>& out, std::string* error) {
  out.clear();
  Parser p{text, 0, &out};
  p.space();
  // The root object's keys are paths themselves (depth 1).
  bool ok = p.i < text.size() && text[p.i] == '{' ? p.object("") : p.fail("expected an object");
  if (ok) {
    p.space();
    if (p.i != text.size()) ok = p.fail("text after the object");
  }
  if (!ok) {
    out.clear();
    if (error) *error = p.error;
  }
  return ok;
}

std::string write(int version, const std::vector<Entry>& entries) {
  std::vector<std::string> sections;
  for (const auto& e : entries)
    if (std::find(sections.begin(), sections.end(), e.section) == sections.end())
      sections.push_back(e.section);
  std::string out = "{\n  \"version\": " + std::to_string(version);
  for (const auto& section : sections) {
    out += ",\n  ";
    quote(out, section);
    out += ": {";
    bool first = true;
    for (const auto& e : entries) {
      if (e.section != section) continue;
      out += first ? "\n    " : ",\n    ";
      first = false;
      quote(out, e.key);
      out += ": ";
      scalar(out, e.value);
    }
    out += first ? "}" : "\n  }";
  }
  out += "\n}\n";
  return out;
}

}  // namespace slrr::options_json
