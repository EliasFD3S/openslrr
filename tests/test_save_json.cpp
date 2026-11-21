// Version 2 save JSON schema: named layouts, fallbacks and strict decoding.
// Portable: links only src/natives/save_json.cpp.
#include "standalone_test.hpp"
#include "save_json.hpp"

#include <bit>
#include <cstdint>
#include <string>
#include <vector>

namespace {
using namespace slrr::save_json;

// Builds a leaf stream the way File.write records it, including the
// script frames (method owner, self) that received the File.
struct Builder {
  Document doc;
  std::vector<int> stack;
  int self_tag = 0;
  void enter(const std::string& owner, const void* self, const std::string& cls) {
    Frame frame;
    frame.owner = owner;
    frame.method = "save";
    frame.self = self;
    frame.self_class = cls;
    frame.parent = stack.empty() ? -1 : stack.back();
    doc.frames.push_back(frame);
    stack.push_back(static_cast<int>(doc.frames.size() - 1));
  }
  void leave() { stack.pop_back(); }
  Leaf& add(Leaf leaf) {
    leaf.frame = stack.empty() ? -1 : stack.back();
    doc.leaves.push_back(std::move(leaf));
    return doc.leaves.back();
  }
  void i(int32_t v) { Leaf l; l.word = static_cast<uint32_t>(v); add(l); }
  void f(float v) { Leaf l; l.type = LeafType::Float; l.word = std::bit_cast<uint32_t>(v); add(l); }
  void s(const char* text) {
    Leaf l; l.type = LeafType::String;
    if (text) l.text = text; else l.null_string = true;
    add(l);
  }
  void r(const char* pack, uint32_t local) {
    Leaf l; l.type = LeafType::Resource;
    if (pack) { l.res = ResKind::Pack; l.pack = pack; l.local = local; }
    add(l);
  }
  void blob(int32_t ordinal) {
    Leaf l; l.type = LeafType::Resource; l.res = ResKind::Blob; l.blob = ordinal; add(l);
  }
  // Part.save for `self`, run inside the subclass frames given by `chain`
  // (most derived first), with `extras` written by the subclass afterwards.
  template <class Children, class Extras>
  void part(const void* self, const std::string& cls, const char* type_pack,
            Children children, Extras extras) {
    enter("java.game.parts." + cls, self, cls);
    enter("java.game.parts.Part", self, cls);
    r(type_pack, 3); i(1); r("parts\\textures.rpk", 9); r(nullptr, 0); f(0.25f); f(0.5f);
    children();
    i(-1);
    leave();
    extras();
    leave();
  }
};

std::string encoded(const Document& doc, std::string* kind) { return encode(doc, kind); }

void require_roundtrip(const Document& doc, const char* expected_kind) {
  std::string kind;
  const std::string text = encoded(doc, &kind);
  SLRR_REQUIRE(kind == expected_kind);
  Document back;
  std::string error;
  SLRR_REQUIRE(decode(text, back, &error) == 1);
  SLRR_REQUIRE(same_stream(doc, back));
}

int tags[8];

Builder car_with_frames() {
  Builder b;
  b.i(static_cast<int32_t>(0x88664422u));
  b.i(5);
  b.part(&tags[0], "Chassis", "cars\\racers\\Focer.rpk",
         [&] {
           b.i(2); b.s(nullptr); b.i(7);  // slot 2 holds a tyre on its slot 7
           b.part(&tags[1], "Tyre", "parts\\tyres.rpk", [] {},
                  [&] { b.i(1); b.f(2.1f); });
           b.i(4); b.s("dent"); b.i(0);  // damaged empty slot
           b.i(5); b.s(nullptr); b.i(1);
           b.part(&tags[2], "Block", "parts\\engines\\Emer.rpk", [] {},
                  [&] { b.i(3); b.f(800.f); b.f(7000.f); b.i(9); b.f(1.f); b.f(2.f); b.f(3.f); });
         },
         [&] {
           b.i(3); b.i(2); b.s(nullptr); b.s("flat"); b.f(0.6f); b.f(1234.5f);
           // A mod subclass writing after Chassis keeps a generic block.
           b.doc.frames.push_back({"mods.Spoiler", "save", &tags[0], "Spoiler", b.stack.back()});
           b.stack.push_back(static_cast<int>(b.doc.frames.size() - 1));
           b.s("wing"); b.i(42);
           b.leave();
         });
  for (int k = 0; k < 8; ++k) b.f(float(k) + 0.5f);
  b.i(3); b.i(1); b.f(13.5f);
  return b;
}
}  // namespace

SLRR_TEST(career_file_uses_named_player_racers_and_market) {
  Builder b;
  b.i(static_cast<int32_t>(0x87654321u)); b.i(21);
  b.r("humans.rpk", 231); b.i(5000); b.f(0.25f); b.i(4); b.i(0); b.s("Elias");
  b.i(2); b.i(1); b.i(3); b.i(0);
  // Inventory: one part, one folder holding an empty inventory.
  b.enter("java.game.Inventory", &tags[5], "Inventory");
  b.i(2);
  b.i(0);
  b.part(&tags[3], "Tyre", "parts\\tyres.rpk", [] {}, [&] { b.i(1); b.f(1.9f); });
  b.i(1); b.s("Set A"); b.s(nullptr); b.i(0);
  b.leave();
  b.i(1); b.blob(1); b.i(5);  // one decal
  for (int racer = 0; racer < 2; ++racer) {
    b.i(racer + 1); b.i(1000 + racer); b.f(0.1f);
    for (int car = 0; car < 2; ++car) {
      b.enter("java.game.VehicleDescriptor", &tags[6], "VehicleDescriptor");
      b.i(3); b.r("cars\\racers\\MC.rpk", 6); b.i(16); b.f(1.f); b.f(1.f); b.f(1.f);
      b.f(1.f); b.f(515.f); b.f(611.f); b.s("MC GT"); b.f(12.5f); b.f(9.5f);
      b.leave();
    }
    b.i(1); b.i(1); b.i(0); b.i(2); b.f(13.75f);
  }
  b.i(2); b.i(0); b.i(1);
  b.enter("java.game.VehicleDescriptor", &tags[6], "VehicleDescriptor");
  b.i(3); b.r("cars\\racers\\MC.rpk", 6); b.i(1); b.f(1.f); b.f(1.f); b.f(1.f);
  b.f(1.f); b.f(5.f); b.f(6.f); b.s("New"); b.f(1.5f); b.f(2.5f);
  b.leave();
  b.i(0);
  b.i(1); b.f(85712.f); b.i(6);
  for (int k = 0; k < 7; ++k) b.i(0);
  b.doc.blobs.push_back({1, 7, {0x89, 'P', 'N', 'G'}});
  require_roundtrip(b.doc, "career");
  const std::string text = encode(b.doc);
  for (const char* needle : {"\"money\": 5000", "\"name\": \"Elias\"", "\"racers\": [",
                             "\"used\": []", "\"new\": [\n", "null", "\"folder\": {",
                             "\"inflation\": 1.9", "\"texture\": {\"blob\": 1}",
                             "\"reserved\": [0, 0, 0, 0, 0, 0, 0]", "\"base64\": \"iVBORw==\"",
                             "\"character\": {\"rpk\": \"humans.rpk\", \"id\": 231}"})
    SLRR_REQUIRE(text.find(needle) != std::string::npos);
}

SLRR_TEST(vehicle_part_tree_names_stock_classes_and_keeps_mod_values) {
  const Builder b = car_with_frames();
  require_roundtrip(b.doc, "vehicle");
  const std::string text = encode(b.doc);
  for (const char* needle : {"\"class\": \"Chassis\"", "\"script\": \"Chassis\"",
                             "\"wheelDamage\": [null, \"flat\"]", "\"mileage\": 1234.5",
                             "\"script\": \"Spoiler\"", "\"values\": [\"wing\", 42]",
                             "\"inflation\": 2.1", "\"damage\": \"dent\"",
                             "\"bestNightQM\": 13.5", "\"rpk\": \"cars/racers/Focer.rpk\""})
    SLRR_REQUIRE(text.find(needle) != std::string::npos);
  // Block wrote an int where the stock script writes a float: generic values.
  SLRR_REQUIRE(text.find("\"script\": \"Block\",\n") != std::string::npos);
  SLRR_REQUIRE(text.find("\"values\": [3, 800.0, 7000.0, 9, 1.0, 2.0, 3.0]") != std::string::npos);
}

SLRR_TEST(stream_without_frames_or_with_unknown_layout_falls_back_to_values) {
  Builder b = car_with_frames();
  for (Leaf& leaf : b.doc.leaves) leaf.frame = -1;  // part data cannot be attributed
  b.doc.frames.clear();
  require_roundtrip(b.doc, "raw");
  Builder other;
  other.i(1); other.f(1.f); other.f(-0.f); other.s(""); other.s(nullptr);
  other.f(std::bit_cast<float>(0x7fc01234u)); other.r(nullptr, 0); other.r("a\\b.rpk", 7);
  require_roundtrip(other.doc, "raw");
  const std::string text = encode(other.doc);
  SLRR_REQUIRE(text.find("\"values\": [") != std::string::npos);
  for (const char* needle : {"1.0", "-0.0", "\"\"", "{\"floatBits\": 2143294004}",
                             "{\"rpk\": null}", "{\"rpk\": \"a/b.rpk\", \"id\": 7}"})
    SLRR_REQUIRE(text.find(needle) != std::string::npos);
}

SLRR_TEST(skin_file_lists_part_textures) {
  Builder b;
  b.i(static_cast<int32_t>(0x82549867u)); b.i(0);
  b.r("parts\\a.rpk", 1); b.blob(1);
  b.r("parts\\b.rpk", 2); b.r("parts\\b.rpk", 3);
  b.i(0);
  b.doc.blobs.push_back({1, 7, {}});
  require_roundtrip(b.doc, "skin");
  // loadSkin reads the zero terminator with readResID.
  b.doc.leaves.back().type = LeafType::Resource;
  require_roundtrip(b.doc, "skin");
}

SLRR_TEST(decoder_accepts_reordered_keys_and_integer_floats) {
  const std::string text =
      "\xef\xbb\xbf{\"blobs\": [], \"vehicle\": {\"fileVersion\": 5, \"fileId\": -2006563806,"
      "\"racesLost\": 1, \"racesWon\": 3, \"bestNightQM\": 13,"
      "\"chassis\": {\"type\": null},"
      "\"maxTestTrackSpeedSq\": 0, \"bestTestTrackAcc\": 0, \"bestTestTrackAcc120\": 0,"
      "\"bestTestTrackTime0\": 0, \"bestTestTrackTime0SpeedSq\": 0, \"bestTestTrackTime1\": 0,"
      "\"bestTestTrackTime2\": 0, \"bestTestTrackTime2SpeedSq\": 0.5},"
      "\"kind\": \"vehicle\", \"version\": 2, \"format\": \"OpenSLRR-save\"}";
  Document doc;
  SLRR_REQUIRE(decode(text, doc) == 1);
  SLRR_REQUIRE(doc.leaves.size() == 14);
  SLRR_REQUIRE(doc.leaves[2].type == LeafType::Resource && doc.leaves[2].res == ResKind::Null);
  SLRR_REQUIRE(doc.leaves[13].type == LeafType::Float &&
               std::bit_cast<float>(doc.leaves[13].word) == 13.f);
  SLRR_REQUIRE(doc.leaves[11].word == 3 && doc.leaves[12].word == 1);
}

SLRR_TEST(version_one_and_non_json_are_left_to_other_readers) {
  Document doc;
  SLRR_REQUIRE(decode("{\"format\":\"OpenSLRR-save\",\"version\":1,\"values\":[],\"packs\":[],\"blobs\":[]}", doc) == 0);
  SLRR_REQUIRE(decode("SDAT", doc) == 0);
  SLRR_REQUIRE(decode("{\"format\":\"Other\",\"version\":2}", doc) == 0);
  SLRR_REQUIRE(decode("", doc) == 0);
}

SLRR_TEST(damaged_version_two_saves_are_rejected) {
  const std::string head = "{\"format\":\"OpenSLRR-save\",\"version\":2,";
  const std::string raw = head + "\"kind\":\"raw\",\"blobs\":[],\"values\":";
  for (const std::string& text : {
           head + "\"kind\":\"raw\",\"values\":[]}",                       // no blobs
           head + "\"blobs\":[],\"values\":[]}",                           // no kind
           head + "\"kind\":\"garage\",\"blobs\":[]}",                     // unknown kind
           head + "\"kind\":\"vehicle\",\"blobs\":[],\"vehicle\":{}}",     // missing fields
           raw + "[{\"blob\":1}]}",                                        // missing blob
           raw + "[{\"rpk\":\"a.rpk\"}]}",                                 // resource without id
           raw + "[{\"rpk\":\"a.rpk\",\"id\":65536}]}",
           raw + "[2147483648]}", raw + "[1e999]}", raw + "[\"\\u0000\"]}",
           raw + "[\"\\u0100\"]}", raw + "[true]}", raw + "[[1]]}",
           head + "\"kind\":\"raw\",\"blobs\":[{\"id\":1,\"kind\":7,\"base64\":\"abc\"}],\"values\":[]}",
           head + "\"kind\":\"raw\",\"blobs\":[{\"id\":1,\"kind\":7,\"base64\":\"\"},"
                  "{\"id\":1,\"kind\":7,\"base64\":\"\"}],\"values\":[]}",
           head + "\"version\":2,\"kind\":\"raw\",\"blobs\":[],\"values\":[]}",  // duplicate key
           head + "\"kind\":\"raw\",\"blobs\":[],\"values\":[]",                // truncated
           std::string("{\"format\":\"OpenSLRR-save\",\"version\":3,\"kind\":\"raw\",\"blobs\":[],\"values\":[]}"),
       }) {
    Document doc;
    // Text that is not valid JSON returns 0 and the version 1 reader, which
    // reserves the "format" opening key, then rejects it.
    SLRR_REQUIRE(decode(text, doc) != 1);
  }
  // A slot number of -1 would end the slot list early.
  Builder b = car_with_frames();
  std::string text = encode(b.doc);
  const size_t slot = text.find("\"slot\": 2");
  SLRR_REQUIRE(slot != std::string::npos);
  text.replace(slot, 9, "\"slot\": -1");
  Document doc;
  SLRR_REQUIRE(decode(text, doc) == -1);
}

int main() {
  const slrr::standalone::Test tests[] = {
    career_file_uses_named_player_racers_and_market,
    vehicle_part_tree_names_stock_classes_and_keeps_mod_values,
    stream_without_frames_or_with_unknown_layout_falls_back_to_values,
    skin_file_lists_part_textures,
    decoder_accepts_reordered_keys_and_integer_floats,
    version_one_and_non_json_are_left_to_other_readers,
    damaged_version_two_saves_are_rejected,
  };
  return slrr::standalone::run("save_json", tests);
}
