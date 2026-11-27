#include "host_options_json.hpp"
#include "standalone_test.hpp"

using namespace slrr::options_json;

namespace {
void mod_key_round_trip() {
  // Structurally different keys must remain distinct even when they would
  // have the same dotted path. Preserve punctuation, controls and Unicode.
  std::vector<Entry> entries{
      {"video", "msaa", {Scalar::Number, 2}},
      {"mods", "video.msaa", {Scalar::Number, -2147483648.0}},
      {"mods", "", {Scalar::String, 0, false, ""}},
      {"mods", "résumé.\"\\\t", {Scalar::String, 0, false, "Line 1\nLine 2"}},
      {"mods", "test.gain", {Scalar::Number, -1.0f / 3}},
  };
  Sections decoded;
  SLRR_REQUIRE(parse(write(1, entries), decoded));
  for (const auto& e : entries) SLRR_REQUIRE(decoded.at(e.section).at(e.key) == e.value);
}

void only_direct_scalars() {
  Sections sections;
  const char* json = R"({"mods.value":1,"mods":{"nested":{"x":1},"a":[{"b":2}],"n":null,
      "k\ud83c\udfc1":"\ud83d\ude97"},"mods.nested":{"x":3}})";
  SLRR_REQUIRE(parse(json, sections));
  const auto& mods = sections.at("mods");
  SLRR_REQUIRE(mods.size() == 1 && mods.at("k🏁").text == "🚗");
  const char* malformed = R"({"mods":{"x":"\ud83d"}})";
  SLRR_REQUIRE(!parse(malformed, sections) && sections.empty());
}
}

int main() {
  return slrr::standalone::run("host_options_json", {mod_key_round_trip, only_direct_scalars});
}
