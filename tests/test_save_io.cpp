#include "save_io_fixture.hpp"
#include <bit>

namespace {
void invalid_resource_roundtrip(int32_t id, int restype) {
  Fixture f;
  SLRR_REQUIRE(f.open("save/career/main", 1) == 1);
  SLRR_REQUIRE(f.write_int(0x11223344) == 4);
  SLRR_REQUIRE(f.write_resource(id, restype) == 0); // failure remains visible
  SLRR_REQUIRE(f.write_int(0x55667788) == 4);
  slrr::Object text; text.str = "after-resource";
  f.invoke("write", "(Ljava.lang.String;)I", {slrr::Value::make_obj(&text)}, &f.file);
  const auto& payload = slrr::g_host_files.at(&f.file);
  SLRR_REQUIRE(payload.bytes.size() == 12 + 4 + text.str.size() + 1);
  f.close();
  SLRR_REQUIRE(f.open("save/career/main", 0) == 1);
  SLRR_REQUIRE(f.read_int() == 0x11223344);
  SLRR_REQUIRE(f.read_resource() == 0);
  SLRR_REQUIRE(f.read_int() == 0x55667788);
  const auto restored = f.invoke("readString", "()Ljava.lang.String;", {}, &f.file);
  SLRR_REQUIRE(restored.tag == slrr::ValueTag::Obj && restored.v.o);
  SLRR_REQUIRE(restored.v.o->str == text.str);
  f.close();
}
} // namespace

SLRR_TEST(game_root_cannot_be_deleted_or_opened_as_a_file) {
  Fixture f;
  f.sentinel(f.root / "system/keep.txt");
  for (const auto& path : {".", "./", ".\\.", "save/..", "/"}) {
    SLRR_REQUIRE(f.erase(path) == 0);
    SLRR_REQUIRE(fs::exists(f.root / "system/keep.txt"));
    SLRR_REQUIRE(f.open(path, 1) == 0);
  }
}
SLRR_TEST(nonempty_directories_are_not_deleted_recursively) {
  Fixture f;
  f.sentinel(f.root / "save/career/main");
  SLRR_REQUIRE(f.erase("save/career") == 0);
  SLRR_REQUIRE(fs::exists(f.root / "save/career/main"));
}
SLRR_TEST(stock_delete_contents_then_directory_still_works) {
  Fixture f;
  f.sentinel(f.root / "save/career/main");
  SLRR_REQUIRE(f.erase("save/career/main") == 1);
  SLRR_REQUIRE(f.erase("save/career") == 1);
  SLRR_REQUIRE(f.erase("save/missing") == 1);
  SLRR_REQUIRE(fs::is_directory(f.root));
}
SLRR_TEST(double_dots_inside_names_are_legal_but_traversal_is_rejected) {
  Fixture f;
  SLRR_REQUIRE(!slrr::under_game(f.root.string(), "save/J..R-1/main").empty());
  SLRR_REQUIRE(!slrr::under_game(f.root.string(), "save/Mr...-1/main").empty());
  for (const auto& path : {"../outside", "save/../../outside", "save\\..\\outside", "C:\\outside"})
    SLRR_REQUIRE(slrr::under_game(f.root.string(), path).empty());
}
SLRR_TEST(dotted_career_names_roundtrip_through_file_natives) {
  Fixture f;
  for (const auto& name : {"J..R-1", "Mr...-1", "normal-1"}) {
    const std::string path = std::string("save/career/") + name + "/main";
    SLRR_REQUIRE(f.open(path, 1) == 1);
    SLRR_REQUIRE(f.write_int(1234567) == 4);
    f.close();
    SLRR_REQUIRE(f.open(path, 0) == 1);
    SLRR_REQUIRE(f.read_int() == 1234567);
    f.close();
    SLRR_REQUIRE(f.erase(path) == 1);
  }
}
SLRR_TEST(unknown_pack_resource_keeps_following_save_fields_aligned) {invalid_resource_roundtrip(0x12340056, 0);}
SLRR_TEST(packless_resource_keeps_following_save_fields_aligned) {invalid_resource_roundtrip(0x56, 0);}
SLRR_TEST(unsupported_blob_resource_keeps_following_save_fields_aligned) {invalid_resource_roundtrip(int32_t(0xffff0056u), 1);}
SLRR_TEST(valid_and_null_resources_still_roundtrip) {
  Fixture f;
  const auto pack = f.packs.open_lib("fixture.rpk");
  const auto id = int32_t(slrr::RpkIndex::make_full_id(pack, 0x56));
  SLRR_REQUIRE(f.open("save/valid", 1) == 1);
  SLRR_REQUIRE(f.write_resource(id) == 4);
  SLRR_REQUIRE(f.write_resource(0) == 4);
  SLRR_REQUIRE(f.write_int(321) == 4);
  f.close();
  SLRR_REQUIRE(f.open("save/valid", 0) == 1);
  SLRR_REQUIRE(f.read_resource() == id);
  SLRR_REQUIRE(f.read_resource() == 0);
  SLRR_REQUIRE(f.read_int() == 321);
  f.close();
}
SLRR_TEST(raw_file_resource_write_keeps_its_original_id) {
  Fixture f;
  SLRR_REQUIRE(f.open("save/raw", 1) == 1);
  slrr::g_host_files.at(&f.file).sdat = false;
  SLRR_REQUIRE(f.write_resource(0x12340056) == 4);
  f.close();
  SLRR_REQUIRE(f.open("save/raw", 0) == 1);
  SLRR_REQUIRE(f.read_resource() == 0x12340056);
  f.close();
}
namespace {
std::string disk_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
void put_file(Fixture& f, const std::string& data) {
  std::ofstream out(f.root / "input", std::ios::binary);
  out.write(data.data(), data.size());
  SLRR_REQUIRE(bool(out));
}
std::string json_save(const std::string& values, const std::string& packs = "[]",
                      const std::string& blobs = "[]") {
  return "{\"format\":\"OpenSLRR-save\",\"version\":1,\"values\":" + values +
      ",\"packs\":" + packs + ",\"blobs\":" + blobs + "}";
}
void append_u32(std::string& out, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) out += static_cast<char>(n >> (8 * i));
}
// Independent legacy fixture builder: never calls the production writer.
std::string legacy_save(int type) {
  std::string out("SDAT\0\1", 6); out += static_cast<char>(type); out += '\0';
  append_u32(out, 0x00010056); // pack-local resource
  append_u32(out, 1); // unsupported embedded resource
  append_u32(out, 0x80000000); // negative-zero float bits
  append_u32(out, 2); out += "x"; out += '\0';
  const size_t trailer = out.size();
  if (type == 2) {
    append_u32(out, 1); // one embedded texture, with real bytes
    append_u32(out, 1); append_u32(out, 7); append_u32(out, 2); out += "ab";
    append_u32(out, 1); // one pack
  }
  append_u32(out, 1); // ordinal
  append_u32(out, 12); out += "fixture.rpk"; out += '\0';
  append_u32(out, static_cast<uint32_t>(out.size() - trailer));
  if (type == 1) append_u32(out, 1); // pack count
  return out;
}
}

SLRR_TEST(new_saves_are_readable_typed_json_with_bit_exact_streams) {
  Fixture f;
  SLRR_REQUIRE(f.open("save/json", 1) == 1);
  for (int32_t n : {INT32_MIN, INT32_MAX, 0x3f800000, 0}) SLRR_REQUIRE(f.write_int(n) == 4);
  const uint32_t float_bits[] = {0, 0x80000000, 1, 0x007fffff, 0x00800000,
      0x3f800001, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc01234};
  for (uint32_t bits : float_bits)
    SLRR_REQUIRE(f.invoke("write", "(F)I", {slrr::Value::make_float(std::bit_cast<float>(bits))}, &f.file).v.i == 4);
  const auto binary = slrr::g_host_files.at(&f.file).bytes;
  f.close();
  const auto text = disk_text(f.root / "save/json");
  SLRR_REQUIRE(text.starts_with("{\n  \"format\": \"OpenSLRR-save\""));
  SLRR_REQUIRE(text.find("[\"int\", -2147483648]") != std::string::npos);
  SLRR_REQUIRE(text.find("[\"float\", -0]") != std::string::npos);
  SLRR_REQUIRE(text.find("[\"floatBits\", 2143294004]") != std::string::npos);
  SLRR_REQUIRE(f.open("save/json", 0) == 1);
  SLRR_REQUIRE(slrr::g_host_files.at(&f.file).bytes == binary);
  SLRR_REQUIRE(f.read_int() == INT32_MIN); SLRR_REQUIRE(f.read_int() == INT32_MAX);
  // Config writes BgScene as int, then reads it as float. Skin writes an int
  // zero sentinel, then reads it with readResID. Do not enforce record types.
  SLRR_REQUIRE(f.invoke("readFloat", "()F", {}, &f.file).v.f == 1.f);
  SLRR_REQUIRE(f.read_resource() == 0);
  for (uint32_t bits : float_bits)
    SLRR_REQUIRE(std::bit_cast<uint32_t>(f.invoke("readFloat", "()F", {}, &f.file).v.f) == bits);
  f.close();
}
SLRR_TEST(json_strings_preserve_byte_encoding_escapes_and_null_distinction) {
  Fixture f;
  SLRR_REQUIRE(f.open("save/strings", 1) == 1);
  slrr::Object empty, text, nul;
  text.str = "quote\" slash\\ line\n\t"; text.str += char(0x80); text.str += char(0xe9);
  nul.str = std::string("before\0after", 12);
  for (auto* value : {&empty, &text, &nul, static_cast<slrr::Object*>(nullptr)})
    f.invoke("write", "(Ljava.lang.String;)I", {slrr::Value::make_obj(value)}, &f.file);
  SLRR_REQUIRE(f.write_int(42) == 4); f.close();
  const auto text_on_disk = disk_text(f.root / "save/strings");
  SLRR_REQUIRE(text_on_disk.find("\\u0080\\u00e9") != std::string::npos);
  SLRR_REQUIRE(text_on_disk.find("[\"string\", null]") != std::string::npos);
  SLRR_REQUIRE(f.open("save/strings", 0) == 1);
  auto read = [&]() { return f.invoke("readString", "()Ljava.lang.String;", {}, &f.file).v.o; };
  auto* a = read(); SLRR_REQUIRE(a && a->str.empty());
  auto* b = read(); SLRR_REQUIRE(b && b->str == text.str);
  auto* c = read(); SLRR_REQUIRE(c && c->str == "before"); // existing strlen contract
  SLRR_REQUIRE(read() == nullptr); SLRR_REQUIRE(f.read_int() == 42); f.close();
}
SLRR_TEST(large_strings_keep_the_existing_open_and_read_string_limits) {
  Fixture f;
  SLRR_REQUIRE(f.open("save/large", 1) == 1);
  SLRR_REQUIRE(f.write_int(321) == 4);
  slrr::Object large; large.str.assign(1u << 20, 'x');
  SLRR_REQUIRE(f.invoke("write", "(Ljava.lang.String;)I", {slrr::Value::make_obj(&large)}, &f.file).v.i == (1u << 20) + 1);
  const auto binary = slrr::g_host_files.at(&f.file).bytes;
  f.close(); SLRR_REQUIRE(f.open("save/large", 0) == 1);
  SLRR_REQUIRE(slrr::g_host_files.at(&f.file).bytes == binary);
  SLRR_REQUIRE(f.read_int() == 321);
  SLRR_REQUIRE(f.invoke("readString", "()Ljava.lang.String;", {}, &f.file).v.o == nullptr);
  f.close();
}
SLRR_TEST(json_resource_paths_remount_in_a_different_session_order) {
  Fixture writer;
  const int pack = writer.packs.open_lib("fixture.rpk");
  SLRR_REQUIRE(writer.open("save/car", 1) == 1);
  SLRR_REQUIRE(writer.write_resource(int32_t(slrr::RpkIndex::make_full_id(pack, 0x56))) == 4);
  SLRR_REQUIRE(writer.write_resource(int32_t(0xffff0056u), 7) == 4);
  SLRR_REQUIRE(writer.write_int(99) == 4); writer.close();
  const auto text = disk_text(writer.root / "save/car");
  SLRR_REQUIRE(text.find("[\"resource\", 65622]") != std::string::npos);
  SLRR_REQUIRE(text.find("fixture.rpk") != std::string::npos);
  Fixture reader;
  reader.packs.open_lib("other.rpk");
  const int mounted = reader.packs.open_lib("fixture.rpk");
  SLRR_REQUIRE(mounted != pack);
  put_file(reader, text);
  SLRR_REQUIRE(reader.open("input", 0) == 1);
  SLRR_REQUIRE(reader.read_resource() == int32_t(slrr::RpkIndex::make_full_id(mounted, 0x56)));
  SLRR_REQUIRE(reader.read_resource() == 0); // existing dynamic-resource limitation
  SLRR_REQUIRE(reader.read_int() == 99); reader.close();
}
SLRR_TEST(legacy_sdat_type_one_and_two_still_load_without_rewriting) {
  Fixture f;
  f.packs.open_lib("other.rpk");
  const int mounted = f.packs.open_lib("fixture.rpk");
  for (int type : {1, 2}) {
    const auto legacy = legacy_save(type); put_file(f, legacy);
    SLRR_REQUIRE(f.open("input", 0) == 1);
    SLRR_REQUIRE(f.read_resource() == int32_t(slrr::RpkIndex::make_full_id(mounted, 0x56)));
    SLRR_REQUIRE(f.read_resource() == 0);
    SLRR_REQUIRE(std::bit_cast<uint32_t>(f.invoke("readFloat", "()F", {}, &f.file).v.f) == 0x80000000);
    auto* s = f.invoke("readString", "()Ljava.lang.String;", {}, &f.file).v.o;
    SLRR_REQUIRE(s && s->str == "x");
    const auto& blobs = slrr::g_host_files.at(&f.file).sdat_blobs;
    SLRR_REQUIRE(blobs.size() == (type == 2 ? 1 : 0));
    if (type == 2) SLRR_REQUIRE(blobs[0].bytes == std::vector<uint8_t>({'a', 'b'}));
    f.close(); SLRR_REQUIRE(disk_text(f.root / "input") == legacy);
  }
}
SLRR_TEST(raw_legacy_stream_including_json_like_initial_integer_still_loads) {
  Fixture f;
  for (int n : {123, 0x227b, 0x7d7b, 0x4141227b}) {
    std::string raw; append_u32(raw, n); append_u32(raw, 0x12340056);
    raw += "OpenSLRR-save"; put_file(f, raw);
    SLRR_REQUIRE(f.open("input", 0) == 1);
    SLRR_REQUIRE(f.read_int() == n); SLRR_REQUIRE(f.read_resource() == 0x12340056); f.close();
  }
}
SLRR_TEST(json_reader_accepts_reordered_fields_utf8_bom_and_edited_numbers) {
  Fixture f;
  put_file(f, "\xef\xbb\xbf {\"blobs\":[],\"packs\":[],\"values\":[[\"float\",1.25e+1],[\"string\",\"caf\xc3\xa9\"]],"
      "\"version\":1,\"format\":\"OpenSLRR-save\"} \n");
  SLRR_REQUIRE(f.open("input", 0) == 1);
  SLRR_REQUIRE(f.invoke("readFloat", "()F", {}, &f.file).v.f == 12.5f);
  auto* s = f.invoke("readString", "()Ljava.lang.String;", {}, &f.file).v.o;
  SLRR_REQUIRE(s && s->str == std::string("caf") + char(0xe9)); f.close();
}
SLRR_TEST(json_embedded_resource_metadata_and_bytes_are_retained) {
  Fixture f;
  put_file(f, json_save("[[\"resource\",1],[\"int\",45]]", "[]", "[[1,7,\"00ff12\"]]"));
  SLRR_REQUIRE(f.open("input", 0) == 1);
  const auto& blobs = slrr::g_host_files.at(&f.file).sdat_blobs;
  SLRR_REQUIRE(blobs.size() == 1 && blobs[0].ordinal == 1 && blobs[0].kind == 7);
  SLRR_REQUIRE(blobs[0].bytes == std::vector<uint8_t>({0, 255, 18}));
  SLRR_REQUIRE(f.read_resource() == 0); // metadata is not a live native resource
  SLRR_REQUIRE(f.read_int() == 45); f.close();
}
SLRR_TEST(corrupt_json_and_sdat_are_rejected_instead_of_becoming_raw_payloads) {
  Fixture f;
  const auto valid = json_save("[]");
  std::vector<std::string> bad = {"{", "{}", "{\"format\":\"OpenSLRR-save\"}", valid + "x",
      "{\"format\":\"Other\",\"version\":1,\"values\":[],\"packs\":[],\"blobs\":[]}",
      "{\"format\":\"OpenSLRR-save\",\"version\":2,\"values\":[],\"packs\":[],\"blobs\":[]}",
      "{\"format\":\"OpenSLRR-save\",\"version\":1,\"version\":1,\"values\":[],\"packs\":[],\"blobs\":[]}",
      json_save("[[\"int\",2147483648]]"), json_save("[[\"int\",-2147483649]]"),
      json_save("[[\"int\",01]]"), json_save("[[\"int\",1.0]]"), json_save("[[\"float\",1e999]]"),
      json_save("[[\"float\",NaN]]"), json_save("[[\"float\",+1]]"), json_save("[[\"float\",1.]]"),
      json_save("[[\"floatBits\",4294967296]]"), json_save("[[\"string\",\"\\u0100\"]]"),
      json_save("[[\"string\",\"\\ud800\"]]"), json_save("[[\"string\",\"\\u0000\"]]"),
      json_save("[[\"string\",\"\\q\"]]"), json_save("[[\"unknown\",0]]"), json_save("[[]]"),
      json_save("[]", "[[1,\"fixture.rpk\"],[1,\"other.rpk\"]]"),
      json_save("[]", "[[0,\"fixture.rpk\"]]"), json_save("[]", "[[1,\"../outside.rpk\"]]"),
      json_save("[]", "[]", "[[1,7,\"f\"]]"), json_save("[]", "[]", "[[1,7,\"gg\"]]"), "SDAT"};
  for (int type : {1, 2}) {
    auto legacy = legacy_save(type); legacy[6] = 3; bad.push_back(legacy);
    legacy = legacy_save(type); legacy.resize(10); bad.push_back(legacy);
    legacy = legacy_save(type); legacy[4] = 2; bad.push_back(legacy);
  }
  // Every nonempty truncated prefix of the emitted format must fail closed.
  for (size_t n = 1; n < valid.size(); ++n) bad.push_back(valid.substr(0, n));
  for (const auto& bytes : bad) {
    put_file(f, bytes); SLRR_REQUIRE(f.open("input", 0) == 0);
    SLRR_REQUIRE(!slrr::g_host_files.contains(&f.file));
  }
}
int main() {
  const slrr::standalone::Test tests[] = {
    game_root_cannot_be_deleted_or_opened_as_a_file,
    nonempty_directories_are_not_deleted_recursively,
    stock_delete_contents_then_directory_still_works,
    double_dots_inside_names_are_legal_but_traversal_is_rejected,
    dotted_career_names_roundtrip_through_file_natives,
    unknown_pack_resource_keeps_following_save_fields_aligned,
    packless_resource_keeps_following_save_fields_aligned,
    unsupported_blob_resource_keeps_following_save_fields_aligned,
    valid_and_null_resources_still_roundtrip,
    raw_file_resource_write_keeps_its_original_id,
    new_saves_are_readable_typed_json_with_bit_exact_streams,
    json_strings_preserve_byte_encoding_escapes_and_null_distinction,
    large_strings_keep_the_existing_open_and_read_string_limits,
    json_resource_paths_remount_in_a_different_session_order,
    legacy_sdat_type_one_and_two_still_load_without_rewriting,
    raw_legacy_stream_including_json_like_initial_integer_still_loads,
    json_reader_accepts_reordered_fields_utf8_bom_and_edited_numbers,
    json_embedded_resource_metadata_and_bytes_are_retained,
    corrupt_json_and_sdat_are_rejected_instead_of_becoming_raw_payloads,
  };
  return slrr::standalone::run("save_io", tests);
}
