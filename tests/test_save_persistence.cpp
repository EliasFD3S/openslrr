#include "save_io_fixture.hpp"
#include "slrr/part_runtime.hpp"

namespace {
std::string disk_bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
void save_int(Fixture& f, const std::string& name, int value) {
  SLRR_REQUIRE(f.open(name,1) == 1);
  SLRR_REQUIRE(f.write_int(value) == 4);
  f.close();
}
int load_int(Fixture& f, const std::string& name) {
  SLRR_REQUIRE(f.open(name,0) == 1);
  const int value=f.read_int(); f.close(); return value;
}
int exists(Fixture& f, const std::string& path) {
  slrr::Object name; name.str=path;
  return f.invoke("exists","(Ljava.lang.String;)I",{slrr::Value::make_obj(&name)}).v.i;
}
int transfer(Fixture& f, const char* operation, const std::string& src, const std::string& dst) {
  slrr::Object a,b; a.str=src;b.str=dst;
  return f.invoke(operation,"(Ljava.lang.String;Ljava.lang.String;)I",
      {slrr::Value::make_obj(&a),slrr::Value::make_obj(&b)}).v.i;
}
struct DenyReplacement {
  HANDLE handle=INVALID_HANDLE_VALUE;
  explicit DenyReplacement(const fs::path& path) {
    handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    SLRR_REQUIRE(handle!=INVALID_HANDLE_VALUE);
  }
  ~DenyReplacement() {if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);}
};
void make_pending(Fixture& f) {
  save_int(f,"save/pending",11);
  DenyReplacement lock(f.root/"save/pending");
  save_int(f,"save/pending",22);
}
bool has_staging_files(const Fixture& f) {
  for(const auto& entry:fs::recursive_directory_iterator(f.root))
    if(entry.path().filename().string().find(".openslrr-save.")!=std::string::npos)return true;
  return false;
}
struct PartFixture : Fixture {
  slrr::Object* part=nullptr;
  PartFixture() {
    {std::ofstream cfg(root/"part.cfg");
      cfg<<"mass 1\nslot 0 0 0 0 0 0 10\nslot 0 0 0 0 0 0 20\n";}
    const std::string payload="native part part.cfg\r\n";
    std::vector<uint8_t> bytes(55,0);
    auto put=[&](size_t offset,uint32_t value){
      for(unsigned i=0;i<4;++i)bytes[offset+i]=uint8_t(value>>(8*i));
    };
    bytes[0]='R';bytes[1]='P';bytes[2]='A';bytes[3]='K';
    put(4,0x200);put(16,23);put(20,1);put(36,6);bytes[40]=8;
    put(46,55);put(50,uint32_t(payload.size()));
    bytes.insert(bytes.end(),payload.begin(),payload.end());
    {std::ofstream rpk(root/"part.rpk",std::ios::binary);
      rpk.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
    const auto pack=packs.open_lib("part.rpk");
    slrr::register_part_natives(vm,packs);
    part=slrr::part_create(vm,packs,nullptr,slrr::RpkIndex::make_full_id(pack,6),"fixture");
    SLRR_REQUIRE(part!=nullptr);
  }
  ~PartFixture(){if(part)slrr::part_destroy(part);}
  slrr::Value damage(int index) {
    return registry.invoke("java.game.parts.Part","getSlotDamage","(I)Ljava.lang.String;",part,
        {slrr::Value::make_int(index)});
  }
  void set_damage(int index,slrr::Object* data) {
    registry.invoke("java.game.parts.Part","setSlotDamage","(ILjava.lang.String;)V",part,
        {slrr::Value::make_int(index),slrr::Value::make_obj(data)});
  }
};
}

SLRR_TEST(locked_replacement_preserves_disk_and_latest_in_memory_save) {
  Fixture f;
  save_int(f,"save/main",11);
  const auto original=disk_bytes(f.root/"save/main");
  {
    DenyReplacement lock(f.root/"save/main");
    save_int(f,"save/main",22);
    SLRR_REQUIRE(disk_bytes(f.root/"save/main")==original);
    SLRR_REQUIRE(load_int(f,"save/main")==22);
    SLRR_REQUIRE(!has_staging_files(f));
  }
  save_int(f,"save/main",33);
  SLRR_REQUIRE(load_int(f,"save/main")==33);
  SLRR_REQUIRE(disk_bytes(f.root/"save/main")!=original);
  SLRR_REQUIRE(!has_staging_files(f));
}
SLRR_TEST(pending_save_remains_visible_without_a_disk_file) {
  Fixture f;make_pending(f);
  fs::remove(f.root/"save/pending");
  SLRR_REQUIRE(exists(f,"save/pending")==1);
  SLRR_REQUIRE(load_int(f,"save/pending")==22);
  SLRR_REQUIRE(f.erase("save/pending")==1);
  SLRR_REQUIRE(exists(f,"save/pending")==0);
}
SLRR_TEST(copy_uses_pending_save_bytes_instead_of_stale_disk_bytes) {
  Fixture f;make_pending(f);
  SLRR_REQUIRE(transfer(f,"copy","save/pending","save/copied")==0);
  SLRR_REQUIRE(load_int(f,"save/copied")==22);
  SLRR_REQUIRE(load_int(f,"save/pending")==22);
}
SLRR_TEST(move_uses_pending_save_bytes_instead_of_stale_disk_bytes) {
  Fixture f;make_pending(f);
  SLRR_REQUIRE(transfer(f,"move","save/pending","save/moved")==0);
  SLRR_REQUIRE(load_int(f,"save/moved")==22);
  SLRR_REQUIRE(exists(f,"save/pending")==0);
}
SLRR_TEST(failed_copy_preserves_pending_source_and_destination) {
  Fixture f;make_pending(f);save_int(f,"save/destination",99);
  const auto original=disk_bytes(f.root/"save/destination");
  {
    DenyReplacement lock(f.root/"save/destination");
    SLRR_REQUIRE(transfer(f,"copy","save/pending","save/destination")==-1);
    SLRR_REQUIRE(disk_bytes(f.root/"save/destination")==original);
    SLRR_REQUIRE(load_int(f,"save/pending")==22);
    SLRR_REQUIRE(!has_staging_files(f));
  }
}
SLRR_TEST(successful_disk_copy_discards_an_older_pending_destination) {
  Fixture f;make_pending(f);save_int(f,"save/source",77);
  SLRR_REQUIRE(transfer(f,"copy","save/source","save/pending")==0);
  SLRR_REQUIRE(load_int(f,"save/pending")==77);
}
SLRR_TEST(successful_disk_move_discards_an_older_pending_destination) {
  Fixture f;make_pending(f);save_int(f,"save/source",77);
  fs::remove(f.root/"save/pending");
  SLRR_REQUIRE(transfer(f,"move","save/source","save/pending")==0);
  SLRR_REQUIRE(load_int(f,"save/pending")==77);
}
SLRR_TEST(empty_and_null_strings_keep_distinct_wire_lengths_and_roundtrip) {
  Fixture f;
  SLRR_REQUIRE(f.open("save/strings",1)==1);
  slrr::Object empty,nonempty;nonempty.str="name";
  auto write=[&](slrr::Object* s){return f.invoke("write","(Ljava.lang.String;)I",{slrr::Value::make_obj(s)},&f.file).v.i;};
  SLRR_REQUIRE(write(&empty)==1);
  SLRR_REQUIRE(write(nullptr)==0);
  SLRR_REQUIRE(write(&nonempty)==5);
  SLRR_REQUIRE(f.write_int(123)==4);
  const auto& bytes=slrr::g_host_files.at(&f.file).bytes;
  SLRR_REQUIRE(bytes[0]==1 && bytes[4]==0 && bytes[5]==0);
  f.close();
  SLRR_REQUIRE(f.open("save/strings",0)==1);
  auto read=[&](){return f.invoke("readString","()Ljava.lang.String;",{},&f.file).v.o;};
  const auto a=read();SLRR_REQUIRE(a && a->str.empty());
  SLRR_REQUIRE(read()==nullptr);
  const auto b=read();SLRR_REQUIRE(b && b->str=="name");
  SLRR_REQUIRE(f.read_int()==123);f.close();
}
SLRR_TEST(undamaged_and_invalid_slots_return_null_without_allocating_strings) {
  PartFixture f;
  const auto heap=f.vm.heap_size();
  for(int index:{-1,0,1,2,999}) SLRR_REQUIRE(f.damage(index).v.o==nullptr);
  SLRR_REQUIRE(f.vm.heap_size()==heap);
}
SLRR_TEST(damage_roundtrips_and_clearing_restores_null) {
  PartFixture f;slrr::Object data;data.str="1,2,3,0,0,0";
  f.set_damage(0,&data);
  const auto value=f.damage(0);
  SLRR_REQUIRE(value.v.o && value.v.o->str=="1.000,2.000,3.000,0.000,0.000,0.000");
  f.set_damage(0,nullptr);
  SLRR_REQUIRE(f.damage(0).v.o==nullptr);
}
int main() {
  const slrr::standalone::Test tests[]={
    locked_replacement_preserves_disk_and_latest_in_memory_save,
    pending_save_remains_visible_without_a_disk_file,
    copy_uses_pending_save_bytes_instead_of_stale_disk_bytes,
    move_uses_pending_save_bytes_instead_of_stale_disk_bytes,
    failed_copy_preserves_pending_source_and_destination,
    successful_disk_copy_discards_an_older_pending_destination,
    successful_disk_move_discards_an_older_pending_destination,
    empty_and_null_strings_keep_distinct_wire_lengths_and_roundtrip,
    undamaged_and_invalid_slots_return_null_without_allocating_strings,
    damage_roundtrips_and_clearing_restores_null,
  };
  return slrr::standalone::run("save_persistence",tests);
}
