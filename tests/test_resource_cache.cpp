#include "save_io_fixture.hpp"
#include "slrr/script_cfg.hpp"
#include "slrr/scene_instances.hpp"
#include <thread>
#include <cstring>
#include <atomic>
#include <chrono>
using namespace slrr;
void pack(const fs::path& path,const std::string& payload,
          uint32_t parent=0,const std::string& dependency={}) {
  const size_t dep_bytes=dependency.empty()?0:64;
  std::vector<unsigned char> b(55+dep_bytes+payload.size());
  auto put=[&](size_t p,uint32_t v){for(int i=0;i<4;++i)b[p+i]=static_cast<unsigned char>(v>>(i*8));};
  std::memcpy(b.data(),"RPAK",4);put(4,0x200);
  if(dep_bytes){put(8,1);put(16,0x10000);std::memcpy(b.data()+20,dependency.data(),dependency.size());}
  put(16+dep_bytes,23);put(20+dep_bytes,1);put(32+dep_bytes,parent);
  put(36+dep_bytes,0x004c);b[40+dep_bytes]=14;put(46+dep_bytes,uint32_t(55+dep_bytes));put(50+dep_bytes,uint32_t(payload.size()));
  std::memcpy(b.data()+55+dep_bytes,payload.data(),payload.size());
  std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(b.data()),b.size());
}

void many_packs(Fixture& f) {
  constexpr int count=1024;
  const auto root=f.root/"many";
  fs::create_directories(root/"cars");
  pack(root/"base.rpk","flags 8\n");
  // Binary payloads stay uncached, exercising stream eviction and reopening.
  const std::string payload("x\0y",3);
  for(int i=0;i<count;++i)
    pack(root/"cars"/("car"+std::to_string(i)+".rpk"),payload,0x1004c,"base.rpk");
  RpkIndex index;index.set_root(root.string());
  SLRR_REQUIRE(index.open_all()==count+1&&index.pack_count()==count+2);
  const auto* base=index.pack_by_path("base.rpk");SLRR_REQUIRE(base);
  const auto parent=RpkIndex::make_full_id(base->pack_id,0x4c);
  std::vector<uint32_t> expected;
  for(int i=1;i<index.pack_count();++i)if(i!=base->pack_id)expected.push_back(RpkIndex::make_full_id(i,0x4c));
  const auto start=std::chrono::steady_clock::now();
  for(int i=0;i<100;++i)SLRR_REQUIRE(index.children_of(parent)==expected);
  std::printf("SCALE %d packs: 100 child queries %.3f ms\n",count+1,
      std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
  const auto generation=index.generation();
  SLRR_REQUIRE(index.open_all()==count+1&&index.generation()==generation);
  DWORD before=0,after=0;SLRR_REQUIRE(GetProcessHandleCount(GetCurrentProcess(),&before));
  size_t misses=0;
  for(auto id:expected)if(index.read_payload(id)!=std::vector<uint8_t>{'x',0,'y'})++misses;
  SLRR_REQUIRE(GetProcessHandleCount(GetCurrentProcess(),&after));
  std::printf("SCALE payload misses=%zu retained handle delta=%ld\n",misses,long(after)-long(before));
  SLRR_REQUIRE(misses==0&&after<=before+68);
  SLRR_REQUIRE(index.read_payload(expected.front())==std::vector<uint8_t>({'x',0,'y'}));
  std::atomic<bool> ok=true;
  std::vector<std::thread> readers;
  for(int t=0;t<8;++t)readers.emplace_back([&,t]{
    for(size_t i=t;i<expected.size();i+=8)
      if(index.read_payload(expected[i])!=std::vector<uint8_t>{'x',0,'y'}||index.children_of(parent)!=expected)ok=false;
  });
  for(auto& t:readers)t.join();SLRR_REQUIRE(ok);
  pack(root/"late.rpk",payload,0x1004c,"base.rpk");
  const auto late=index.open_lib("late.rpk");
  expected.push_back(RpkIndex::make_full_id(late,0x4c));
  SLRR_REQUIRE(index.children_of(parent)==expected);
  fs::create_directories(root/"duplicate");pack(root/"duplicate/base.rpk",payload);
  index.open_lib("duplicate/base.rpk");
  SLRR_REQUIRE(index.pack_by_name("missing/BASE.rpk")->pack_id==RpkIndex::pack_from_full(parent));
  SLRR_REQUIRE(index.pack_by_name("duplicate/base.rpk")->pack_id!=RpkIndex::pack_from_full(parent));
  std::puts("PASS >286/512/1024 packs, bounded streams, concurrent reopen and child index invalidation");

  f.vm.set_game_root(root.string());f.packs.set_root(root.string());
  Object dir;dir.str="cars";
  auto scan=[&]{return f.registry.invoke("java.lang.System","rpkScan","(Ljava.lang.String;)I",nullptr,{Value::make_obj(&dir)}).v.i;};
  SLRR_REQUIRE(scan()==count&&scan()==count&&f.packs.pack_count()==count+1);
  Object name;name.str=f.packs.pack_by_id(count)->path;
  const auto mounted=f.registry.invoke("java.lang.System","openLib","(Ljava.lang.String;)I",nullptr,{Value::make_obj(&name)}).v.i;
  SLRR_REQUIRE(mounted>286&&mounted==f.packs.pack_by_path(name.str)->pack_id);
  SLRR_REQUIRE(f.open("save/scale",1)==1);
  SLRR_REQUIRE(f.write_resource(int32_t(RpkIndex::make_full_id(mounted,0x4c)))==4);
  SLRR_REQUIRE(f.write_int(99)==4);f.close();
  f.packs.set_root(f.root.string());f.packs.set_root(root.string());
  const auto remounted=f.packs.open_lib(name.str);SLRR_REQUIRE(remounted!=mounted);
  SLRR_REQUIRE(f.open("save/scale",0)==1);
  SLRR_REQUIRE(f.read_resource()==int32_t(RpkIndex::make_full_id(remounted,0x4c))&&f.read_int()==99);f.close();
  std::puts("PASS native scan/open above 286 and save remount after pack renumbering");
}

void late_dependency(Fixture& f) {
  const auto root=f.root/"dependency";fs::create_directories(root);
  pack(root/"owner.rpk","flags 8\n",0x1004c,"base.rpk");
  RpkIndex index;index.set_root(root.string());
  const auto owner=index.open_lib("owner.rpk");
  const auto child=RpkIndex::make_full_id(owner,0x4c);
  SLRR_REQUIRE(index.children_of(child).empty()); // Missing dependency falls back to self.
  pack(root/"base.rpk","flags 16\n");
  const auto base=index.open_lib("base.rpk");
  SLRR_REQUIRE(index.children_of(RpkIndex::make_full_id(base,0x4c))==std::vector<uint32_t>{child});
  SLRR_REQUIRE(index.payload_prop(0x4c,"flags")=="16"); // Last-pack-wins unscoped IDs.
  index.set_root((f.root/"a").string());index.open_lib("frontend.rpk");
  SLRR_REQUIRE(!index.pack_by_name("base.rpk")&&index.children_of(child).empty());
  std::puts("PASS late dependency and root changes invalidate name/child indexes");
}

void pack_capacity(Fixture& f) {
  const auto root=f.root/"capacity";fs::create_directories(root);
  RpkIndex index;index.set_root(root.string());
  for(int i=1;i<RpkIndex::kMaxPackId;++i)
    SLRR_REQUIRE(index.open_lib("missing"+std::to_string(i)+".rpk")==i);
  pack(root/"a.rpk","flags 8\n");pack(root/"b.rpk","flags 16\n");
  SLRR_REQUIRE(index.open_all()==2&&index.pack_count()==RpkIndex::kMaxPackId+1);
  const bool a=index.pack_by_path("a.rpk")!=nullptr;
  const auto accepted=a?"a.rpk":"b.rpk",refused=a?"b.rpk":"a.rpk";
  SLRR_REQUIRE(index.open_lib(accepted)==RpkIndex::kMaxPackId);
  const auto generation=index.generation();
  SLRR_REQUIRE(index.open_lib(refused)==0&&!index.pack_by_path(refused));
  SLRR_REQUIRE(index.open_all()==2&&index.generation()==generation);
  SLRR_REQUIRE(!index.pack_by_id(0x7000));
  SLRR_REQUIRE(index.payload_prop(RpkIndex::make_full_id(RpkIndex::kMaxPackId,0x4c),"flags")==std::string(a?"8":"16"));
  std::puts("PASS RID exhaustion refuses new packs without aliasing live handles or changing existing IDs");
}
int main() try {
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
  many_packs(f);
  late_dependency(f);
  pack_capacity(f);
  return 0;
} catch(const std::exception& e) {
  std::fprintf(stderr,"FAIL resource_cache: %s\n",e.what());return 1;
}
