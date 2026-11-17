#include "save_io_fixture.hpp"
#include "slrr/script_cfg.hpp"
#include "slrr/scene_instances.hpp"
#include <thread>
#include <cstring>
#include <atomic>
using namespace slrr;
void pack(const fs::path& path,const std::string& payload) {
  std::vector<unsigned char> b(55+payload.size());
  auto put=[&](size_t p,uint32_t v){for(int i=0;i<4;++i)b[p+i]=static_cast<unsigned char>(v>>(i*8));};
  std::memcpy(b.data(),"RPAK",4);put(4,0x200);put(16,23);put(20,1);
  put(36,0x004c);b[40]=14;put(46,55);put(50,uint32_t(payload.size()));
  std::memcpy(b.data()+55,payload.data(),payload.size());
  std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(b.data()),b.size());
}
int main() {
  Fixture f;
  const auto a=f.root/"a",b=f.root/"b";
  for(auto root:{a,b})fs::create_directories(root/"frontend/scripts");
  pack(a/"frontend.rpk","flags 8\nTexture = first ;comment\ntexture second\n");
  pack(b/"frontend.rpk","flags 16\ntexture other\n");
  std::ofstream(a/"frontend/scripts/page.cfg")<<"render 0x004c\nname RootA\neof\n";
  std::ofstream(b/"frontend/scripts/page.cfg")<<"render 0x004c\nname RootB\neof\n";
  RpkIndex index;index.set_root(a.string());auto pid=index.open_lib("frontend.rpk");
  const auto id=RpkIndex::make_full_id(pid,0x4c);
  std::atomic<bool> ok=true;
  std::vector<std::thread> readers;
  for(int i=0;i<8;++i)readers.emplace_back([&]{for(int j=0;j<100;++j){auto values=index.payload_props(id,"TEXTURE");if(values!=std::vector<std::string>{"first","second"} || index.payload_prop(id,"flags")!="8")ok=false;}});
  for(auto& t:readers)t.join();
  SLRR_REQUIRE(ok&&index.payload_disk_reads()==1&&index.rsd_parse_count()==1);
  SLRR_REQUIRE(index.payload_prop(0x4c,"flags")=="8");
  SLRR_REQUIRE(index.find_entry(pid,0xffff004c)==index.find_entry(pid,0x4c));
  SLRR_REQUIRE(!index.find_entry(pid,0xffff));
  GameRefState first;apply_script_cfg(index,&first,id);
  SLRR_REQUIRE(first.script_name=="RootA");
  scene_instances_set_resources(&index);
  uint32_t flags=0;scene_shadow_info(id,&flags,nullptr);SLRR_REQUIRE(flags==8);
  const auto generation=index.generation();
  index.set_root(b.string());SLRR_REQUIRE(index.generation()!=generation&&index.pack_count()==0);
  SLRR_REQUIRE(index.open_lib("frontend.rpk")==pid);
  SLRR_REQUIRE(index.payload_prop(id,"flags")=="16");
  SLRR_REQUIRE(index.payload_disk_reads()==1&&index.rsd_parse_count()==1);
  GameRefState second;apply_script_cfg(index,&second,id);
  SLRR_REQUIRE(second.script_name=="RootB");
  scene_instances_set_resources(&index);
  scene_shadow_info(id,&flags,nullptr);SLRR_REQUIRE(flags==16);
  scene_instances_set_resources(nullptr);
  {
    RpkIndex different;
    different.set_root(b.string());different.open_lib("missing.rpk");
    const auto fe=different.open_lib("frontend.rpk");
    GameRefState state;apply_script_cfg(different,&state,RpkIndex::make_full_id(fe,0x4c));
    SLRR_REQUIRE(fe!=pid && state.script_name=="RootB");
  }
  {
    RpkIndex active;active.set_root(a.string());active.open_lib("frontend.rpk");rpk_set_active(&active);
    SLRR_REQUIRE(rpk_resolve_resource("frontend.rpk",0x4c)>0xffff);
  }
  SLRR_REQUIRE(rpk_resolve_resource("frontend.rpk",0x4c)==0x4c);
  std::puts("PASS concurrent repeated properties: 1 disk read / 1 RSD parse");
  std::puts("PASS repeated property order, local IDs and misses");
  std::puts("PASS root switch invalidates RPK, frontend CFG and shadow metadata");
  std::puts("PASS active index does not dangle after destruction");
  return 0;
}
