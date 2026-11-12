#include "slrr/scene_sun.hpp"

#include <algorithm>
#include <cmath>

namespace slrr {
namespace {
SceneSun g_sun;
float g_sun_time = 12.f * 3600.f;

float channel(int32_t packed, int shift) {
  return static_cast<float>((packed >> shift) & 255) / 256.f;
}

void write_color(float* dst, int32_t packed) {
  dst[0] = channel(packed, 16);
  dst[1] = channel(packed, 8);
  dst[2] = channel(packed, 0);
}
}  // namespace

void scene_sun_set_packed(int32_t diffuse, int32_t ambient, int32_t specular) {
  scene_sun_apply_packed(g_sun,diffuse,ambient,specular);
}
void scene_sun_apply_packed(SceneSun& light,int32_t diffuse,int32_t ambient,int32_t specular) {
  write_color(light.diffuse, diffuse);
  write_color(light.ambient, ambient);
  write_color(light.specular, specular);
  light.set = true;
}

void scene_sun_set_resource(const float diffuse[3], const float ambient[3],
                            const float specular[3], const float direction[3]) {
  for (int i = 0; i < 3; ++i) {
    g_sun.diffuse[i] = diffuse[i];
    g_sun.ambient[i] = ambient[i];
    g_sun.specular[i] = specular[i];
    g_sun.direction[i] = direction ? direction[i] : 0.f;
  }
  const float len = direction ? std::sqrt(direction[0] * direction[0] +
                                          direction[1] * direction[1] +
                                          direction[2] * direction[2])
                              : 0.f;
  g_sun.has_direction = len > 1e-4f;
  g_sun.set = true;
}

SceneSun scene_sun() { return g_sun; }

void scene_sun_set_time(float seconds) {
  if (std::isfinite(seconds)) g_sun_time = seconds;
}

float scene_sun_time() { return g_sun_time; }

void scene_sun_apply_day_cycle(SceneSun& light) {
  constexpr float tau = 6.28318530718f;
  const float phase = std::fmod(g_sun_time, 86400.f) * (tau / 86400.f) - tau * .25f;
  const float elevation = std::sin(phase);
  light.direction[0] = -std::cos(phase);
  light.direction[1] = -.85f * elevation;
  light.direction[2] = .5268f * elevation;
  light.has_direction = true;
  light.set = true;
  // The old ramp (* 5) was full day until the sun was almost down, then black.
  // This one stays lit through the orange band and is out once it has set.
  const float daylight = std::clamp((elevation + 0.06f) / 0.28f, 0.f, 1.f);
  const float twilight = std::clamp((elevation - 0.02f) / 0.22f, 0.f, 1.f);
  constexpr float night[] = {.10f, .13f, .20f};
  constexpr float warm[] = {1.f, .66f, .44f};
  // The city light is a tint that used to sit on a bright lightmap. Without
  // that bake, a dim diffuse leaves noon looking grey. Lift it toward a
  // full sun, and let the daylight ramp bring dusk back down.
  const float base_luma = light.diffuse[0] * 0.2126f + light.diffuse[1] * 0.7152f +
                          light.diffuse[2] * 0.0722f;
  const float gain = base_luma > 1e-4f ? std::min(1.15f / base_luma, 2.8f) : 1.f;
  for (int i = 0; i < 3; ++i) {
    const float tint = warm[i] + (1.f - warm[i]) * twilight;
    light.diffuse[i] *= daylight * std::max(gain, 1.f) * tint;
    light.specular[i] *= daylight * std::max(gain, 1.f);
    light.ambient[i] = night[i] + (std::max(light.ambient[i], night[i]) - night[i]) * twilight;
  }
}
}  // namespace slrr
