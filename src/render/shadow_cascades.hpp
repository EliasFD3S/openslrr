#pragma once
#include "render_debug.hpp"
#include "slrr/scene_instances.hpp"
#include <algorithm>
#include <array>
#include <bx/math.h>
#include <cmath>
#include <cstdint>
namespace slrr {
// Camera-centred distance cascades: useful also for cubemap views around the
// car.
inline constexpr std::array<float, 4> shadow_radii{32.f, 100.f, 320.f, 1000.f};
// The radii in use: shadow_radii times the debug cascade scale (#243).
inline std::array<float, 4> cascade_radii() {
  const float scale = std::clamp(render_debug().cascade_scale, .25f, 4.f);
  return {shadow_radii[0] * scale, shadow_radii[1] * scale, shadow_radii[2] * scale,
          shadow_radii[3] * scale};
}
inline constexpr std::array<uint16_t, 4> shadow_views{0, 2, 3, 4};
// World-space box of a mesh under an instance transform (same convention as
// shadow_bounds_visible). False when the bounds are unusable.
inline bool shadow_bounds_box(const ScxGeometry &mesh,
                              const std::array<float, 16> &m, float lower[3],
                              float upper[3]) {
  if (!std::isfinite(mesh.position_scale) || mesh.position_scale <= 0)
    return false;
  float center[3]{}, extent[3]{};
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(mesh.info.bbox_min[i]) ||
        !std::isfinite(mesh.info.bbox_max[i]) ||
        mesh.info.bbox_max[i] < mesh.info.bbox_min[i])
      return false;
    const float c = (mesh.info.bbox_min[i] + mesh.info.bbox_max[i]) * .5f *
                    mesh.position_scale * (i == 2 ? -1.f : 1.f);
    const float e = (mesh.info.bbox_max[i] - mesh.info.bbox_min[i]) * .5f *
                    mesh.position_scale;
    for (int row = 0; row < 3; ++row) {
      center[row] += m[i * 4 + row] * c;
      extent[row] += std::abs(m[i * 4 + row]) * e;
    }
  }
  for (int i = 0; i < 3; ++i) {
    lower[i] = center[i] + m[12 + i] - extent[i];
    upper[i] = center[i] + m[12 + i] + extent[i];
  }
  return true;
}

inline bool shadow_bounds_visible(const ScxGeometry &mesh,
                                  const std::array<float, 16> &m,
                                  const SceneFrustum &frustum) {
  float center[3]{}, extent[3]{};
  if (!std::isfinite(mesh.position_scale) || mesh.position_scale <= 0)
    return true;
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(mesh.info.bbox_min[i]) ||
        !std::isfinite(mesh.info.bbox_max[i]) ||
        mesh.info.bbox_max[i] < mesh.info.bbox_min[i])
      return true;
  for (int i = 0; i < 3; ++i) {
    const float c = (mesh.info.bbox_min[i] + mesh.info.bbox_max[i]) * .5f *
                    mesh.position_scale * (i == 2 ? -1.f : 1.f);
    const float e = (mesh.info.bbox_max[i] - mesh.info.bbox_min[i]) * .5f *
                    mesh.position_scale;
    for (int row = 0; row < 3; ++row) {
      center[row] += m[i * 4 + row] * c;
      extent[row] += std::abs(m[i * 4 + row]) * e;
    }
  }
  for (int i = 0; i < 3; ++i)
    center[i] += m[12 + i];
  for (const auto &p : frustum)
    if (p[0] * center[0] + p[1] * center[1] + p[2] * center[2] + p[3] <
        -(std::abs(p[0]) * extent[0] + std::abs(p[1]) * extent[1] +
          std::abs(p[2]) * extent[2]))
      return false;
  return true;
}
inline void shadow_rebase_origin(std::array<float,3>& origin,const float focus[3],const float direction[3], uint16_t resolution = 2048) {
  const float delta[3]={focus[0]-origin[0],focus[1]-origin[1],focus[2]-origin[2]};
  if(std::hypot(delta[0],delta[1],delta[2])<256.f) return;
  const auto travel=bx::normalize(bx::Vec3{direction[0],direction[1],direction[2]});
  const bx::Vec3 up=std::abs(travel.y)>.98f?bx::Vec3{0,0,1}:bx::Vec3{0,1,0};
  float basis[16];bx::mtxLookAt(basis,bx::mul(travel,-1.f),{0,0,0},up);
  float local[3]{};
  // Common multiple of every cascade texel size: do not change lattice phase
  // when rebasing after a long drive. LCM(32,100,320,1000) is 8000.
  const float common_step=2.2f*8000.f/resolution;
  for(int row=0;row<3;++row) {
    for(int axis=0;axis<3;++axis) local[row]+=delta[axis]*basis[axis*4+row];
    if(row<2) local[row]=std::round(local[row]/common_step)*common_step;
  }
  for(int axis=0;axis<3;++axis) for(int row=0;row<3;++row)
    origin[axis]+=basis[axis*4+row]*local[row];
}
struct ShadowProjection {
  std::array<float, 16> view{}, projection{}, sample{};
};
inline ShadowProjection shadow_projection(const float eye[3],
                                          const float direction[3],
                                          float radius, uint16_t resolution,
                                          bool homogeneousDepth,
                                          bool originBottomLeft) {
  ShadowProjection result;
  const auto travel =
      bx::normalize(bx::Vec3{direction[0], direction[1], direction[2]});
  const bx::Vec3 up =
      std::abs(travel.y) > .98f ? bx::Vec3{0, 0, 1} : bx::Vec3{0, 1, 0};
  const float extent = radius * 1.1f, distance = extent + 768.f;
  bx::mtxLookAt(result.view.data(), bx::mul(travel, -distance), {0, 0, 0}, up);
  auto &view = result.view;
  const float texel = 2.f * extent / resolution;
  // Snap translation in a fixed light basis, not in the camera basis.
  const float x = eye[0] * view[0] + eye[1] * view[4] + eye[2] * view[8];
  const float y = eye[0] * view[1] + eye[1] * view[5] + eye[2] * view[9];
  const float z = eye[0] * view[2] + eye[1] * view[6] + eye[2] * view[10];
  view[12] -= std::round(x / texel) * texel;
  view[13] -= std::round(y / texel) * texel;
  view[14] -= z;
  bx::mtxOrtho(result.projection.data(), -extent, extent, -extent, extent, 0.f,
               2.f * distance, 0.f, homogeneousDepth);
  float clip[16];
  bx::mtxMul(clip, view.data(), result.projection.data());
  const float crop[16] = {.5f,
                          0,
                          0,
                          0,
                          0,
                          originBottomLeft ? .5f : -.5f,
                          0,
                          0,
                          0,
                          0,
                          homogeneousDepth ? .5f : 1.f,
                          0,
                          .5f,
                          .5f,
                          homogeneousDepth ? .5f : 0.f,
                          1};
  bx::mtxMul(result.sample.data(), clip, crop);
  return result;
}
} // namespace slrr
