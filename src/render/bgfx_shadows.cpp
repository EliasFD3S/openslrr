// Cached map shadows, car shadow maps and the traffic kocsiarnyek decal.
#include "bgfx_lib_backend.hpp"
#include "slrr/frame_profiler.hpp"

#include "slrr/part_runtime.hpp"
#include "slrr/dds_image.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/ptx_image.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/video_fmv.hpp"

#include <bx/math.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <utility>

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
    // The ground is the receiver. Casting it paints its own texels back
    // onto the apron as a grid, and it is the heaviest mesh in the pass.
    if (!instance.geometry || instance.sky || instance.vehicle_material ||
        instance.gpu_skin || instance.light_flags || instance.geometry->ground_polygons)
      continue;
    const auto found = instance_meshes_.find(instance.geometry.get());
    if (found == instance_meshes_.end()) continue;
    const auto& part = found->second;
    if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh)) continue;
    for (const auto& group : part.geo.groups) {
      const auto flags = group.material_flags;
      const bool v4 = part.geo.info.version == 4;
      if ((v4 && (flags & 0x40000000u) && (group.material_type & 0x04u)) ||
          (part.geo.info.version == 3 && (flags & 0x101u) == 0x101u))
        continue;
      // Storefront mix uses the texture's alpha as a colour weight.
      const bool cutout = v4 ? group.material_type != 0x03u && (flags & 0x20000000u)
                             : (flags & 1u) != 0;
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

bool LibBgfxBackend::ensure_vehicle_shadow_target(int slot, uint16_t size) {
  if (bgfx::isValid(shadow_fb_[slot])) return true;
  constexpr uint64_t color_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP |
                                   BGFX_SAMPLER_V_CLAMP;
  const bgfx::TextureHandle color = bgfx::createTexture2D(
      size, size, false, 1, bgfx::TextureFormat::RGBA8, color_flags);
  const bgfx::TextureHandle depth = bgfx::createTexture2D(
      size, size, false, 1, bgfx::TextureFormat::D16, BGFX_TEXTURE_RT_WRITE_ONLY);
  if (bgfx::isValid(color) && bgfx::isValid(depth)) {
    const bgfx::TextureHandle attachments[] = {color, depth};
    shadow_fb_[slot] = bgfx::createFrameBuffer(2, attachments, true);
    if (bgfx::isValid(shadow_fb_[slot])) {
      shadow_tex_[slot] = color;
      return true;
    }
  }
  if (bgfx::isValid(color)) bgfx::destroy(color);
  if (bgfx::isValid(depth)) bgfx::destroy(depth);
  return false;
}

void LibBgfxBackend::draw_vehicle_shadows(uint16_t view_id, const float* eye,
                                         const VideoOptions& video)
{
    const auto size = static_cast<uint16_t>(std::max(64, video.shadow_size));
    if (size != vehicle_shadow_size_) {
      for (int i = 0; i < kShadowSlots; ++i) {
        if (bgfx::isValid(shadow_fb_[i])) bgfx::destroy(shadow_fb_[i]);
        shadow_fb_[i] = BGFX_INVALID_HANDLE;
        shadow_tex_[i] = BGFX_INVALID_HANDLE;
      }
      vehicle_shadow_size_ = size;
    }
    // One owner is one car. Traffic carries a kocsiarnyek quad and does not
    // cast. A body with render flags (flags & 9) == 8 is drawn white into
    // that car's shadow map, then the ground is multiplied by it.
    struct CarShadow {
      void* owner = nullptr;
      bool decal = false;
      SceneShadowDecal blob;
      const float* xform = nullptr;
      float script[16]{};
      std::vector<const SceneInstance*> casters;
    };
    std::vector<CarShadow> cars;
    std::unordered_map<void*, size_t> car_index;
    for (const auto& instance : instances_) {
      if (!instance.key || instance.sky || !instance.geometry) continue;
      uint32_t flags = 0;
      SceneShadowDecal decal;
      scene_shadow_info(static_cast<uint32_t>(instance.type_id), &flags, &decal);
      const bool casts = decal.present || (flags & 9u) == 8u;
      if (!casts) continue;
      float frame[16]{};
      void* owner = part_shadow_source(instance.key, frame);
      if (!owner) {
        // A dealer display still casts. Its render matrix is the pose.
        owner = instance.key;
        frame[0] = frame[5] = frame[10] = frame[15] = 1.f;
        frame[12] = instance.transform[12];
        frame[13] = instance.transform[13];
        frame[14] = -instance.transform[14];
      }
      if (eye && std::hypot(frame[12] - eye[0], frame[13] - eye[1], frame[14] - eye[2]) >
                     video.shadow_distance)
        continue;
      auto found = car_index.find(owner);
      if (found == car_index.end()) {
        found = car_index.emplace(owner, cars.size()).first;
        CarShadow car;
        car.owner = owner;
        std::memcpy(car.script, frame, sizeof(frame));
        cars.push_back(std::move(car));
      }
      CarShadow& car = cars[found->second];
      if (decal.present) {
        if (!car.decal) {
          car.decal = true;
          car.blob = std::move(decal);
          car.xform = instance.transform.data();
        }
      } else if ((flags & 9u) == 8u) {
        car.casters.push_back(&instance);
      }
    }

    struct Slot {
      bool used = false;
      float mtx[16]{};
      float view[16]{};
      float centre[3]{};
      float centre_y = 0.f;
      float ramp = 0.f;
      float radius = 0.f;
      float bottom = 0.f;
      float script_x = 0.f, script_y = 0.f, script_z = 0.f;
      void* owner = nullptr;
      bgfx::TextureHandle tex = BGFX_INVALID_HANDLE;
    };
    Slot slots[kShadowSlots];
    // The offscreen map never reached the courtyard. Flatten the body onto
    // the ground along the sun; the stencil keeps overlapping panels from
    // stacking the ink.
    SceneSun sun = scene_instances_light();
    float travel[3] = {sun.direction[0], sun.direction[1], -sun.direction[2]};
    {
      const float tlen = std::sqrt(travel[0] * travel[0] + travel[1] * travel[1] +
                                   travel[2] * travel[2]);
      if (tlen < 1e-5f) {
        travel[0] = 0.f;
        travel[1] = -1.f;
        travel[2] = 0.f;
      } else {
        travel[0] /= tlen;
        travel[1] /= tlen;
        travel[2] /= tlen;
      }
    }
    const uint64_t flat_state =
        BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL |
        BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                              BGFX_STATE_BLEND_INV_SRC_ALPHA);
    const uint32_t flat_stencil =
        BGFX_STENCIL_TEST_NOTEQUAL | BGFX_STENCIL_FUNC_REF(1) |
        BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
        BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_REPLACE;
    // Stock multiplies the ground by black inside the silhouette. The
    // fill is that ink, one write, so the middle matches the contour.
    const float flat_tint[4] = {0.f, 0.f, 0.f, 1.f};
    const float flat_alpha[4] = {0.f, 1.f, 0.f, 0.f};
    std::unordered_set<void*> active_shadows;
    const auto floor_revision = scene_instances_floor_revision();
    const std::array<float, 3> shadow_light = {travel[0], travel[1], travel[2]};
    // The silhouette sheet is a plane and reads as one. Stock inks the
    // ground from a shadow map instead; that path is below.
    if (false) {
      for (const CarShadow& car : cars) {
        if (car.decal || car.casters.empty()) continue;
        // Each vertex sits on the surface under it. A triangle that jumps
        // from the deck to the pit is split until the pieces lie on one
        // surface; the sliver that still bridges is dropped.
        struct Cell {
          bool hit = false;
          float h = 0, nx = 0, ny = 1, nz = 0;
        };
        std::unordered_map<uint64_t, Cell> cells;
        auto probe = [&](float sx, float sz, float below, Cell& cell) {
          // Five centimetres follows the lip of the pit. The shadow's
          // plan position is the mesh, so this key only caches height.
          const int ix = static_cast<int>(std::floor(sx * 20.f));
          const int iz = static_cast<int>(std::floor(sz * 20.f));
          const uint64_t key =
              (static_cast<uint64_t>(static_cast<uint32_t>(ix)) << 32) |
              static_cast<uint32_t>(iz);
          auto found = cells.find(key);
          if (found != cells.end()) {
            cell = found->second;
            return;
          }
          Cell stored;
          SceneFloorHit hit;
          if (scene_instances_sample_floor(sx, sz, below, car.owner, hit)) {
            stored.hit = true;
            stored.h = hit.height;
            stored.nx = hit.normal[0];
            stored.ny = hit.normal[1];
            stored.nz = hit.normal[2];
          }
          cells.emplace(key, stored);
          cell = stored;
        };
        struct Placed {
          bool ok = false;
          float x = 0, y = 0, z = 0, h = 0;
        };
        auto place = [&](float rx, float ry, float rz) {
          Placed out;
          float sx = rx, sz = rz;
          for (int iter = 0; iter < 2; ++iter) {
            Cell cell;
            // Chassis height stays under the ceiling and over the deck.
            // A wheel vertex below the deck must not cache the pit for the
            // whole column.
            probe(sx, -sz, car.script[13] + 0.45f, cell);
            if (!cell.hit) return out;
            const float ny = std::fabs(cell.ny) > 0.2f ? cell.ny : 1.f;
            if (std::fabs(travel[1]) < 0.05f || iter == 1) {
              out.ok = true;
              out.h = cell.h;
              // Stay under the vertex. A nudge along the floor normal
              // walks the shadow off the tyre and the sill.
              out.x = sx;
              out.y = cell.h + 0.004f;
              out.z = sz;
              (void)ny;
              return out;
            }
            const float t = (cell.h + 0.02f - ry) / travel[1];
            if (!(t > -0.02f)) return out;
            sx = rx + travel[0] * t;
            sz = rz + travel[2] * t;
          }
          return out;
        };
        // Already projected. Sampling again would walk the shadow sideways.
        auto drop = [&](float rx, float rz) {
          Placed out;
          Cell cell;
          probe(rx, -rz, car.script[13] + 0.45f, cell);
          if (!cell.hit) return out;
          const float ny = std::fabs(cell.ny) > 0.2f ? cell.ny : 1.f;
          out.ok = true;
          out.h = cell.h;
          out.x = rx;
          out.y = cell.h + 0.004f;
          out.z = rz;
          (void)ny;
          return out;
        };
        // A parked car still jitters by a millimetre. Rebuilding the
        // silhouette on that jitter was the frame hitch.
        auto quant = [](std::array<float, 16> m) {
          for (float& v : m) v = std::round(v * 100.f) * 0.01f;
          return m;
        };
        const float ceiling = std::round(car.script[13] * 100.f) * 0.01f;
        active_shadows.insert(car.owner);
        auto& cached = projected_shadows_[car.owner];
        bool dirty = cached.owner != car.owner || cached.light != shadow_light ||
                     cached.ceiling != ceiling ||
                     cached.floor_revision != floor_revision ||
                     cached.stamps.size() != car.casters.size();
        if (!dirty) {
          for (size_t i = 0; i < car.casters.size(); ++i) {
            const SceneInstance* inst = car.casters[i];
            const auto& stamp = cached.stamps[i];
            if (stamp.key != inst->key || stamp.source != inst->geometry ||
                stamp.transform != quant(inst->transform)) {
              dirty = true;
              break;
            }
          }
        }
        if (dirty) {
          cached.owner = car.owner;
          cached.light = shadow_light;
          cached.ceiling = ceiling;
          cached.floor_revision = floor_revision;
          cached.stamps.clear();
          cached.vertices.clear();
          cached.indices.clear();
          struct Tri {
            float x[3], z[3];
          };
          std::vector<Tri> tris;
          float minz = 0.f, maxz = 0.f;
          bool any = false;
          for (const SceneInstance* inst : car.casters) {
            ProjectedShadow::Stamp stamp;
            stamp.key = inst->key;
            stamp.source = inst->geometry;
            stamp.transform = quant(inst->transform);
            cached.stamps.push_back(std::move(stamp));
            if (!inst->geometry) continue;
            const auto& geo = *inst->geometry;
            const size_t n = geo.positions.size() / 3;
            if (n == 0 || geo.indices.size() < 3) continue;
            const float scale =
                geo.position_scale > 0.f ? geo.position_scale : 0.01f;
            const float* m = inst->transform.data();
            std::vector<Placed> placed(n);
            for (size_t i = 0; i < n; ++i) {
              const float lx = geo.positions[i * 3 + 0] * scale;
              const float ly = geo.positions[i * 3 + 1] * scale;
              const float lz = -geo.positions[i * 3 + 2] * scale;
              placed[i] = place(m[0] * lx + m[4] * ly + m[8] * lz + m[12],
                                m[1] * lx + m[5] * ly + m[9] * lz + m[13],
                                m[2] * lx + m[6] * ly + m[10] * lz + m[14]);
            }
            for (size_t t = 0; t + 2 < geo.indices.size(); t += 3) {
              const uint32_t ia = geo.indices[t];
              const uint32_t ib = geo.indices[t + 1];
              const uint32_t ic = geo.indices[t + 2];
              if (ia >= n || ib >= n || ic >= n) continue;
              if (!placed[ia].ok || !placed[ib].ok || !placed[ic].ok) continue;
              Tri tri{{placed[ia].x, placed[ib].x, placed[ic].x},
                      {placed[ia].z, placed[ib].z, placed[ic].z}};
              const float zlo = std::min(tri.z[0], std::min(tri.z[1], tri.z[2]));
              const float zhi = std::max(tri.z[0], std::max(tri.z[1], tri.z[2]));
              if (!any) {
                minz = zlo;
                maxz = zhi;
                any = true;
              } else {
                minz = std::min(minz, zlo);
                maxz = std::max(maxz, zhi);
              }
              tris.push_back(tri);
            }
          }
          // One band per centimetre. Its left and right edges are the
          // real contour, and everything between them is filled, so the
          // gaps between the sill and the wheels close.
          if (any && maxz - minz > 1e-3f && !tris.empty()) {
            constexpr float kWant = 0.004f;
            int nrows = static_cast<int>(std::ceil((maxz - minz) / kWant));
            if (nrows < 1) nrows = 1;
            if (nrows > 1400) nrows = 1400;
            const float row = (maxz - minz) / static_cast<float>(nrows);
            struct Span {
              float lo = 1e9f, hi = -1e9f;
            };
            std::vector<Span> spans(static_cast<size_t>(nrows));
            auto cover = [&](const Tri& tri, float z, Span& span) {
              float lo = 1e9f, hi = -1e9f;
              auto edge = [&](int a, int b) {
                const float az = tri.z[a], bz = tri.z[b];
                const float ax = tri.x[a], bx = tri.x[b];
                if (std::fabs(az - bz) < 1e-6f) {
                  if (std::fabs(az - z) > 1e-4f) return;
                  lo = std::min(lo, std::min(ax, bx));
                  hi = std::max(hi, std::max(ax, bx));
                  return;
                }
                const float t = (z - az) / (bz - az);
                if (t < -1e-4f || t > 1.f + 1e-4f) return;
                const float x = ax + t * (bx - ax);
                lo = std::min(lo, x);
                hi = std::max(hi, x);
              };
              edge(0, 1);
              edge(1, 2);
              edge(2, 0);
              if (hi < lo) return;
              span.lo = std::min(span.lo, lo);
              span.hi = std::max(span.hi, hi);
            };
            for (const Tri& tri : tris) {
              const float zlo = std::min(tri.z[0], std::min(tri.z[1], tri.z[2]));
              const float zhi = std::max(tri.z[0], std::max(tri.z[1], tri.z[2]));
              int r0 = static_cast<int>(std::floor((zlo - minz) / row));
              int r1 = static_cast<int>(std::floor((zhi - minz) / row));
              if (r0 < 0) r0 = 0;
              if (r1 >= nrows) r1 = nrows - 1;
              if (r0 > r1) continue;
              for (int r = r0; r <= r1; ++r) {
                const float za = minz + static_cast<float>(r) * row;
                const float zb = za + row;
                cover(tri, std::max(za, zlo), spans[static_cast<size_t>(r)]);
                cover(tri, std::min(zb, zhi), spans[static_cast<size_t>(r)]);
              }
            }
            auto emit_quad = [&](float x0, float x1, float z0, float z1, float h) {
              if (x1 < x0 + 1e-4f) return;
              const float y = h + 0.012f;
              const auto base = static_cast<uint32_t>(cached.vertices.size());
              auto push = [&](float x, float z) {
                MeshVertex vertex{};
                vertex.x = x;
                vertex.y = y;
                vertex.z = z;
                vertex.ny = 1.f;
                cached.vertices.push_back(vertex);
              };
              push(x0, z0);
              push(x1, z0);
              push(x1, z1);
              push(x0, z0);
              push(x1, z1);
              push(x0, z1);
              for (uint32_t i = 0; i < 6; ++i) cached.indices.push_back(base + i);
            };
            for (int r = 0; r < nrows; ++r) {
              const Span& span = spans[static_cast<size_t>(r)];
              if (span.hi < span.lo) continue;
              const float z0 = minz + static_cast<float>(r) * row;
              const float z1 = z0 + row;
              const float midz = 0.5f * (z0 + z1);
              struct Sample {
                float x, h;
                bool ok;
              };
              std::vector<Sample> samples;
              for (float x = span.lo; x < span.hi + 1e-4f; x += 0.05f) {
                const float sx = std::min(x, span.hi);
                const Placed p = drop(sx, midz);
                samples.push_back(Sample{sx, p.ok ? p.h : 0.f, p.ok});
              }
              if (samples.empty()) continue;
              // A crack in the lift mesh sees the pit and used to skip a
              // chunk of the silhouette. Compare against the original
              // samples: healing in place would climb out of the whole pit.
              const std::vector<Sample> raw = samples;
              for (size_t i = 0; i < samples.size(); ++i) {
                float deck = -1e9f;
                for (size_t j = 0; j < raw.size(); ++j) {
                  if (!raw[j].ok) continue;
                  if (std::fabs(raw[j].x - raw[i].x) > 0.2f) continue;
                  deck = std::max(deck, raw[j].h);
                }
                if (deck < -1e8f) continue;
                if (!samples[i].ok || samples[i].h < deck - 0.22f) {
                  samples[i].h = deck;
                  samples[i].ok = true;
                }
              }
              size_t i = 0;
              while (i < samples.size()) {
                if (!samples[i].ok) {
                  ++i;
                  continue;
                }
                size_t j = i;
                while (j + 1 < samples.size() && samples[j + 1].ok &&
                       std::fabs(samples[j + 1].h - samples[i].h) <= 0.22f)
                  ++j;
                emit_quad(samples[i].x, samples[j].x, z0 - row * 0.5f,
                          z1 + row * 0.5f, samples[i].h);
                i = j + 1;
              }
            }
          }
        }
        const auto& idx = cached.indices;
        if (idx.size() < 3 || cached.vertices.empty()) continue;
        const auto vn = static_cast<uint32_t>(cached.vertices.size());
        // One car's panels do not fit a single transient buffer. The
        // indices are local to each triangle, so the soup splits cleanly.
        constexpr uint32_t kBatch = 9000;
        float ident[16];
        bx::mtxIdentity(ident);
        for (uint32_t start = 0; start + 2 < vn; start += kBatch) {
          uint32_t count = std::min(kBatch, vn - start);
          count -= count % 3;
          if (count < 3) break;
          if (bgfx::getAvailTransientVertexBuffer(count, layout_) < count ||
              bgfx::getAvailTransientIndexBuffer(count, true) < count)
            break;
          bgfx::TransientVertexBuffer tvb;
          bgfx::TransientIndexBuffer tib;
          bgfx::allocTransientVertexBuffer(&tvb, count, layout_);
          bgfx::allocTransientIndexBuffer(&tib, count, true);
          std::memcpy(tvb.data, cached.vertices.data() + start,
                      count * sizeof(MeshVertex));
          auto* ids = reinterpret_cast<uint32_t*>(tib.data);
          for (uint32_t i = 0; i < count; ++i) ids[i] = i;
          bgfx::setTransform(ident);
          bgfx::setVertexBuffer(0, &tvb);
          bgfx::setIndexBuffer(&tib);
          bgfx::setTexture(0, s_tex_, white_tex_);
          bind_baked_light(nullptr, nullptr);
          bgfx::setUniform(u_color_, flat_tint);
          bgfx::setUniform(u_alpha_test_, flat_alpha);
          bgfx::setStencil(flat_stencil, flat_stencil);
          bgfx::setState(flat_state);
          // Pull the ink toward the camera so the coplanar tread cannot
          // win the depth test in spots.
          bgfx::setDepthControl(-8, -2.f);
          bgfx::submit(view_id, program_);
        }
      }
      bgfx::setDepthControl(0, 0.f);
    }
    for (auto it = projected_shadows_.begin(); it != projected_shadows_.end();) {
      if (!active_shadows.count(it->first)) it = projected_shadows_.erase(it);
      else ++it;
    }
    const bool maps = true;
    if (maps && bgfx::isValid(shadow_program_) && bgfx::isValid(shadow_recv_)) {
      SceneSun sun = scene_instances_light();
      float travel[3] = {sun.direction[0], sun.direction[1], -sun.direction[2]};
      const float tlen = std::sqrt(travel[0] * travel[0] + travel[1] * travel[1] +
                                   travel[2] * travel[2]);
      if (tlen < 1e-5f) {
        travel[0] = 0.f;
        travel[1] = -1.f;
        travel[2] = 0.f;
      } else {
        travel[0] /= tlen;
        travel[1] /= tlen;
        travel[2] /= tlen;
      }
      auto light_view = [](const float d[3], const float centre[3], float* view) {
        float rx, ry = 0.f, rz;
        if (std::fabs(d[0]) < 1e-5f && std::fabs(d[2]) < 1e-5f) {
          rx = 1.f;
          rz = 0.f;
        } else {
          rx = -d[2];
          rz = d[0];
          const float rl = std::sqrt(rx * rx + rz * rz);
          rx /= rl;
          rz /= rl;
        }
        float ux = ry * d[2] - rz * d[1];
        float uy = rz * d[0] - rx * d[2];
        float uz = rx * d[1] - ry * d[0];
        const float ul = std::sqrt(ux * ux + uy * uy + uz * uz);
        if (ul > 1e-8f) {
          ux /= ul;
          uy /= ul;
          uz /= ul;
        }
        std::fill(view, view + 16, 0.f);
        view[0] = rx;
        view[1] = ux;
        view[2] = d[0];
        view[4] = ry;
        view[5] = uy;
        view[6] = d[1];
        view[8] = rz;
        view[9] = uz;
        view[10] = d[2];
        view[12] = -(rx * centre[0] + ry * centre[1] + rz * centre[2]);
        view[13] = -(ux * centre[0] + uy * centre[1] + uz * centre[2]);
        view[14] = -(d[0] * centre[0] + d[1] * centre[1] + d[2] * centre[2]);
        view[15] = 1.f;
      };
      const bgfx::Caps* caps = bgfx::getCaps();
      const float sy = caps->originBottomLeft ? 0.5f : -0.5f;
      int slot_n = 0;
      const uint64_t caster_state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
      for (const CarShadow& car : cars) {
        if (car.decal || car.casters.empty() || slot_n >= kShadowSlots) continue;
        if (!ensure_vehicle_shadow_target(slot_n, size)) break;
        float mn[3] = {1e30f, 1e30f, 1e30f};
        float mx[3] = {-1e30f, -1e30f, -1e30f};
        bool any = false;
        for (const SceneInstance* inst : car.casters) {
          const float scale = inst->geometry->position_scale > 0.f
                                  ? inst->geometry->position_scale
                                  : 0.01f;
          const float* m = inst->transform.data();
          for (int ix = 0; ix < 2; ++ix)
            for (int iy = 0; iy < 2; ++iy)
              for (int iz = 0; iz < 2; ++iz) {
                const float lx =
                    (ix ? inst->geometry->info.bbox_max[0]
                       : inst->geometry->info.bbox_min[0]) *
                    scale;
                const float ly =
                    (iy ? inst->geometry->info.bbox_max[1]
                       : inst->geometry->info.bbox_min[1]) *
                    scale;
                const float lz =
                    -((iz ? inst->geometry->info.bbox_max[2]
                         : inst->geometry->info.bbox_min[2]) *
                      scale);
                const float x = m[0] * lx + m[4] * ly + m[8] * lz + m[12];
                const float y = m[1] * lx + m[5] * ly + m[9] * lz + m[13];
                const float z = m[2] * lx + m[6] * ly + m[10] * lz + m[14];
                mn[0] = std::min(mn[0], x);
                mn[1] = std::min(mn[1], y);
                mn[2] = std::min(mn[2], z);
                mx[0] = std::max(mx[0], x);
                mx[1] = std::max(mx[1], y);
                mx[2] = std::max(mx[2], z);
                any = true;
              }
        }
        if (!any) continue;
        const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
        float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        if (radius < 1e-4f) continue;
        if (radius < 0.5f) radius = 0.5f;
        // The sun is not straight down. The contact patch sits outside the
        // body box; a tighter map clips it.
        radius *= 1.35f;
        const float centre[3] = {(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f,
                                 (mn[2] + mx[2]) * 0.5f};
        float view[16], proj[16];
        light_view(travel, centre, view);
        bx::mtxOrtho(proj, -radius, radius, -radius, radius, -30.f, 30.f, 0.f,
                     caps->homogeneousDepth);
        const auto shadow_view = static_cast<uint16_t>(kVehicleShadowFirst + slot_n);
        bgfx::setViewFrameBuffer(shadow_view, shadow_fb_[slot_n]);
        bgfx::setViewRect(shadow_view, 0, 0, size, size);
        bgfx::setViewClear(shadow_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                           0x000000ff, 1.f, 0);
        bgfx::setViewMode(shadow_view, bgfx::ViewMode::Sequential);
        bgfx::touch(shadow_view);
        bgfx::setViewTransform(shadow_view, view, proj);
        for (const SceneInstance* inst : car.casters) {
          auto mesh = instance_meshes_.find(inst->geometry.get());
          if (mesh == instance_meshes_.end()) continue;
          const ScenePart& part = mesh->second;
          if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh)) continue;
          bgfx::setTransform(inst->transform.data());
          bgfx::setVertexBuffer(0, part.vbh);
          bgfx::setIndexBuffer(part.ibh);
          bgfx::setState(caster_state);
          bgfx::submit(shadow_view, shadow_program_);
        }
        const float crop[16] = {
            0.5f, 0.f, 0.f, 0.f, 0.f, sy, 0.f, 0.f, 0.f, 0.f,
            caps->homogeneousDepth ? 0.5f : 1.f, 0.f, 0.5f, 0.5f,
            caps->homogeneousDepth ? 0.5f : 0.f, 1.f};
        float pv[16], tex[16];
        bx::mtxMul(pv, view, proj);
        bx::mtxMul(tex, pv, crop);
        Slot& slot = slots[slot_n];
        slot.used = true;
        std::memcpy(slot.mtx, tex, sizeof(tex));
        std::memcpy(slot.view, view, sizeof(view));
        std::memcpy(slot.centre, centre, sizeof(centre));
        slot.centre_y = centre[1];
        slot.ramp = 0.25f / radius;
        slot.radius = radius;
        slot.bottom = mn[1];
        slot.script_x = car.script[12];
        slot.script_y = car.script[13];
        slot.script_z = car.script[14];
        slot.owner = car.owner;
        slot.tex = shadow_tex_[slot_n];
        ++slot_n;
      }
      const uint64_t recv_state =
          BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL |
          BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_INV_SRC_COLOR) |
          BGFX_STATE_MSAA;
      auto receive = [&](bgfx::VertexBufferHandle vb, bgfx::IndexBufferHandle ib,
                         const float* matrix) {
        if (!bgfx::isValid(vb) || !bgfx::isValid(ib)) return;
        for (const Slot& slot : slots) {
          if (!slot.used || !bgfx::isValid(slot.tex)) continue;
          bgfx::setUniform(u_shadow_mtx_, slot.mtx);
          const float diffuse = (sun.diffuse[0] + sun.diffuse[1] + sun.diffuse[2]) / 3.f;
          const float ambient = (sun.ambient[0] + sun.ambient[1] + sun.ambient[2]) / 3.f;
          const float strength = outdoor_lighting_
              ? std::clamp(diffuse / std::max(diffuse + ambient, .01f), 0.f, .85f) : 1.f;
          const float ramp[4] = {1.f / size, slot.centre_y, strength, slot.ramp};
          bgfx::setUniform(u_shadow_ramp_, ramp);
          bgfx::setTransform(matrix);
          bgfx::setVertexBuffer(0, vb);
          bgfx::setIndexBuffer(ib);
          bgfx::setTexture(0, s_tex_, slot.tex,
                           BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
          bgfx::setState(recv_state);
          // The floor is already in the depth buffer. A small pull toward
          // the camera lets this multiply pass land on it. The car is
          // nearer, so it still covers the ink.
          bgfx::setDepthControl(-2, -1.f);
          bgfx::submit(view_id, shadow_recv_);
        }
      };
      float identity[16];
      bx::mtxIdentity(identity);
      if(garage_)
        for (const ScenePart& part : parts_) receive(part.vbh, part.ibh, identity);
      for (const auto& instance : instances_) {
        if (instance.sky || instance.vehicle_material || instance.gpu_skin ||
            !instance.geometry)
          continue;
        auto mesh = instance_meshes_.find(instance.geometry.get());
        if (mesh == instance_meshes_.end()) continue;
        receive(mesh->second.vbh, mesh->second.ibh, instance.transform.data());
      }
      bgfx::setDepthControl(0, 0.f);
    }

    const uint64_t blob_state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA |
                                BGFX_STATE_MSAA;
    const float blob_tint[4] = {1.f, 1.f, 1.f, 1.f};
    const float blob_alpha[4] = {0.f, 1.f, 1.f, 0.f};
    for (const CarShadow& car : cars) {
      if (!car.decal || !car.xform) continue;
      bgfx::TextureHandle tex = tex_for_osd_icon(car.blob.texture);
      if (!bgfx::isValid(tex)) continue;
      SceneFloorHit hit;
      const bool on_floor = scene_instances_sample_floor(
          car.script[12], car.script[14], car.script[13] + 0.5f, car.owner, hit);
      const float* m = car.xform;
      const float* c = car.blob.center;
      const float* u = car.blob.axis_u;
      const float* v = car.blob.axis_v;
      MeshVertex verts[4]{};
      const float su[4] = {-1.f, 1.f, 1.f, -1.f};
      const float sv[4] = {-1.f, -1.f, 1.f, 1.f};
      const float uu[4] = {car.blob.uv[0], car.blob.uv[2], car.blob.uv[2],
                           car.blob.uv[0]};
      const float vv[4] = {car.blob.uv[1], car.blob.uv[1], car.blob.uv[3],
                           car.blob.uv[3]};
      for (int i = 0; i < 4; ++i) {
        const float lx = c[0] + su[i] * u[0] + sv[i] * v[0];
        const float ly = c[1] + su[i] * u[1] + sv[i] * v[1];
        const float lz = -(c[2] + su[i] * u[2] + sv[i] * v[2]);
        verts[i].x = m[0] * lx + m[4] * ly + m[8] * lz + m[12];
        verts[i].y = on_floor ? hit.height + 0.03f : m[13] + 0.03f;
        verts[i].z = m[2] * lx + m[6] * ly + m[10] * lz + m[14];
        verts[i].ny = 1.f;
        verts[i].u = uu[i];
        verts[i].v = vv[i];
      }
      const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
      if (bgfx::getAvailTransientVertexBuffer(4, layout_) < 4 ||
          bgfx::getAvailTransientIndexBuffer(6) < 6)
        continue;
      bgfx::TransientVertexBuffer tvb;
      bgfx::TransientIndexBuffer tib;
      bgfx::allocTransientVertexBuffer(&tvb, 4, layout_);
      bgfx::allocTransientIndexBuffer(&tib, 6);
      std::memcpy(tvb.data, verts, sizeof(verts));
      std::memcpy(tib.data, idx, sizeof(idx));
      float identity[16];
      bx::mtxIdentity(identity);
      bgfx::setTransform(identity);
      bgfx::setVertexBuffer(0, &tvb);
      bgfx::setIndexBuffer(&tib);
      bgfx::setTexture(0, s_tex_, tex);
      bind_baked_light(nullptr, nullptr);
      bgfx::setUniform(u_color_, blob_tint);
      bgfx::setUniform(u_alpha_test_, blob_alpha);
      bgfx::setState(blob_state);
      bgfx::submit(view_id, program_);
    }
  }

}  // namespace slrr
