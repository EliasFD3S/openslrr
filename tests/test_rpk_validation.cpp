#include "save_io_fixture.hpp"
#include "slrr/scene_instances.hpp"
#include <cstring>
using namespace slrr;
uint32_t get(const std::vector<uint8_t>& b,size_t p){return uint32_t(b[p])|(uint32_t(b[p+1])<<8)|(uint32_t(b[p+2])<<16)|(uint32_t(b[p+3])<<24);}
void put(std::vector<uint8_t>& b,size_t p,uint32_t v){for(int i=0;i<4;++i)b[p+i]=uint8_t(v>>(8*i));}
void save(const fs::path& p,const std::vector<uint8_t>& b){std::ofstream o(p,std::ios::binary);o.write(reinterpret_cast<const char*>(b.data()),b.size());}
std::vector<uint8_t> pack(const std::vector<uint8_t>& data,uint8_t kind=14){std::vector<uint8_t>b(55+data.size());std::memcpy(b.data(),"RPAK",4);put(b,4,0x200);put(b,16,23);put(b,20,1);put(b,36,1);b[40]=kind;put(b,46,55);put(b,50,uint32_t(data.size()));std::copy(data.begin(),data.end(),b.begin()+55);return b;}
std::vector<uint8_t> node(const std::string& text,int kids){std::vector<uint8_t>b(8+kids*23+8+text.size());put(b,4,kids);put(b,8+kids*23,0x00445352);put(b,12+kids*23,uint32_t(text.size()));std::memcpy(b.data()+16+kids*23,text.data(),text.size());return b;}
int main(int argc,char** argv) try {
 if(argc>1&&std::string(argv[1])=="--corpus"){
  RpkIndex r;r.set_root(fs::absolute("game").string());r.open_all();size_t files=0,bad=0,entries=0;
  for(auto& e:fs::recursive_directory_iterator("game"))if(e.path().extension()==".rpk"){
   ++files;auto rel=fs::relative(e.path(),"game").string();auto* p=r.pack_by_path(rel);
   std::ifstream in(e.path(),std::ios::binary);std::vector<uint8_t> b(16);in.read(reinterpret_cast<char*>(b.data()),16);
   if(!p||p->version!=0x200){++bad;std::printf("REJECT %s\n",rel.c_str());continue;}
   const uint64_t table=16+(uint64_t(get(b,8))+get(b,12))*64;in.seekg(table);std::vector<uint8_t> t(8);in.read(reinterpret_cast<char*>(t.data()),8);
   SLRR_REQUIRE(in&&p->entries.size()==get(t,4));entries+=p->entries.size();
  }
  std::printf("CORPUS packs=%zu accepted_entries=%zu rejected_packs=%zu\n",files,entries,bad);return bad?1:0;
 }
 Fixture f;std::vector<uint8_t> text={'f','l','a','g','s',' ','8','\n'};auto valid=pack(text);
 int serial=0;auto reject=[&](std::vector<uint8_t> data,const char* label){auto path=f.root/("bad"+std::to_string(++serial)+".rpk");save(path,data);RpkIndex r;r.set_root(f.root.string());auto id=r.open_lib(path.filename().string());SLRR_REQUIRE(r.pack_by_id(id)->entries.empty()&&r.pack_by_id(id)->version==0);SLRR_REQUIRE(r.read_payload(RpkIndex::make_full_id(id,1)).empty());std::printf("PASS reject %s\n",label);};
 {auto b=valid;put(b,20,UINT32_MAX);reject(b,"TOC count");}
 {auto b=valid;put(b,8,UINT32_MAX);put(b,12,1);reject(b,"dependency addition overflow");}
 {auto b=valid;put(b,50,UINT32_MAX);reject(b,"payload size");}
 {auto b=valid;put(b,46,UINT32_MAX-4);put(b,50,16);reject(b,"offset plus size overflow");}
 {auto b=valid;b[54]=255;reject(b,"truncated alias");}
 {auto b=valid;b[41]=1;reject(b,"truncated bounds");}
 {auto b=valid;put(b,16,UINT32_MAX);reject(b,"table range");}
 auto path=f.root/"truncated.rpk";save(path,valid);{RpkIndex r;r.set_root(f.root.string());auto id=r.open_lib(path.filename().string());SLRR_REQUIRE(r.find_entry(id,1));fs::resize_file(path,55);SLRR_REQUIRE(r.read_payload(RpkIndex::make_full_id(id,1)).empty()&&r.payload_disk_reads()==0);}
 // A large payload must not be read or allocated while indexing metadata.
 auto metadata=valid;metadata.resize(55+6+48);put(metadata,16,23+6+48);metadata[41]=1;metadata[54]=6;std::memcpy(metadata.data()+55,"alias",6);
 path=f.root/"large.rpk";save(path,metadata);constexpr uint32_t payload_at=256*1024*1024;
 fs::resize_file(path,uint64_t(payload_at)+text.size());
 {std::fstream out(path,std::ios::binary|std::ios::in|std::ios::out);put(metadata,46,payload_at);out.seekp(46);out.write(reinterpret_cast<const char*>(metadata.data()+46),4);out.seekp(payload_at);out.write(reinterpret_cast<const char*>(text.data()),text.size());SLRR_REQUIRE(out);}
 {RpkIndex r;r.set_root(f.root.string());IO_COUNTERS before{},after{};SLRR_REQUIRE(GetProcessIoCounters(GetCurrentProcess(),&before));auto id=r.open_lib("large.rpk");SLRR_REQUIRE(GetProcessIoCounters(GetCurrentProcess(),&after));
  const auto bytes=after.ReadTransferCount-before.ReadTransferCount;std::printf("INDEX 256 MiB pack: read %llu bytes\n",static_cast<unsigned long long>(bytes));
  SLRR_REQUIRE(r.find_entry(id,1)&&r.find_entry(id,1)->alias=="alias"&&bytes<64*1024);
  SLRR_REQUIRE(r.read_payload(RpkIndex::make_full_id(id,1))==text);}
 std::puts("PASS metadata-only indexing and deferred large-offset payload read");
 {fs::create_directories(f.root/"mods/sub");fs::create_directories(f.root/"ok/mods");save(f.root/"mods/sub/skipped.rpk",valid);save(f.root/"ok/mods/loaded.rpk",valid);
  // NTFS permits an unpaired surrogate in an extension or an RPK name.
  const std::wstring lone(1, static_cast<wchar_t>(0xd800));
  for (const auto& filename : {L"bad." + lone, lone + L".rpk"}) {
    save(f.root/filename, valid); SLRR_REQUIRE(fs::is_regular_file(f.root/filename));
  }
  RpkIndex r;r.set_root(f.root.string());r.open_all();
  SLRR_REQUIRE(!r.pack_by_path("mods\\sub\\skipped.rpk")&&!r.pack_by_path("mods/sub/skipped.rpk"));
  SLRR_REQUIRE(r.pack_by_path("ok\\mods\\loaded.rpk")&&!r.pack_by_path("ok\\mods\\loaded.rpk")->entries.empty());
  std::puts("PASS open_all skips top-level mods/ packs but loads nested mods/ packs");}
 auto root=node("params 0x2\n",2);
 for(int k=0;k<2;++k){size_t p=8+k*23;root[p+8]=1;put(root,p+14,55);put(root,p+18,uint32_t(root.size()));}
 path=f.root/"cycle.rpk";save(path,pack(root,1));{RpkIndex r;r.set_root(f.root.string());auto id=r.open_lib("cycle.rpk");auto map=scene_instances_ground_map(r,RpkIndex::make_full_id(id,1));SLRR_REQUIRE(map.render_id==RpkIndex::make_full_id(id,2));}
 auto leaf=node("gametype 0x3\nparams 1,2,3,0,0,0\n",0);root=node("params 0x2\n",2);
 for(int k=0;k<2;++k){size_t p=8+k*23;root[p+8]=1;root[p+9]=0x20;put(root,p+14,uint32_t(55+root.size()));put(root,p+18,uint32_t(leaf.size()));}
 auto shared=pack(root,1);shared.insert(shared.end(),leaf.begin(),leaf.end());path=f.root/"shared.rpk";save(path,shared);
 {RpkIndex r;r.set_root(f.root.string());auto id=r.open_lib("shared.rpk");auto map=scene_instances_ground_map(r,RpkIndex::make_full_id(id,1));SLRR_REQUIRE(map.placements.size()==1&&map.placements[0].position[0]==1);}
 std::puts("PASS truncated disk payload rejected before allocation/read");std::puts("PASS cyclic map terminates and shared node emits one placement");return 0;
} catch(const std::exception& e) {
 std::fprintf(stderr,"FAIL rpk_validation: %s\n",e.what());return 1;
}
