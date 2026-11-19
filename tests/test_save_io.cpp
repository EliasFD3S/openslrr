#include "save_io_fixture.hpp"
#include "slrr/paint_texture.hpp"
#include "slrr/gameref.hpp"

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
SLRR_TEST(painter_texture_roundtrips_through_the_save_trailer) {
  Fixture f;
  slrr::PaintImage image;
  image.width = 16;
  image.height = 8;
  image.rgba.resize(16 * 8 * 4);
  for (size_t i = 0; i < image.rgba.size(); ++i) image.rgba[i] = static_cast<uint8_t>(i * 7);
  const uint32_t id = slrr::paint_texture_create(image);
  SLRR_REQUIRE(id != 0);
  SLRR_REQUIRE(f.open("save/painted", 1) == 1);
  SLRR_REQUIRE(f.write_resource(int32_t(id), 7) == 4);
  SLRR_REQUIRE(f.write_resource(int32_t(id), 7) == 4);  // same blob twice
  SLRR_REQUIRE(f.write_int(99) == 4);
  f.close();
  slrr::paint_texture_release(id);
  SLRR_REQUIRE(f.open("save/painted", 0) == 1);
  const auto restored = static_cast<uint32_t>(f.read_resource());
  SLRR_REQUIRE(slrr::paint_texture_exists(restored));
  SLRR_REQUIRE(static_cast<uint32_t>(f.read_resource()) == restored);
  SLRR_REQUIRE(f.read_int() == 99);
  const slrr::PaintImage* back = slrr::paint_texture_committed(restored);
  SLRR_REQUIRE(back && back->width == 16 && back->height == 8 && back->rgba == image.rgba);
  f.close();
  slrr::paint_texture_release(restored);

  // A dead Painter id is a null placeholder, keeping later fields aligned.
  SLRR_REQUIRE(f.open("save/dead", 1) == 1);
  SLRR_REQUIRE(f.write_resource(int32_t(id), 7) == 0);
  SLRR_REQUIRE(f.write_int(5) == 4);
  f.close();
  SLRR_REQUIRE(f.open("save/dead", 0) == 1);
  SLRR_REQUIRE(f.read_resource() == 0);
  SLRR_REQUIRE(f.read_int() == 5);
  f.close();
}
SLRR_TEST(stock_side_file_texture_is_loaded_for_an_empty_blob) {
  Fixture f;
  // Stock layout: payload, then one empty type-7 blob and no packs; the
  // image is "<save>.<ordinal>" beside the file.
  std::vector<uint8_t> file = {'S', 'D', 'A', 'T', 0x00, 0x01, 0x02, 0x00};
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) file.push_back(uint8_t(v >> (8 * i))); };
  u32(1);  // payload: the wired ordinal
  u32(1); u32(1); u32(7); u32(0);  // one blob: ordinal 1, kind 7, size 0
  u32(0);  // no packs
  u32(20);
  fs::create_directories(f.root / "save/stock");
  { std::ofstream out(f.root / "save/stock/PlayerCar0", std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size())); }
  slrr::PaintImage image;
  image.width = image.height = 4;
  image.rgba.assign(64, 200);
  const auto png = slrr::paint_png_encode(image);
  { std::ofstream out(f.root / "save/stock/PlayerCar0.1", std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size())); }
  SLRR_REQUIRE(f.open("save/stock/PlayerCar0", 0) == 1);
  const auto restored = static_cast<uint32_t>(f.read_resource());
  SLRR_REQUIRE(slrr::paint_texture_exists(restored));
  SLRR_REQUIRE(slrr::paint_texture_committed(restored)->rgba == image.rgba);
  f.close();
  slrr::paint_texture_release(restored);
}
SLRR_TEST(file_backed_decal_has_an_id_and_survives_save_load_and_resource_ownership) {
  Fixture f;
  slrr::gameref_clear();
  slrr::register_resref_natives(f.vm,f.packs,f.sections);
  slrr::register_painter_natives(f.vm);
  slrr::PaintImage image;
  image.width=image.height=4;
  image.rgba.assign(64,128);
  const auto png=slrr::paint_png_encode(image);
  {std::ofstream out(f.root/"decal.png",std::ios::binary);
   out.write(reinterpret_cast<const char*>(png.data()),std::streamsize(png.size()));}
  slrr::Object texture,path,restored_ref,decal;
  path.str="decal.png";
  auto resource=[&](const char* method,const char* sig,slrr::Object* self,std::vector<slrr::Value> args={}){
    return f.registry.invoke("java.util.resource.ResourceRef",method,sig,self,args);
  };
  resource("makeTexture","(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V",&texture,
           {slrr::Value::make_obj(nullptr),slrr::Value::make_obj(&path)});
  const auto id=resource("id","()I",&texture).v.i;
  SLRR_REQUIRE(id>=0x70000000);
  // Catalog buys new Decal(texture.id()), which binds another wrapper.
  resource("set","(I)V",&decal,{slrr::Value::make_int(id)});
  SLRR_REQUIRE(f.open("save/decal",1)==1);
  SLRR_REQUIRE(f.invoke("write","(Ljava.util.resource.ResourceRef;)I",{slrr::Value::make_obj(&decal)},&f.file).v.i==4);
  f.close();
  resource("deleteNative","()V",&decal);
  resource("deleteNative","()V",&texture);
  SLRR_REQUIRE(f.open("save/decal",0)==1);
  const auto loaded=f.read_resource();
  SLRR_REQUIRE(slrr::paint_texture_exists(uint32_t(loaded)));
  SLRR_REQUIRE(slrr::paint_texture_committed(uint32_t(loaded))->rgba==image.rgba);
  resource("set","(I)V",&restored_ref,{slrr::Value::make_int(loaded)});
  f.close();
  for(int i=0;i<5;++i)slrr::painter_tick();
  SLRR_REQUIRE(slrr::paint_texture_exists(uint32_t(loaded)));
  resource("deleteNative","()V",&restored_ref);
  for(int i=0;i<3;++i)slrr::painter_tick();
  SLRR_REQUIRE(!slrr::paint_texture_exists(uint32_t(loaded)));
  slrr::gameref_clear();
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
    painter_texture_roundtrips_through_the_save_trailer,
    stock_side_file_texture_is_loaded_for_an_empty_blob,
    file_backed_decal_has_an_id_and_survives_save_load_and_resource_ownership,
  };
  return slrr::standalone::run("save_io", tests);
}
