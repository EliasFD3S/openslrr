#include "slrr/scx_mesh.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <iostream>
#include <chrono>
using Bytes = std::vector<unsigned char>;
void word(Bytes& b, uint32_t v) { for(int i=0;i<4;++i)b.push_back((v>>(i*8))&255); }
void scalar(Bytes& b,float v) { uint32_t bits;std::memcpy(&bits,&v,4);word(b,bits); }
Bytes vertices(bool skin) { Bytes b;word(b,3);word(b,skin?3:1);for(int i=0;i<3;++i){scalar(b,float(i));scalar(b,0);scalar(b,0);}return b; }
int main() {
 int failed=0;
 for(int mode=0;mode<4;++mode) {
  std::vector<std::pair<uint32_t,Bytes>> chunks;
  auto material=[&]{ Bytes b;word(b,0);word(b,0);word(b,0);chunks.push_back({0,b}); };
  auto palette=[&]{ Bytes b;word(b,0);word(b,1);word(b,7);chunks.push_back({3,b}); };
  auto group=[&](bool skin){chunks.push_back({4,vertices(skin)});Bytes b;word(b,3);b.insert(b.end(),{0,0,1,0,2,0});chunks.push_back({5,b});};
  material();if(mode!=2)palette();
  if(mode==1)group(false);
  group(mode!=2);
  if(mode==3)group(false);
  Bytes file{'I','N','V','O'};word(file,4);word(file,uint32_t(chunks.size()));
  uint32_t offset=12+uint32_t(chunks.size())*8;
  for(auto& [type,data]:chunks){word(file,type);word(file,offset);offset+=8+uint32_t(data.size());}
  for(auto& [type,data]:chunks){word(file,type);word(file,8+uint32_t(data.size()));file.insert(file.end(),data.begin(),data.end());}
  // Each executable owns a unique temporary fixture directory.
  auto root=std::filesystem::temp_directory_path()/std::filesystem::path("openslrr-scx-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);auto path=root/"fixture.scx";
  {std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(file.data()),file.size());}
  auto geo=slrr::load_scx_geometry(path.string());std::filesystem::remove(path);std::filesystem::remove(root);
  bool ok=geo.info.error.empty()&&geo.positions.size()==(mode==1||mode==3?18:9);
  if(mode==2)ok=ok&&geo.skin_weights.empty()&&geo.skin_bones.empty();
  else {ok=ok&&geo.skin_weights.size()==geo.positions.size()/3*4&&geo.skin_bones.size()==geo.skin_weights.size();
   if(ok)for(size_t i=0;i<geo.positions.size()/3;++i){bool skin=!(mode==1&&i<3)&&!(mode==3&&i>=3);ok=ok&&geo.skin_weights[i*4]==(skin?1.f:0.f);if(skin)ok=ok&&geo.skin_bones[i*4]==7;}
  }
  std::cout<<"SCX case "<<mode<<": "<<(ok?"PASS":"FAIL")<<'\n';if(!ok)++failed;
 }
 return failed?1:0;
}
