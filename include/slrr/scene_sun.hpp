#pragma once

#include <cstdint>

namespace slrr {

struct SceneFlare {
  uint32_t texture = 0, color = 0xffffffff;
  float min_size = 0.f, max_size = 0.f;
  int flares = 0, rays = 0;
};

// The directional light of the current scene. The garage does not spawn the
// skydome sun (internalScene): it instances misc.garage:0x001D, which points
// straight down. Outdoors, maps.skydome:0x0124 supplies the direction and
// setLight replaces the colours for the time of day.
struct SceneSun {
  float diffuse[3] = {1.f, 1.f, 1.f};
  float ambient[3] = {0.45f, 0.45f, 0.45f};
  float specular[3] = {1.f, 1.f, 1.f};
  // Game space, the direction the light travels (D3D). (0, -1, 0) is the
  // garage neon.
  float direction[3] = {0.f, -1.f, 0.f};
  bool set = false;
  bool has_direction = false;
  SceneFlare flare;
};

void scene_sun_set_packed(int32_t diffuse, int32_t ambient, int32_t specular);
void scene_sun_apply_packed(SceneSun& light,int32_t diffuse,int32_t ambient,int32_t specular);
void scene_sun_set_resource(const float diffuse[3], const float ambient[3],
                            const float specular[3], const float direction[3]);
SceneSun scene_sun();
void scene_sun_set_time(float seconds);
void scene_sun_apply_day_cycle(SceneSun& light);

}  // namespace slrr
