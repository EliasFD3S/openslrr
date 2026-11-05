#include "slrr/scene_sun.hpp"

#include <cmath>

namespace slrr {
namespace {
SceneSun g_sun;

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
  write_color(g_sun.diffuse, diffuse);
  write_color(g_sun.ambient, ambient);
  write_color(g_sun.specular, specular);
  g_sun.set = true;
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
}  // namespace slrr
