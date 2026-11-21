#include "slrr/pedestrian_lines.hpp"
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <vector>
// Crowds must keep moving. Walkers used to pin themselves on the gates of narrow
// corridors (the wall clearance left no room to cross) and stand still forever.
static int stuck_walkers(const slrr::PedestrianLines& lines,float cx,float cz,uint32_t rng){
 const float radius=200.f,dt=1.f/30;
 std::vector<slrr::PedestrianWalker> w;
 int want=static_cast<int>(0.03f*lines.area_within(cx,0,cz,radius));
 for(int i=0;i<want;++i){slrr::PedestrianWalker a;if(lines.spawn(cx,0,cz,radius,0,rng,a))w.push_back(a);}
 std::vector<float> mx(w.size()),mz(w.size());std::vector<char> was(w.size(),0),stuck(w.size(),0);
 const int window=20*30;
 for(int t=0;t<=100*30;++t){
  if(t)lines.step(w,dt,nullptr,0);
  if(t%window)continue;
  for(size_t i=0;t&&i<w.size();++i){
   bool still=w[i].ok&&std::hypot(w[i].x-mx[i],w[i].z-mz[i])<2.f;
   if(still&&was[i])stuck[i]=1;
   was[i]=still;}
  for(size_t i=0;i<w.size();++i){mx[i]=w[i].x;mz[i]=w[i].z;}
 }
 int n=0;for(char c:stuck)n+=c;
 std::printf("crowd at (%.0f,%.0f): %zu walkers, %d stuck for 40 s\n",cx,cz,w.size(),n);
 return n;}
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
 std::printf("crossings %d failed / %d\n",failed,trials);
 int crowded=stuck_walkers(lines,100,1200,10)+stuck_walkers(lines,-800,800,11);
 std::printf("crowds %d stuck\n",crowded);
 return failed||trials!=20||crowded>3?1:0;}
