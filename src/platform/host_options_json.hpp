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
};

// Every scalar under its dotted path ("input.ffb_damper", "version").
// Arrays and nulls are skipped. Returns false, with out cleared, when the
// text is not one valid JSON object. In flat-scalar mode reject duplicate or
// empty keys, nulls and containers; Number.text retains its JSON spelling.
bool parse(std::string_view text, std::map<std::string, Scalar>& out,
           std::string* error = nullptr, bool flat_scalars_only = false);

struct Entry {
  std::string section, key;
  Scalar value;
};

// An object with "version" first, then one object per section in the order
// sections first appear, two-space indented.
std::string write(int version, const std::vector<Entry>& entries);

}  // namespace slrr::options_json
