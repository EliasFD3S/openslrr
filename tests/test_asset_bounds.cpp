#include "standalone_test.hpp"
#include "slrr/scx_mesh.hpp"
#include "slrr/format/matrixdata.hpp"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <chrono>
using Bytes=std::vector<uint8_t>;
void word(Bytes& b,uint32_t x){for(int i=0;i<4;++i)b.push_back(uint8_t(x>>(8*i)));}
void real(Bytes& b,float x){uint32_t v;std::memcpy(&v,&x,4);word(b,v);}
void group(Bytes& b,uint32_t index){size_t pos=b.size();b.resize(pos+56);b[pos]=56;word(b,3);b.resize(b.size()+3*44);word(b,1);word(b,0);word(b,1);word(b,index);}
void scx(bool invalid,bool second=false){
 Bytes b{'I','N','V','O'};word(b,3);group(b,invalid?3:2);if(second)group(b,2);
 auto dir=std::filesystem::temp_directory_path()/std::filesystem::path("openslrr-bounds-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directory(dir);auto path=dir/"test.scx";
 {std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());}
 auto info=slrr::parse_scx_file(path.string());auto geo=slrr::load_scx_geometry(path.string());
 std::filesystem::remove(path);std::filesystem::remove(dir);
 SLRR_REQUIRE(info.ok==!invalid&&geo.info.ok==!invalid);
 if(invalid)SLRR_REQUIRE(info.error=="v3 face vertex index out of range"&&geo.indices.empty());
 else SLRR_REQUIRE(geo.indices.size()==(second?6:3));
}
SLRR_TEST(valid_scx){scx(false,true);}
SLRR_TEST(out_of_group){scx(true);}
SLRR_TEST(next_group_not_valid){scx(true,true);}
SLRR_TEST(truncated_keys){Bytes b;word(b,1);word(b,100);slrr::Clip clip;std::string error;SLRR_REQUIRE(!slrr::matrixdata_read(b.data(),b.size(),&clip,&error));SLRR_REQUIRE(clip.times.empty());SLRR_REQUIRE(clip.tracks[0].rotation.empty()&&clip.tracks[0].direction.empty());}
SLRR_TEST(valid_clip){Bytes b;word(b,1);word(b,1);for(float x:{0.f,0.f,0.f,0.f,1.f,1.f,2.f,3.f})real(b,x);b.insert(b.end(),{'b','o','n','e'});slrr::Clip clip;std::string e;SLRR_REQUIRE(slrr::matrixdata_read(b.data(),b.size(),&clip,&e));SLRR_REQUIRE(clip.times.size()==1&&clip.tracks[0].name=="bone");}
int main(){const slrr::standalone::Test tests[]={valid_scx,out_of_group,next_group_not_valid,truncated_keys,valid_clip};return slrr::standalone::run("asset_bounds",tests);}
