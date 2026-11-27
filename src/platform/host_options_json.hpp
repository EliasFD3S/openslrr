#pragma once
// The JSON of the host-only options file (save/game/openslrr_options.json,
// #405): one object of sections, each an object of numbers, booleans and
// strings. Pure text in, text out; host_game_options.cpp owns the file.
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace slrr::options_json {

struct Scalar {
  enum Kind { Number, Bool, String } kind = Number;
  double number = 0;
  bool boolean = false;
  std::string text;
  bool operator==(const Scalar&) const = default;
};

using Section = std::map<std::string, Scalar>;
using Sections = std::map<std::string, Section>;

// Only direct scalar members of section objects are settings. A literal dot
// (including one in a mod key) is never a path separator. On failure out is empty.
bool parse(std::string_view text, Sections& out,
           std::string* error = nullptr);

struct Entry {
  std::string section, key;
  Scalar value;
};

// An object with "version" first, then one object per section in the order
// sections first appear, two-space indented.
std::string write(int version, const std::vector<Entry>& entries);

}  // namespace slrr::options_json
