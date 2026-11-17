#include "save_io_fixture.hpp"

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
  };
  return slrr::standalone::run("save_io", tests);
}
