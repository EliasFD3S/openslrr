#include "slrr/pedestrian_lines.hpp"
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdio>
int main(){std::ifstream in("game/maps/city/ai/city.walk");std::stringstream text;text<<in.rdbuf();slrr::PedestrianLines lines;if(!lines.load(text.str()))return 2;
 uint32_t rng=42;int trials=0,failed=0;
 for(int n=0;n<200&&trials<20;++n){slrr::PedestrianWalker a;if(!lines.spawn(0,0,0,4000,0,rng,a))continue;
 for(int axis=0;axis<2;++axis){auto b=a;float hx=axis?0:1,hz=axis?1:0;
  if(!lines.inside(a.x-hx*5,a.y,a.z-hz*5,1.05f)||!lines.inside(a.x+hx*8,a.y,a.z+hz*8,1.05f))continue;
  b.x+=hx*6;b.z+=hz*6;a.hx=hx;a.hz=hz;a.vx=hx;a.vz=hz;b.hx=-hx;b.hz=-hz;b.vx=-hx;b.vz=-hz;
  if(!lines.inside(b.x,b.y,b.z,1.05f))continue;
  std::vector<slrr::PedestrianWalker> w{a,b};
  for(int tick=0;tick<400;++tick)lines.step(w,.025f,nullptr,0);
  float progress=(w[0].x-w[1].x)*hx+(w[0].z-w[1].z)*hz;
  bool ok=w[0].ok&&w[1].ok&&progress>0;
  std::printf("crossing %d row=%d start=(%.2f,%.2f) progress=%.3f %s\n",trials,a.row,a.x,a.z,progress,ok?"PASS":"FAIL");++trials;if(!ok)++failed;break;
 }
 }
 std::printf("crossings %d failed / %d\n",failed,trials);return failed||trials!=20?1:0;}
