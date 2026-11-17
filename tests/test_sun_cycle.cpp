#include "slrr/scene_sun.hpp"
#include <cmath>
#include <cstdio>
#include <initializer_list>

float luma(const float* rgb) {
  return rgb[0]*.2126f + rgb[1]*.7152f + rgb[2]*.0722f;
}
slrr::SceneSun at(float hours) {
  slrr::SceneSun light;
  for (int i=0;i<3;++i) light.diffuse[i]=.5f;
  slrr::scene_sun_set_time(hours*3600.f);
  slrr::scene_sun_apply_day_cycle(light);
  return light;
}
int main() {
  int failed=0;
  auto check=[&](bool ok,const char* name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);failed+=!ok;};
  auto noon=at(12),night=at(0),sunset=at(17.8f),dusk=at(18.5f);
  check(luma(noon.diffuse)>=1.4f && luma(noon.diffuse)<1.5f,"daylight direct energy");
  check(noon.direction[1]<-.8f && noon.has_direction,"noon sun overhead");
  check(luma(night.diffuse)<1e-5f && luma(dusk.diffuse)<1e-5f,"no direct sun after sunset");
  check(std::abs(night.ambient[0]-.10f)<1e-5f && std::abs(night.ambient[1]-.13f)<1e-5f && std::abs(night.ambient[2]-.20f)<1e-5f,"night ambient unchanged");
  check(sunset.diffuse[0]>sunset.diffuse[2] && luma(sunset.diffuse)<luma(noon.diffuse),"warm fading sunset");
  for(float h:{0.f,5.f,6.f,7.f,12.f,17.8f,18.5f,23.f,36.f}) {
    auto light=at(h); bool finite=true;
    for(int i=0;i<3;++i) finite=finite&&std::isfinite(light.diffuse[i])&&light.diffuse[i]>=0;
    check(finite,"finite nonnegative cycle");
  }
  auto next=at(36);check(std::abs(luma(next.diffuse)-luma(noon.diffuse))<1e-5f,"24 hour repeat");
  slrr::scene_sun_set_time(43200.f);
  return failed?1:0;
}
