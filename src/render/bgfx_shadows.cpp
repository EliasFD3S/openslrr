// Active cached map/garage shadows and vehicle depth maps.
#include "bgfx_lib_backend.hpp"
#include "slrr/frame_profiler.hpp"

#include <bx/math.h>
#include <algorithm>
#include <cmath>

namespace slrr {

void LibBgfxBackend::update_static_shadow(const float* eye, const VideoOptions& video) {
  perf::Scope perf_scope(perf::Zone::Shadows);
  static_shadow_active_ = false;
  // The garage has no sun map. The neon reuses this same view and looks down
  // on the bay, so the car and the room cast one shadow without a new pass.
  if (garage_ && local_light_[3] > 0.f && video.shadows != 0 &&
      debug_overlay_.render_options().map_shadows &&
      bgfx::isValid(static_shadow_program_)) {
    constexpr uint16_t size = kShadowResolution;
    if (!bgfx::isValid(static_shadow_fb_) || size != static_shadow_size_) {
      if (bgfx::isValid(static_shadow_fb_)) bgfx::destroy(static_shadow_fb_);
      static_shadow_fb_ = BGFX_INVALID_HANDLE;
      static_shadow_tex_ = BGFX_INVALID_HANDLE;
      constexpr uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP |
          BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
      const auto depth = bgfx::createTexture2D(size, size, false, 1,
                                              bgfx::TextureFormat::D32F, flags);
      if (!bgfx::isValid(depth)) return;
      static_shadow_fb_ = bgfx::createFrameBuffer(1, &depth, true);
      if (!bgfx::isValid(static_shadow_fb_)) {
        bgfx::destroy(depth);
        return;
      }
      static_shadow_tex_ = depth;
      static_shadow_size_ = size;
    }
    const auto* caps = bgfx::getCaps();
    const bx::Vec3 light{local_light_[0], local_light_[1], local_light_[2]};
    float view[16], proj[16], clip[16];
    bx::mtxLookAt(view, light, bx::Vec3{light.x, light.y - 1.f, light.z}, {0.f, 0.f, 1.f});
    constexpr float extent = 10.f;
    bx::mtxOrtho(proj, -extent, extent, -extent, extent, 0.15f, 16.f, 0.f,
                 caps->homogeneousDepth);
    bx::mtxMul(clip, view, proj);
    const float crop[16] = {
        .5f, 0.f, 0.f, 0.f, 0.f, caps->originBottomLeft ? .5f : -.5f, 0.f, 0.f,
        0.f, 0.f, caps->homogeneousDepth ? .5f : 1.f, 0.f, .5f, .5f,
        caps->homogeneousDepth ? .5f : 0.f, 1.f};
    bx::mtxMul(static_shadow_matrix_.data(), clip, crop);
    bgfx::setViewFrameBuffer(kStaticShadowView, static_shadow_fb_);
    bgfx::setViewRect(kStaticShadowView, 0, 0, size, size);
    bgfx::setViewClear(kStaticShadowView, BGFX_CLEAR_DEPTH, 0, 1.f, 0);
    bgfx::setViewTransform(kStaticShadowView, view, proj);
    bgfx::touch(kStaticShadowView);
    const uint64_t state = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
    auto cast = [&](bgfx::VertexBufferHandle vb, bgfx::IndexBufferHandle ib,
                    const auto& groups, const float* matrix) {
      if (!bgfx::isValid(vb) || !bgfx::isValid(ib)) return;
      auto submit = [&](uint32_t start, uint32_t count) {
        const float opaque[4] = {0.f, -1.f, 0.f, 0.f};
        bgfx::setUniform(u_static_shadow_params_, opaque);
        bgfx::setTexture(0, s_tex_, white_tex_);
        bgfx::setTransform(matrix);
        bgfx::setVertexBuffer(0, vb);
        bgfx::setIndexBuffer(ib, start, count);
        bgfx::setState(state);
        bgfx::submit(kStaticShadowView, static_shadow_program_);
      };
      if (groups.empty()) return;
      for (const auto& group : groups) {
        if ((group.material_flags & 0x40000000u) || group.diffuse[3] < .99f) continue;
        submit(group.index_start, group.index_count);
      }
    };
    // The room stays a receiver. Casting the shell from a light under the
    // ceiling would put the whole floor in shadow.
    for (const auto& instance : instances_) {
      if (!instance.vehicle_material || instance.light_flags || !instance.geometry) continue;
      const auto found = instance_meshes_.find(instance.geometry.get());
      if (found == instance_meshes_.end()) continue;
      cast(found->second.vbh, found->second.ibh, found->second.geo.groups,
           instance.transform.data());
    }
    static_shadow_eye_ = {light.x, light.y, light.z};
    static_shadow_distance_ = 40.f;
    static_shadow_world_ = nullptr;
    static_shadow_active_ = true;
    return;
  }
  if (!eye || video.shadows == 0 || !scene_instances_has_ground_props()) {
    static_shadow_world_ = nullptr;
    if (bgfx::isValid(static_shadow_fb_)) bgfx::destroy(static_shadow_fb_);
    static_shadow_fb_ = BGFX_INVALID_HANDLE;
    static_shadow_tex_ = BGFX_INVALID_HANDLE;
    return;
  }
  if (!debug_overlay_.render_options().map_shadows || !bgfx::isValid(static_shadow_program_))
    return;
  const auto ground = std::find_if(instances_.begin(), instances_.end(),
      [](const SceneInstance& instance) { return instance.geometry->ground_polygons; });
  if (ground == instances_.end()) return;
  const auto sun = ground->lighting;
  if (!sun.has_direction || sun.direction[1] >= 0.02f) return;
  constexpr uint16_t size = kShadowResolution;
  // Static buildings shade streets well beyond the range of vehicle shadows.
  const bool conservative =
      (bgfx::getCaps()->supported & BGFX_CAPS_CONSERVATIVE_RASTER) != 0;
  // Close to the car, a 150 m map turns each texel into a stripe on the
  // ground. Keep the resolution on the street around the camera.
  const float range = std::min(conservative ? 56.f : 36.f, video.view_distance);
  const float step = std::max(8.f, range * .15f);
  const float half_width = range + step;
  static_shadow_eye_ = {eye[0], eye[1], -eye[2]};
  const std::array<float, 3> focus{eye[0], eye[1], eye[2]};
  const float moved = std::hypot(eye[0] - static_shadow_focus_[0],
                                 eye[1] - static_shadow_focus_[1],
                                 eye[2] - static_shadow_focus_[2]);
  const std::array<float, 3> direction{sun.direction[0], sun.direction[1], -sun.direction[2]};
  const float sun_change = std::hypot(direction[0] - static_shadow_sun_[0],
                                      direction[1] - static_shadow_sun_[1],
                                      direction[2] - static_shadow_sun_[2]);
  if (size != static_shadow_size_ && bgfx::isValid(static_shadow_fb_)) {
    bgfx::destroy(static_shadow_fb_);
    static_shadow_fb_ = BGFX_INVALID_HANDLE;
    static_shadow_tex_ = BGFX_INVALID_HANDLE;
  }
  if (range == static_shadow_distance_ && static_shadow_world_ == ground->key && moved < step &&
      sun_change < .003f && bgfx::isValid(static_shadow_fb_)) {
    static_shadow_active_ = true;
    return;
  }
  if (!bgfx::isValid(static_shadow_fb_)) {
    constexpr uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP |
        BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
    const auto depth = bgfx::createTexture2D(size, size, false, 1,
                                            bgfx::TextureFormat::D32F, flags);
    if (!bgfx::isValid(depth)) return;
    static_shadow_fb_ = bgfx::createFrameBuffer(1, &depth, true);
    if (!bgfx::isValid(static_shadow_fb_)) {
      bgfx::destroy(depth);
      return;
    }
    static_shadow_tex_ = depth;
    static_shadow_size_ = size;
  }
  const auto* caps = bgfx::getCaps();
  const bx::Vec3 target{focus[0], focus[1], -focus[2]};
  const bx::Vec3 travel = bx::normalize(bx::Vec3{direction[0], direction[1], direction[2]});
  const bx::Vec3 up = std::abs(travel.y) > .98f ? bx::Vec3{0.f, 0.f, 1.f}
                                              : bx::Vec3{0.f, 1.f, 0.f};
  float view[16], proj[16], clip[16];
  bx::mtxLookAt(view, bx::sub(target, bx::mul(travel, 768.f)), target, up);
  bx::mtxOrtho(proj, -half_width, half_width, -half_width, half_width, 0.f, 1024.f,
              0.f, caps->homogeneousDepth);
  const auto frustum = view_frustum(view, proj);
  bx::mtxMul(clip, view, proj);
  const float crop[16] = {
      .5f, 0.f, 0.f, 0.f, 0.f, caps->originBottomLeft ? .5f : -.5f, 0.f, 0.f,
      0.f, 0.f, caps->homogeneousDepth ? .5f : 1.f, 0.f, .5f, .5f,
      caps->homogeneousDepth ? .5f : 0.f, 1.f};
  bx::mtxMul(static_shadow_matrix_.data(), clip, crop);

  bgfx::setViewFrameBuffer(kStaticShadowView, static_shadow_fb_);
  bgfx::setViewRect(kStaticShadowView, 0, 0, size, size);
  bgfx::setViewClear(kStaticShadowView, BGFX_CLEAR_DEPTH, 0, 1.f, 0);
  bgfx::setViewTransform(kStaticShadowView, view, proj);
  bgfx::touch(kStaticShadowView);
  for (const auto& instance : scene_instances_collect(&frustum)) {
    // Cars have their own depth map. Skinned pedestrians move, so they would
    // freeze into this cached map. Everything else on the map casts.
    // Ground POLY also contains buildings, stairs and track structures.
    // It must cast as well as receive; receiver-plane bias handles self-shadow.
    if (!instance.geometry || instance.sky || instance.vehicle_material ||
        instance.gpu_skin || instance.light_flags || instance.emissive_overlay)
      continue;
    const auto found = instance_meshes_.find(instance.geometry.get());
    if (found == instance_meshes_.end()) continue;
    const auto& part = found->second;
    if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh)) continue;
    for (const auto& group : part.geo.groups) {
      const bool v4 = part.geo.info.version == 4;
      const auto surface = scx_surface(part.geo.info.version, group);
      if (surface.blend) continue;
      const bool cutout = surface.cutout;
      const auto slot = group.texture_index;
      const auto tex = slot < part.tex.size() && bgfx::isValid(part.tex[slot])
          ? part.tex[slot] : white_tex_;
      const float threshold = !cutout ? -1.f : v4
          ? group.alpha_reference / 255.f : .5f;
      const float params[4] = {0.f, threshold, static_cast<float>(group.base_map.channel), 0.f};
      bgfx::setUniform(u_static_shadow_params_, params);
      bgfx::setTexture(0, s_tex_, tex);
      bgfx::setTransform(instance.transform.data());
      bgfx::setVertexBuffer(0, part.vbh);
      bgfx::setIndexBuffer(part.ibh, group.index_start, group.index_count);
      // Preserve narrow casters that overlap a texel without covering its centre.
      const uint64_t coverage = conservative ? BGFX_STATE_CONSERVATIVE_RASTER : 0;
      bgfx::setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | coverage);
      bgfx::submit(kStaticShadowView, static_shadow_program_);
    }
  }
  std::vector<SceneBatch> shadow_batches;
  scene_instances_collect_batches(&frustum, shadow_batches);
  draw_prop_batches(kStaticShadowView, shadow_batches, true, conservative);
  static_shadow_distance_ = range;
  static_shadow_world_ = ground->key;
  static_shadow_focus_ = focus;
  static_shadow_sun_ = direction;
  static_shadow_active_ = true;
}

void LibBgfxBackend::update_car_depth(const float* position, const VideoOptions& video) {
  perf::Scope perf_scope(perf::Zone::Shadows);
  car_depth_active_ = false;
  if (!position || video.shadows == 0 || !debug_overlay_.render_options().shadows ||
      !bgfx::isValid(static_shadow_program_)) return;

  constexpr uint16_t size = kShadowResolution;
  if (!bgfx::isValid(car_depth_fb_)) {
    const auto texture = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::D32F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
        BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
    if (!bgfx::isValid(texture)) return;
    car_depth_fb_ = bgfx::createFrameBuffer(1, &texture, true);
    if (!bgfx::isValid(car_depth_fb_)) {
      bgfx::destroy(texture);
      return;
    }
    car_depth_tex_ = texture;
  }

  const auto sun = scene_instances_light();
  const bx::Vec3 direction = sun.has_direction
      ? bx::Vec3{sun.direction[0], sun.direction[1], -sun.direction[2]}
      : bx::Vec3{0.f, -1.f, 0.f};
  const auto travel = bx::normalize(direction);
  if (travel.y >= 0.02f) return;
  const bx::Vec3 centre{position[0], position[1], -position[2]};
  const bx::Vec3 up = std::abs(travel.y) > .98f ? bx::Vec3{0,0,1} : bx::Vec3{0,1,0};
  const auto* caps = bgfx::getCaps();
  float view[16], proj[16], clip[16];
  bx::mtxLookAt(view, bx::sub(centre, bx::mul(travel, 20.f)), centre, up);
  bx::mtxOrtho(proj, -14.f, 14.f, -14.f, 14.f, 0.f, 48.f, 0.f, caps->homogeneousDepth);
  bx::mtxMul(clip, view, proj);
  const float crop[16] = {
      .5f, 0.f, 0.f, 0.f, 0.f, caps->originBottomLeft ? .5f : -.5f, 0.f, 0.f,
      0.f, 0.f, caps->homogeneousDepth ? .5f : 1.f, 0.f, .5f, .5f,
      caps->homogeneousDepth ? .5f : 0.f, 1.f};
  bx::mtxMul(car_depth_matrix_.data(), clip, crop);
  constexpr uint16_t id = kVehicleShadowFirst;
  bgfx::setViewFrameBuffer(id, car_depth_fb_);
  bgfx::setViewRect(id, 0, 0, size, size);
  bgfx::setViewClear(id, BGFX_CLEAR_DEPTH, 0, 1.f, 0);
  bgfx::setViewTransform(id, view, proj);
  bgfx::touch(id);

  for (const auto& instance : instances_) {
    if (!instance.vehicle_material || instance.light_flags) continue;
    const auto found = instance_meshes_.find(instance.geometry.get());
    if (found == instance_meshes_.end()) continue;
    const auto& part = found->second;
    if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh)) continue;
    for (const auto& group : part.geo.groups) {
      if ((group.material_flags & 0x40000000u) || group.diffuse[3] < .99f) continue;
      const float opaque[4] = {0.f, -1.f, 0.f, 0.f};
      bgfx::setUniform(u_static_shadow_params_, opaque);
      bgfx::setTexture(0, s_tex_, white_tex_);
      bgfx::setTransform(instance.transform.data());
      bgfx::setVertexBuffer(0, part.vbh);
      bgfx::setIndexBuffer(part.ibh, group.index_start, group.index_count);
      bgfx::setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
      bgfx::submit(id, static_shadow_program_);
    }
  }
  car_depth_active_ = true;
}

}  // namespace slrr
