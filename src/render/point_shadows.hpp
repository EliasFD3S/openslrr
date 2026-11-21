#pragma once
#include <array>
#include <bx/math.h>
#include <cmath>
#include <cstdint>
namespace slrr {
inline constexpr uint16_t point_shadow_tile = 512;
inline constexpr uint16_t point_shadow_width = point_shadow_tile * 3;
inline constexpr uint16_t point_shadow_height = point_shadow_tile * 2;
// View 64 is reserved and ordered before the main world, below OSD/reflections.
inline constexpr std::array<uint16_t, 6> point_shadow_views{0, 1, 2, 3, 4, 64};
inline unsigned point_shadow_face(const std::array<float, 3> &d) {
  const float x = std::abs(d[0]), y = std::abs(d[1]), z = std::abs(d[2]);
  return x >= y && x >= z ? (d[0] >= 0 ? 0 : 1)
         : y >= z         ? (d[1] >= 0 ? 2 : 3)
                          : (d[2] >= 0 ? 4 : 5);
}
struct PointShadowProjection {
  std::array<float, 16> view{}, projection{}, sample{};
};
inline PointShadowProjection point_shadow_projection(unsigned face, float range,
                                                     bool homogeneous,
                                                     bool bottomLeft) {
  constexpr bx::Vec3 axes[] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                               {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  const bx::Vec3 up = face == 2   ? bx::Vec3{0, 0, -1}
                      : face == 3 ? bx::Vec3{0, 0, 1}
                                  : bx::Vec3{0, 1, 0};
  PointShadowProjection result;
  bx::mtxLookAt(result.view.data(), {0, 0, 0}, axes[face], up);
  // Small overlap keeps PCF within the selected face at the 90-degree seams.
  bx::mtxProj(result.projection.data(), 92.f, 1.f, .05f, range, homogeneous);
  float clip[16];
  bx::mtxMul(clip, result.view.data(), result.projection.data());
  const float crop[16] = {.5f,
                          0,
                          0,
                          0,
                          0,
                          bottomLeft ? .5f : -.5f,
                          0,
                          0,
                          0,
                          0,
                          homogeneous ? .5f : 1.f,
                          0,
                          .5f,
                          .5f,
                          homogeneous ? .5f : 0.f,
                          1};
  bx::mtxMul(result.sample.data(), clip, crop);
  return result;
}
} // namespace slrr
