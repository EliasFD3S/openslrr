// Active cached map/garage shadows and vehicle depth maps.
#include "bgfx_lib_backend.hpp"
#include "point_shadows.hpp"
#include "shadow_quality.hpp"
#include "shadow_cascades.hpp"
#include "slrr/frame_profiler.hpp"
#include "slrr/particle_cloud.hpp"

#include <bx/math.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "slrr/part_runtime.hpp"

namespace slrr {
namespace {
// Characters cast into the sun cascades up to this distance from the camera.
// Past it a pedestrian covers about one texel of the 320 m cascade.
constexpr float kSkinnedShadowRange = 100.f;
uint32_t skin_palette_hash(const std::vector<float> &palette) {
  uint32_t hash = 2166136261u;
  for (const float value : palette) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    hash = (hash ^ bits) * 16777619u;
  }
  return hash;
}
} // namespace

void LibBgfxBackend::update_fixture_shadows(const VideoOptions &video) {
  ++light_uniform_revision_;
  bool was_active = fixture_shadow_active_;
  fixture_shadow_active_ = false;
  if (fixture_lights_.empty() || !garage_ || video.shadows == 0 ||
      !render_debug().fixture_shadows ||
      !debug_overlay_.render_options().map_shadows) {
    fixture_shadow_casters_.clear();
    return;
  }
  const uint16_t tile=local_shadow_resolution(),width=tile*4,height=tile*3;
  if(bgfx::isValid(fixture_shadow_fb_) && fixture_shadow_tile_!=tile) {
    bgfx::destroy(fixture_shadow_fb_);fixture_shadow_fb_=BGFX_INVALID_HANDLE;
    fixture_shadow_tex_=BGFX_INVALID_HANDLE;was_active=false;
  }
  fixture_shadow_tile_=tile;
  if (!bgfx::isValid(fixture_shadow_fb_)) {
    const auto depth = bgfx::createTexture2D(
        width, height, false, 1, bgfx::TextureFormat::D32F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(depth))
      return;
    fixture_shadow_fb_ = bgfx::createFrameBuffer(1, &depth, true);
    if (!bgfx::isValid(fixture_shadow_fb_)) {
      bgfx::destroy(depth);
      return;
    }
    fixture_shadow_tex_ = depth;
  }
  std::array<std::array<float, 16>, kFixtureLights> views{}, projections{};
  fixture_shadow_matrices_ = {};
  const auto *caps = bgfx::getCaps();
  const float crop[16] = {.5f,
                          0,
                          0,
                          0,
                          0,
                          caps->originBottomLeft ? .5f : -.5f,
                          0,
                          0,
                          0,
                          0,
                          caps->homogeneousDepth ? .5f : 1.f,
                          0,
                          .5f,
                          .5f,
                          caps->homogeneousDepth ? .5f : 0.f,
                          1};
  for (size_t i = 0; i < fixture_lights_.size(); ++i) {
    const auto &light = fixture_lights_[i];
    const bx::Vec3 position{light.position[0], light.position[1],
                            light.position[2]};
    const bx::Vec3 direction{light.direction[0], light.direction[1],
                             light.direction[2]};
    const bx::Vec3 up =
        std::abs(direction.y) > .98f ? bx::Vec3{0, 0, 1} : bx::Vec3{0, 1, 0};
    bx::mtxLookAt(views[i].data(), position, bx::add(position, direction), up);
    bx::mtxProj(projections[i].data(),
                2.f * std::acos(light.outer_cos) * 57.2957795f, 1.f, .05f,
                light.range, caps->homogeneousDepth);
    float clip[16];
    bx::mtxMul(clip, views[i].data(), projections[i].data());
    bx::mtxMul(fixture_shadow_matrices_[i].data(), clip, crop);
  }
  std::vector<ShadowCasterStamp> stamps;
  for (const auto &instance : instances_) {
    if (!instance.geometry || instance.sky || instance.light_flags ||
        instance.emissive_overlay || instance.gpu_skin ||
        (instance.vehicle_material && !debug_overlay_.render_options().shadows))
      continue;
    cache_scene_mesh(instance.geometry, instance.key, false);
    const auto &part = instance_meshes_.at(instance.geometry.get());
    stamps.push_back({instance.geometry.get(), instance.geometry->revision,
                      part.vbh.idx, part.ibh.idx, instance.transform});
  }
  fixture_shadow_active_ = true;
  if (was_active && render_debug().shadow_caching &&
      fixture_cached_matrices_ == fixture_shadow_matrices_ &&
      stamps == fixture_shadow_casters_)
    return;
  const float no_apertures[4]{};
  enc()->setUniform(u_aperture_params_, no_apertures);
  for (size_t i = 0; i < fixture_lights_.size(); ++i) {
    const uint16_t view = uint16_t(128 + i);
    bgfx::setViewFrameBuffer(view, fixture_shadow_fb_);
    bgfx::setViewRect(view, uint16_t(i % 4 * tile), uint16_t(i / 4 * tile),
                      tile, tile);
    bgfx::setViewClear(view, BGFX_CLEAR_DEPTH, 0, 1.f);
    bgfx::setViewTransform(view, views[i].data(), projections[i].data());
    enc()->touch(view);
    const auto frustum = view_frustum(views[i].data(), projections[i].data());
    for (const auto &stamp : stamps) {
      const auto &part = instance_meshes_.at(stamp.source);
      if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh) ||
          !shadow_bounds_visible(*stamp.source, stamp.transform, frustum))
        continue;
      for (const auto &group : part.shadow_groups) {
        const auto surface = scx_surface(part.geo.info.version, group);
        const float threshold = !surface.cutout ? -1.f
                                : part.geo.info.version == 4
                                    ? group.alpha_reference / 255.f
                                    : .5f;
        const float params[4] = {0, threshold, float(group.base_map.channel),
                                 0};
        const auto slot = group.texture_index;
        set_uniform(u_static_shadow_params_, params);
        enc()->setTexture(0, s_tex_,
                         slot < part.tex.size() && bgfx::isValid(part.tex[slot])
                             ? part.tex[slot]
                             : white_tex_);
        enc()->setVertexBuffer(0, part.vbh);
        enc()->setIndexBuffer(part.ibh, group.index_start, group.index_count);
        enc()->setTransform(stamp.transform.data());
        enc()->setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
        submit_counted(view, static_shadow_program_);
      }
    }
  }
  fixture_shadow_casters_ = std::move(stamps);
  fixture_cached_matrices_ = fixture_shadow_matrices_;
  ++fixture_shadow_revision_;
}

// Street lamp shadows (#237). Outside garages the fixture atlas is free: the
// nearest kLampShadows scenery lamps each get one tile with a downward
// 150-degree perspective projection. A lamp hangs above the street and lights
// mostly downward, so one projection covers the ground out to about 3.7 times
// its height (29 m for the 7.8 m lamp_base, where its light is about a tenth
// of the peak) for one depth pass, instead of the five or six passes of a
// cube. The shader fades the shadow out at the tile rim.
void LibBgfxBackend::update_lamp_shadows(const VideoOptions &video) {
  ++light_uniform_revision_;
  lamp_shadow_atlas_bound_ = false;
  lamp_shadow_count_ = 0;
  const auto free_slots = [&] {
    for (auto &slot : lamp_shadow_slots_) slot = {};
    car_shadow_tiles_.clear();
    car_shadow_stamps_.clear();
    car_shadow_valid_ = false;
  };
  // No tiles while the lamps are faint or the sun still casts shadows
  // (#271): a lamp shadow beside a sun shadow reads as a doubled shadow.
  const auto sun = scene_instances_sun_state();
  const float night = sun.night;
  if (garage_ || night < kLampShadowNightMin || sun.shadowing || !scenery_lights_count() ||
      video.shadows == 0 ||
      !render_debug().lamp_shadows || !render_debug().lamp_lighting ||
      !host_enhanced_graphics().street_lamps ||
      !debug_overlay_.render_options().map_shadows ||
      !host_enhanced_graphics().local_lights ||
      !bgfx::isValid(static_shadow_program_)) {
    free_slots();
    lamp_shadow_clock_ = -1.0;
    return;
  }
  perf::Scope perf_scope(perf::Zone::Shadows);
  perf::Scope detail_scope(perf::Zone::LampShadows);
  // Lamps added here at full power fade with the darkness too (#271).
  const uint16_t tile = local_shadow_resolution();
  if (bgfx::isValid(fixture_shadow_fb_) && fixture_shadow_tile_ != tile) {
    bgfx::destroy(fixture_shadow_fb_);
    fixture_shadow_fb_ = BGFX_INVALID_HANDLE;
    fixture_shadow_tex_ = BGFX_INVALID_HANDLE;
    free_slots();
  }
  fixture_shadow_tile_ = tile;
  if (!bgfx::isValid(fixture_shadow_fb_)) {
    const auto depth = bgfx::createTexture2D(
        uint16_t(tile * 4), uint16_t(tile * 3), false, 1, bgfx::TextureFormat::D32F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(depth)) return;
    fixture_shadow_fb_ = bgfx::createFrameBuffer(1, &depth, true);
    if (!bgfx::isValid(fixture_shadow_fb_)) {
      bgfx::destroy(depth);
      return;
    }
    fixture_shadow_tex_ = depth;
    free_slots();
  }
  // The garage fixtures may have drawn into the atlas since the last frame.
  if (fixture_shadow_active_) free_slots();
  // Fade step: real time, so the fade does not depend on the frame rate.
  float dt = lamp_shadow_dt_override_;
  if (dt < 0.f) {
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    dt = lamp_shadow_clock_ < 0.0 ? 0.f
                                  : float(std::clamp(now - lamp_shadow_clock_, 0.0, .1));
    lamp_shadow_clock_ = now;
  }
  const float step = dt / kLampShadowFade;
  const auto find_lamp = [&](const void *source) {
    for (size_t i = 0; i < scenery_lamps_.size(); ++i)
      if (scenery_lamps_[i].source == source) return int(i);
    return -1;
  };
  // Candidates: lamps whose light reaches the focus (the followed car, else
  // the camera), nearest first. Not the camera frustum and not the camera
  // direction: a car's shadows must stay put while the camera turns.
  const std::array<float, 3> focus =
      lamp_focus_valid_ ? std::array<float, 3>{lamp_focus_[0], lamp_focus_[1], -lamp_focus_[2]}
                        : std::array<float, 3>{cull_eye_[0], cull_eye_[1], -cull_eye_[2]};
  const auto candidates = scenery_lights_near(focus, kLampShadowReach, kLightGroupCity,
      [](void *parent, float *matrix) { return scene_instances_render_matrix(parent, matrix); });
  const auto find_candidate = [&](const void *source) {
    for (size_t i = 0; i < candidates.size(); ++i)
      if (candidates[i].source == source) return int(i);
    return -1;
  };
  // Debug budget (#243): fewer shadowed lamps; extra held slots fade out.
  // The saved option (#377) sets the default; the debug budget can lower it.
  const int saved_lamps = std::clamp(host_enhanced_graphics().shadowed_lamps, 0, int(kLampShadows));
  const size_t shadow_budget = size_t(std::min(saved_lamps,
      render_debug_budget(render_debug().shadowed_lamps, int(kLampShadows), int(kLampShadows))));
  // 1. Held lamps keep their slot while their light still reaches the focus.
  for (auto &slot : lamp_shadow_slots_) {
    if (!slot.source) continue;
    slot.hold += dt;
    const int index = find_candidate(slot.source);
    if (index < 0 || shadow_budget == 0) slot.wanted = false;
    else slot.distance = candidates[size_t(index)].distance;
  }
  // A smaller (debug) budget releases the farthest held lamps.
  for (;;) {
    LampShadowSlot *farthest = nullptr;
    size_t wanted = 0;
    for (auto &slot : lamp_shadow_slots_)
      if (slot.source && slot.wanted) {
        ++wanted;
        if (!farthest || slot.distance > farthest->distance) farthest = &slot;
      }
    if (wanted <= shadow_budget || !farthest) break;
    farthest->wanted = false;
  }
  // 2. Priority with hysteresis: replace the farthest held lamp only by a
  //    clearly nearer one, after a minimum hold, one swap per frame.
  {
    LampShadowSlot *worst = nullptr;
    for (auto &slot : lamp_shadow_slots_)
      if (slot.source && slot.wanted && (!worst || slot.distance > worst->distance)) worst = &slot;
    const SceneryLightPick *best = nullptr;
    for (const auto &candidate : candidates)
      if (std::none_of(lamp_shadow_slots_.begin(), lamp_shadow_slots_.end(),
                       [&](const LampShadowSlot &slot) { return slot.source == candidate.source; })) {
        best = &candidate;
        break;
      }
    const size_t wanted = size_t(std::count_if(lamp_shadow_slots_.begin(), lamp_shadow_slots_.end(),
        [](const LampShadowSlot &slot) { return slot.source && slot.wanted; }));
    if (worst && best && wanted >= shadow_budget && worst->hold >= kLampShadowHold &&
        best->distance < kLampShadowSwitchRatio * worst->distance &&
        worst->distance - best->distance > kLampShadowSwitchMargin)
      worst->wanted = false;
  }
  // 3. Fade: wanted lamps toward full strength, released ones toward zero.
  for (auto &slot : lamp_shadow_slots_) {
    if (!slot.source) continue;
    slot.weight = std::clamp(slot.weight + (slot.wanted ? step : -step), 0.f, 1.f);
    if (!slot.wanted && slot.weight <= 0.f) slot = {};
  }
  const auto *caps = bgfx::getCaps();
  const float crop[16] = {.5f, 0, 0, 0,
                          0, caps->originBottomLeft ? .5f : -.5f, 0, 0,
                          0, 0, caps->homogeneousDepth ? .5f : 1.f, 0,
                          .5f, .5f, caps->homogeneousDepth ? .5f : 0.f, 1};
  const auto projection_of = [&](const std::array<float, 3> &p, float range,
                                 float *view, float *proj) {
    const bx::Vec3 position{p[0], p[1], p[2]};
    bx::mtxLookAt(view, position, bx::add(position, bx::Vec3{0, -1, 0}),
                  bx::Vec3{0, 0, 1});
    bx::mtxProj(proj, kLampShadowFov, 1.f, kLampShadowNear, range,
                caps->homogeneousDepth);
  };
  // 4. Fill: the nearest unassigned candidates while fewer than kLampShadows
  //    lamps are wanted and a slot is free.
  for (const auto &candidate : candidates) {
    const size_t wanted = size_t(std::count_if(lamp_shadow_slots_.begin(), lamp_shadow_slots_.end(),
        [](const LampShadowSlot &slot) { return slot.source && slot.wanted; }));
    if (wanted >= shadow_budget) break;
    const bool assigned = std::any_of(lamp_shadow_slots_.begin(), lamp_shadow_slots_.end(),
        [&](const LampShadowSlot &slot) { return slot.source == candidate.source; });
    if (assigned || !candidate.source) continue;
    LampLight lamp;
    lamp.position = candidate.position;
    lamp.color = candidate.color;
    lamp.attenuation = candidate.attenuation;
    lamp.intensity = candidate.intensity * night;
    lamp.range = candidate.range;
    lamp.source = candidate.source;
    const auto free = std::find_if(lamp_shadow_slots_.begin(), lamp_shadow_slots_.end(),
        [](const LampShadowSlot &slot) { return !slot.source; });
    // Every slot busy: the wanted lamp waits until a fading slot frees.
    if (free == lamp_shadow_slots_.end()) break;
    LampShadowSlot slot;
    slot.source = lamp.source;
    slot.position = lamp.position;
    slot.range = lamp.range;
    slot.wanted = true;
    slot.weight = std::min(step, 1.f); // fades in from zero: no pop
    slot.distance = candidate.distance;
    slot.lamp = lamp;
    // Static scenery inside this lamp's projection, collected once. The
    // ground POLY (city-wide, also buildings) does not cast: it would cost
    // its whole mesh per lamp and its streets only receive. Meshes whose box
    // holds the lamp are its own housing and pole, which would black it out.
    float view[16], proj[16];
    projection_of(slot.position, slot.range, view, proj);
    const auto frustum = view_frustum(view, proj);
    const auto holds_lamp = [&](const ScxGeometry &geo, const std::array<float, 16> &m) {
      float lower[3], upper[3];
      if (!shadow_bounds_box(geo, m, lower, upper)) return false;
      for (int axis = 0; axis < 3; ++axis)
        if (slot.position[axis] < lower[axis] - .5f || slot.position[axis] > upper[axis] + .5f)
          return false;
      return true;
    };
    for (auto &instance : scene_instances_collect(&frustum, nullptr, false)) {
      if (!instance.geometry || instance.sky || instance.vehicle_material ||
          instance.gpu_skin || instance.light_flags || instance.emissive_overlay ||
          instance.geometry->ground_polygons || instance.opacity < .99f ||
          holds_lamp(*instance.geometry, instance.transform) ||
          !shadow_bounds_visible(*instance.geometry, instance.transform, frustum))
        continue;
      cache_scene_mesh(instance.geometry, nullptr, false);
      slot.statics.push_back(std::move(instance));
    }
    std::vector<SceneBatch> batches;
    scene_instances_collect_batches(&frustum, batches);
    for (auto &batch : batches) {
      if (!batch.geometry) continue;
      SceneBatch kept;
      kept.geometry = batch.geometry;
      kept.lighting = batch.lighting;
      for (const auto &instance : batch.instances)
        if (!holds_lamp(*batch.geometry, instance.transform) &&
            shadow_bounds_visible(*batch.geometry, instance.transform, frustum))
          kept.instances.push_back(instance);
      if (kept.instances.empty()) continue;
      cache_scene_mesh(kept.geometry, nullptr, true);
      slot.static_batches.push_back(std::move(kept));
    }
    *free = std::move(slot);
    ++lamp_shadow_assignments_;
  }
  const bool conservative = (caps->supported & BGFX_CAPS_CONSERVATIVE_RASTER) != 0;
  const auto submit_groups = [&](uint16_t view, const ScenePart &part,
                                 const std::array<float, 16> &transform,
                                 const SceneInstance *skinned) {
    for (const auto &group : part.shadow_groups) {
      const auto surface = scx_surface(part.geo.info.version, group);
      if (surface.blend) continue;
      const float threshold = !surface.cutout ? -1.f
                              : part.geo.info.version == 4 ? group.alpha_reference / 255.f
                                                           : .5f;
      const float params[4] = {0, threshold, float(group.base_map.channel), 0};
      set_uniform(u_static_shadow_params_, params);
      const auto slot = group.texture_index;
      enc()->setTexture(0, s_tex_, slot < part.tex.size() && bgfx::isValid(part.tex[slot])
                                      ? part.tex[slot] : white_tex_);
      enc()->setTransform(transform.data());
      if (skinned) {
        enc()->setUniform(u_skin_bones_, skinned->skin.data(), 48);
        enc()->setVertexBuffer(0, part.skin_vbh);
      } else {
        enc()->setVertexBuffer(0, part.vbh);
      }
      enc()->setIndexBuffer(part.ibh, group.index_start, group.index_count);
      enc()->setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                     (conservative ? BGFX_STATE_CONSERVATIVE_RASTER : 0));
      submit_counted(view, skinned ? ao_skin_depth_program_ : static_shadow_program_);
    }
  };
  const float no_apertures[4]{};
  enc()->setUniform(u_aperture_params_, no_apertures);
  // Car tiles: every lamp whose light reaches the cover sphere around the
  // followed car, nearest first, up to kCarShadowTiles. Strength fades over
  // kCarShadowFade metres before the reach (or the first lamp past the
  // budget), so a lamp gains or loses its car shadow where its light on the
  // car is near zero.
  const size_t car_budget = size_t(render_debug_budget(render_debug().car_tiles,
      int(kCarShadowTiles), int(kCarShadowTiles)));
  const bool car_shadows = lamp_focus_valid_ && debug_overlay_.render_options().shadows &&
                           render_debug().car_lamp_tiles && car_budget > 0;
  const std::array<float, 3> car_center{focus[0], focus[1] + 1.f, focus[2]};
  const auto covered_by_car_tiles = [&](const SceneInstance &instance) {
    if (!car_shadows) return false;
    float lower[3], upper[3];
    if (!shadow_bounds_box(*instance.geometry, instance.transform, lower, upper)) return false;
    float d2 = 0.f;
    for (int axis = 0; axis < 3; ++axis) {
      const float c = .5f * (lower[axis] + upper[axis]) - car_center[axis];
      d2 += c * c;
    }
    return d2 < kCarShadowCover * kCarShadowCover;
  };
  car_shadow_tiles_.clear();
  if (car_shadows) {
    std::vector<const SceneryLightPick *> reach;
    for (const auto &candidate : candidates) {
      const float dx = candidate.position[0] - car_center[0];
      const float dy = candidate.position[1] - car_center[1];
      const float dz = candidate.position[2] - car_center[2];
      const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (d < candidate.range + kCarShadowCover) reach.push_back(&candidate);
    }
    float cut = kSceneryLightMaxRange + kCarShadowCover;
    if (reach.size() > car_budget) {
      cut = reach[car_budget]->distance;
      reach.resize(car_budget);
    }
    for (const auto *candidate : reach) {
      const float t = std::clamp((std::min(cut, candidate->range + kCarShadowCover) -
                                  candidate->distance) / kCarShadowFade, 0.f, 1.f);
      CarShadowTile tile;
      tile.source = candidate->source;
      tile.strength = t * t * (3.f - 2.f * t);
      if (tile.strength <= 0.f) continue;
      const bx::Vec3 eye{candidate->position[0], candidate->position[1], candidate->position[2]};
      const bx::Vec3 at{car_center[0], car_center[1], car_center[2]};
      const auto delta = bx::sub(at, eye);
      const float d = bx::length(delta);
      const auto dir = bx::normalize(delta);
      const bx::Vec3 up = std::abs(dir.y) > .98f ? bx::Vec3{0, 0, 1} : bx::Vec3{0, 1, 0};
      bx::mtxLookAt(tile.view.data(), eye, at, up);
      const float half = d > kCarShadowCover ? std::asin(kCarShadowCover / d) : 1.48f;
      bx::mtxProj(tile.proj.data(), std::min(2.f * half * 57.2957795f, 170.f), 1.f,
                  std::max(.1f, d - kCarShadowCover), candidate->range, caps->homogeneousDepth);
      float clip[16];
      bx::mtxMul(clip, tile.view.data(), tile.proj.data());
      bx::mtxMul(tile.sample.data(), clip, crop);
      tile.lamp.position = candidate->position;
      tile.lamp.color = candidate->color;
      tile.lamp.attenuation = candidate->attenuation;
      tile.lamp.intensity = candidate->intensity * night;
      tile.lamp.range = candidate->range;
      tile.lamp.source = candidate->source;
      car_shadow_tiles_.push_back(tile);
    }
  }
  if (!car_shadow_tiles_.empty()) {
    // Casters: the vehicles in the cover sphere, all tiles together.
    std::vector<const SceneInstance *> cars;
    std::vector<ShadowCasterStamp> stamps;
    for (const auto &instance : instances_) {
      if (!instance.geometry || !instance.vehicle_material || instance.light_flags ||
          instance.opacity < .99f || !covered_by_car_tiles(instance))
        continue;
      const auto found = instance_meshes_.find(instance.geometry.get());
      if (found == instance_meshes_.end() || !bgfx::isValid(found->second.vbh) ||
          !bgfx::isValid(found->second.ibh))
        continue;
      cars.push_back(&instance);
      stamps.push_back({instance.geometry.get(), instance.geometry->revision,
                        found->second.vbh.idx, found->second.ibh.idx, instance.transform});
    }
    // No vehicle in the sphere (car hidden, cockpit-only): no car tiles.
    if (cars.empty()) car_shadow_tiles_.clear();
    // The tile matrices are part of the cache key (lamp set and car pose).
    for (const auto &tile : car_shadow_tiles_)
      stamps.push_back({nullptr, 0, 0, 0, tile.sample});
    if (!car_shadow_tiles_.empty() && (!car_shadow_valid_ || stamps != car_shadow_stamps_ ||
                                       !render_debug().shadow_caching)) {
      const uint16_t half = uint16_t(tile / 2);
      bgfx::setViewFrameBuffer(kCarShadowView, fixture_shadow_fb_);
      bgfx::setViewRect(kCarShadowView, 0, uint16_t(2 * tile), uint16_t(4 * tile), tile);
      bgfx::setViewClear(kCarShadowView, BGFX_CLEAR_DEPTH, 0, 1.f);
      float identity[16];
      bx::mtxIdentity(identity);
      bgfx::setViewTransform(kCarShadowView, identity, identity);
      // The tile draws are one job (#270): it may run on a worker encoder.
      const bool bottom_left = caps->originBottomLeft;
      queue_or_run([this, cars, tile, half, conservative, bottom_left] {
      enc()->touch(kCarShadowView);
      for (size_t k = 0; k < car_shadow_tiles_.size(); ++k) {
        const auto &tile_info = car_shadow_tiles_[k];
        const int column = int(k % 8), row = int(k / 8);
        // Clip remap into this quarter of the view rect, plus a scissor.
        float remap[16];
        bx::mtxIdentity(remap);
        remap[0] = .125f;
        remap[12] = -1.f + .125f + .25f * float(column);
        remap[5] = .5f;
        remap[13] = bottom_left ? -.5f + float(row) : .5f - float(row);
        float view_proj[16], to_tile[16];
        bx::mtxMul(view_proj, tile_info.view.data(), tile_info.proj.data());
        bx::mtxMul(to_tile, view_proj, remap);
        for (const auto *instance : cars) {
          float model[16];
          bx::mtxMul(model, instance->transform.data(), to_tile);
          const auto &part = instance_meshes_.at(instance->geometry.get());
          for (const auto &group : part.shadow_groups) {
            const auto surface = scx_surface(part.geo.info.version, group);
            if (surface.blend) continue;
            const float params[4] = {0, -1.f, 0, 0};
            set_uniform(u_static_shadow_params_, params);
            enc()->setTexture(0, s_tex_, white_tex_);
            enc()->setTransform(model);
            enc()->setVertexBuffer(0, part.vbh);
            enc()->setIndexBuffer(part.ibh, group.index_start, group.index_count);
            enc()->setScissor(uint16_t(column * half), uint16_t(2 * tile + row * half), half, half);
            enc()->setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                           (conservative ? BGFX_STATE_CONSERVATIVE_RASTER : 0));
            submit_counted(kCarShadowView, static_shadow_program_);
          }
        }
      }
      });
      car_shadow_stamps_ = std::move(stamps);
      car_shadow_valid_ = true;
      ++car_shadow_redraws_;
    }
  } else {
    car_shadow_valid_ = false;
    car_shadow_stamps_.clear();
  }
  std::vector<size_t> shadowed; // slots
  for (size_t s = 0; s < kLampShadowSlots; ++s) {
    auto &slot = lamp_shadow_slots_[s];
    if (!slot.source) continue;
    float view[16], proj[16], clip[16];
    projection_of(slot.position, slot.range, view, proj);
    bx::mtxMul(clip, view, proj);
    bx::mtxMul(slot.sample.data(), clip, crop);
    const auto frustum = view_frustum(view, proj);
    // Dynamic casters from the main view: vehicles (the player car too) and
    // characters with complete palettes. Their stamps decide the redraw.
    std::vector<ShadowCasterStamp> stamps;
    std::vector<const SceneInstance *> dynamic;
    for (const auto &instance : instances_) {
      if (!instance.geometry || instance.sky || instance.light_flags ||
          instance.emissive_overlay || instance.opacity < .99f)
        continue;
      const auto found = instance_meshes_.find(instance.geometry.get());
      if (found == instance_meshes_.end() || !bgfx::isValid(found->second.ibh)) continue;
      if (instance.gpu_skin) {
        if (instance.skin.size() < 48u * 16u || !bgfx::isValid(found->second.skin_vbh) ||
            !bgfx::isValid(ao_skin_depth_program_) || !render_debug().skinned_casters)
          continue;
        // Skinned bounds are the rest pose: cull by distance instead.
        const float dx = instance.transform[12] - slot.position[0];
        const float dy = instance.transform[13] - slot.position[1];
        const float dz = instance.transform[14] - slot.position[2];
        if (dx * dx + dy * dy + dz * dz > (slot.range + 3.f) * (slot.range + 3.f)) continue;
        stamps.push_back({instance.geometry.get(), instance.geometry->revision,
                          found->second.skin_vbh.idx, found->second.ibh.idx, instance.transform});
        stamps.back().revision ^= skin_palette_hash(instance.skin);
      } else if (instance.vehicle_material) {
        if (!debug_overlay_.render_options().shadows || !bgfx::isValid(found->second.vbh) ||
            !shadow_bounds_visible(*instance.geometry, instance.transform, frustum) ||
            covered_by_car_tiles(instance))
          continue;
        stamps.push_back({instance.geometry.get(), instance.geometry->revision,
                          found->second.vbh.idx, found->second.ibh.idx, instance.transform});
      } else {
        continue;
      }
      dynamic.push_back(&instance);
    }
    shadowed.push_back(s);
    if (slot.valid && stamps == slot.stamps && render_debug().shadow_caching) continue;
    const uint16_t view_id = uint16_t(128 + s);
    bgfx::setViewFrameBuffer(view_id, fixture_shadow_fb_);
    bgfx::setViewRect(view_id, uint16_t(s % 4 * tile), uint16_t(s / 4 * tile), tile, tile);
    bgfx::setViewClear(view_id, BGFX_CLEAR_DEPTH, 0, 1.f);
    bgfx::setViewTransform(view_id, view, proj);
    enc()->touch(view_id);
    for (const auto &instance : slot.statics) {
      const auto found = instance_meshes_.find(instance.geometry.get());
      if (found == instance_meshes_.end() || !bgfx::isValid(found->second.vbh) ||
          !bgfx::isValid(found->second.ibh))
        continue;
      submit_groups(view_id, found->second, instance.transform, nullptr);
    }
    draw_prop_batches(view_id, slot.static_batches, true, conservative);
    for (const auto *instance : dynamic)
      submit_groups(view_id, instance_meshes_.at(instance->geometry.get()), instance->transform,
                    instance->gpu_skin ? instance : nullptr);
    slot.stamps = std::move(stamps);
    slot.valid = true;
    ++lamp_shadow_redraws_;
  }
  // Shadowed lamps go first in the lamp array, in slot order; the rest keep
  // their nearest-first order. A shadowed lamp outside the frustum-selected
  // set is added at full power (its light is near the car; the lit set
  // leaves kLampShadowSlots entries free for this).
  // Car-tiled lamps follow (or share an entry with) the scenery-shadowed
  // ones; together at most kLampSpecial entries.
  std::vector<LampLight> ordered;
  ordered.reserve(scenery_lamps_.size() + shadowed.size() + car_shadow_tiles_.size());
  std::vector<char> taken(scenery_lamps_.size(), 0);
  const auto car_tile_of = [&](const void *source) {
    for (size_t k = 0; k < car_shadow_tiles_.size(); ++k)
      if (car_shadow_tiles_[k].source == source) return int(k);
    return -1;
  };
  const auto set_car_tile = [&](size_t entry, const void *source) {
    const int k = car_tile_of(source);
    if (k < 0) {
      lamp_car_tiles_[entry] = {0.f, 0.f, 0.f, 0.f};
      return;
    }
    const auto &tile = car_shadow_tiles_[size_t(k)];
    lamp_car_matrices_[entry] = tile.sample;
    lamp_car_tiles_[entry] = {float(k % 8) * .125f, 2.f / 3.f + float(k / 8) / 6.f,
                              tile.strength, 0.f};
  };
  const auto push_lamp = [&](const void *source, const LampLight &fallback) {
    const int index = find_lamp(source);
    if (index >= 0) {
      ordered.push_back(scenery_lamps_[size_t(index)]);
      taken[size_t(index)] = 1;
    } else {
      ordered.push_back(fallback);
    }
  };
  for (const auto s : shadowed) {
    const auto &slot = lamp_shadow_slots_[s];
    lamp_shadow_matrices_[ordered.size()] = slot.sample;
    lamp_shadow_tiles_[ordered.size()] = {float(s % 4) * .25f, float(s / 4) / 3.f,
                                          slot.weight, 0.f};
    set_car_tile(ordered.size(), slot.source);
    push_lamp(slot.source, slot.lamp);
  }
  for (const auto &tile : car_shadow_tiles_) {
    if (std::any_of(shadowed.begin(), shadowed.end(), [&](size_t s) {
          return lamp_shadow_slots_[s].source == tile.source;
        }))
      continue;
    lamp_shadow_tiles_[ordered.size()] = {0.f, 0.f, 0.f, 0.f};
    set_car_tile(ordered.size(), tile.source);
    push_lamp(tile.source, tile.lamp);
  }
  lamp_shadow_count_ = ordered.size();
  for (size_t i = 0; i < scenery_lamps_.size(); ++i)
    if (!taken[i]) ordered.push_back(scenery_lamps_[i]);
  scenery_lamps_ = std::move(ordered);
  lamp_shadow_atlas_bound_ = lamp_shadow_count_ > 0;
}

// A separate directional field: punctual shadow visibility must never occlude
// sunlight, nor may the sky's ambient term replace the direct source.
// Headlight shadows (#283). Every lit headlight beam in the lamp array is a
// candidate (update_car_lights already applied the #279 rules: installed
// part, running engine, wear); the followed car's come first, then the
// others in the array's nearest-first order, up to kHeadlightShadows tiles.
// One perspective frustum per headlight part, along its beam, 2 x the outer
// cone half-angle plus a margin wide, from kHeadlightShadowNear to its range.
// Casters are the main view's: world meshes, vehicles other than the
// light's own car, characters, and the instanced props in the frustum (a
// caster outside the camera view but inside a beam casts nothing; the beams
// point ahead of the followed car, mostly inside the view). Tiles are redrawn
// every frame: the beams move with their cars.
void LibBgfxBackend::update_headlight_shadows(const VideoOptions &video) {
  for (auto &lamp : scenery_lamps_) lamp.shadow_tile = -1;
  const size_t was = headlight_shadow_count_;
  headlight_shadow_count_ = 0;
  if (was) ++light_uniform_revision_;
  const size_t budget = size_t(render_debug_budget(render_debug().headlight_tiles,
      int(kHeadlightShadows), int(kHeadlightShadows)));
  if (garage_ || video.shadows == 0 || !render_debug().headlight_shadows || !budget ||
      !host_enhanced_graphics().headlight_shadows || !host_enhanced_graphics().car_lights ||
      !render_debug().car_lights || !host_enhanced_graphics().local_lights ||
      !bgfx::isValid(static_shadow_program_))
    return;
  std::vector<size_t> picks;
  for (int pass = 0; pass < 2 && picks.size() < budget; ++pass)
    for (size_t i = 0; i < std::min(scenery_lamps_.size(), kSceneryLamps) && picks.size() < budget; ++i) {
      const auto &lamp = scenery_lamps_[i];
      if (!lamp.headlight || !(lamp.intensity > 0.f) || lamp.cos_outer < -1.f) continue;
      const bool focus = headlight_focus_car_ && lamp.car == headlight_focus_car_;
      if (focus != (pass == 0)) continue;
      picks.push_back(i);
    }
  if (picks.empty()) return;
  perf::Scope perf_scope(perf::Zone::Shadows);
  const uint16_t tile = uint16_t(std::max<int>(64, local_shadow_resolution() / 2));
  if (bgfx::isValid(headlight_shadow_fb_) && headlight_shadow_tile_ != tile) {
    bgfx::destroy(headlight_shadow_fb_);
    headlight_shadow_fb_ = BGFX_INVALID_HANDLE;
    headlight_shadow_tex_ = BGFX_INVALID_HANDLE;
  }
  headlight_shadow_tile_ = tile;
  if (!bgfx::isValid(headlight_shadow_fb_)) {
    const auto depth = bgfx::createTexture2D(uint16_t(tile * 3), uint16_t(tile * 2), false, 1,
        bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(depth)) return;
    headlight_shadow_fb_ = bgfx::createFrameBuffer(1, &depth, true);
    if (!bgfx::isValid(headlight_shadow_fb_)) { bgfx::destroy(depth); return; }
    headlight_shadow_tex_ = depth;
  }
  const auto *caps = bgfx::getCaps();
  const float crop[16] = {.5f, 0, 0, 0,
                          0, caps->originBottomLeft ? .5f : -.5f, 0, 0,
                          0, 0, caps->homogeneousDepth ? .5f : 1.f, 0,
                          .5f, .5f, caps->homogeneousDepth ? .5f : 0.f, 1};
  const bool conservative = (caps->supported & BGFX_CAPS_CONSERVATIVE_RASTER) != 0;
  // The chassis of each vehicle instance, to leave a light's own car out.
  std::vector<const void *> owner(instances_.size(), nullptr);
  for (size_t i = 0; i < instances_.size(); ++i)
    if (instances_[i].vehicle_material && instances_[i].key)
      if (auto *part = part_render_owner(instances_[i].key)) owner[i] = part_chassis(part);
  for (size_t t = 0; t < picks.size(); ++t) {
    auto &lamp = scenery_lamps_[picks[t]];
    const bx::Vec3 eye{lamp.position[0], lamp.position[1], lamp.position[2]};
    const bx::Vec3 dir{lamp.direction[0], lamp.direction[1], lamp.direction[2]};
    const bx::Vec3 up = std::abs(dir.y) > .98f ? bx::Vec3{0, 0, 1} : bx::Vec3{0, 1, 0};
    float view[16], proj[16], clip[16];
    bx::mtxLookAt(view, eye, bx::add(eye, dir), up);
    const float half = std::acos(std::clamp(lamp.cos_outer, -1.f, 1.f)) * 57.2957795f;
    bx::mtxProj(proj, std::min(2.f * half + 2.f * kHeadlightShadowMargin, 170.f), 1.f,
                kHeadlightShadowNear, std::max(lamp.range, kHeadlightShadowNear + 1.f),
                caps->homogeneousDepth);
    bx::mtxMul(clip, view, proj);
    bx::mtxMul(headlight_shadow_matrices_[t].data(), clip, crop);
    const float u = 1.f / 3.f, v = .5f;
    headlight_shadow_rects_[t] = {float(t % 3) * u, float(t / 3) * v, u, v};
    const auto frustum = view_frustum(view, proj);
    auto casters = std::make_shared<std::vector<const SceneInstance *>>();
    for (size_t i = 0; i < instances_.size(); ++i) {
      const auto &instance = instances_[i];
      if (!instance.geometry || instance.sky || instance.light_flags || instance.emissive_overlay ||
          instance.opacity < .99f || instance.geometry->ground_polygons)
        continue;
      if (instance.vehicle_material && lamp.car && owner[i] == lamp.car) continue;
      const auto found = instance_meshes_.find(instance.geometry.get());
      if (found == instance_meshes_.end() || !bgfx::isValid(found->second.ibh)) continue;
      if (instance.gpu_skin) {
        if (instance.skin.size() < 48u * 16u || !bgfx::isValid(found->second.skin_vbh) ||
            !bgfx::isValid(ao_skin_depth_program_) || !render_debug().skinned_casters)
          continue;
        const float dx = instance.transform[12] - lamp.position[0];
        const float dy = instance.transform[13] - lamp.position[1];
        const float dz = instance.transform[14] - lamp.position[2];
        if (dx * dx + dy * dy + dz * dz > (lamp.range + 3.f) * (lamp.range + 3.f)) continue;
      } else if (!bgfx::isValid(found->second.vbh) ||
                 !shadow_bounds_visible(*instance.geometry, instance.transform, frustum)) {
        continue;
      }
      casters->push_back(&instance);
    }
    auto batches = std::make_shared<std::vector<SceneBatch>>();
    const float reach2 = (lamp.range + 10.f) * (lamp.range + 10.f);
    for (const auto &batch : prop_batches_) {
      if (batch.instances.empty() || !batch.geometry) continue;
      SceneBatch kept;
      kept.geometry = batch.geometry;
      kept.lighting = batch.lighting;
      for (const auto &instance : batch.instances) {
        const float dx = instance.transform[12] - lamp.position[0];
        const float dy = instance.transform[13] - lamp.position[1];
        const float dz = instance.transform[14] - lamp.position[2];
        if (dx * dx + dy * dy + dz * dz > reach2) continue;
        if (shadow_bounds_visible(*batch.geometry, instance.transform, frustum))
          kept.instances.push_back(instance);
      }
      if (!kept.instances.empty()) batches->push_back(std::move(kept));
    }
    const uint16_t view_id = uint16_t(kHeadlightShadowView + t);
    bgfx::setViewFrameBuffer(view_id, headlight_shadow_fb_);
    bgfx::setViewRect(view_id, uint16_t(t % 3 * tile), uint16_t(t / 3 * tile), tile, tile);
    bgfx::setViewClear(view_id, BGFX_CLEAR_DEPTH, 0, 1.f);
    bgfx::setViewTransform(view_id, view, proj);
    // One job per tile view (#270): it may run on a worker encoder.
    queue_or_run([this, view_id, casters, batches, conservative] {
      enc()->touch(view_id);
      const float no_apertures[4]{};
      enc()->setUniform(u_aperture_params_, no_apertures);
      for (const auto *instance : *casters) {
        const auto &part = instance_meshes_.at(instance->geometry.get());
        const bool skinned = instance->gpu_skin;
        for (const auto &group : part.shadow_groups) {
          const auto surface = scx_surface(part.geo.info.version, group);
          if (surface.blend) continue;
          const float threshold = !surface.cutout ? -1.f
                                  : part.geo.info.version == 4 ? group.alpha_reference / 255.f : .5f;
          const float params[4] = {0, threshold, float(group.base_map.channel), 0};
          set_uniform(u_static_shadow_params_, params);
          const auto slot = group.texture_index;
          enc()->setTexture(0, s_tex_, slot < part.tex.size() && bgfx::isValid(part.tex[slot])
                                          ? part.tex[slot] : white_tex_);
          enc()->setTransform(instance->transform.data());
          if (skinned) {
            enc()->setUniform(u_skin_bones_, instance->skin.data(), 48);
            enc()->setVertexBuffer(0, part.skin_vbh);
          } else {
            enc()->setVertexBuffer(0, part.vbh);
          }
          enc()->setIndexBuffer(part.ibh, group.index_start, group.index_count);
          enc()->setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                          (conservative ? BGFX_STATE_CONSERVATIVE_RASTER : 0));
          submit_counted(view_id, skinned ? ao_skin_depth_program_ : static_shadow_program_);
        }
      }
      draw_prop_batches(view_id, *batches, true, conservative);
    });
    lamp.shadow_tile = int(t);
    ++headlight_shadow_redraws_;
  }
  headlight_shadow_count_ = picks.size();
  ++light_uniform_revision_;
}

void LibBgfxBackend::update_interior_sun(const VideoOptions &video) {
  ++light_uniform_revision_;
  bool was_active = interior_sun_active_;
  interior_sun_active_ = false;
  if (!garage_ || video.shadows == 0 ||
      !host_enhanced_graphics().dynamic_lighting ||
      !debug_overlay_.render_options().map_shadows) {
    interior_sun_casters_.clear();
    return;
  }
  struct Aperture {
    std::array<float, 16> inverse;
    std::array<float, 4> uv, edge;
  };
  std::vector<Aperture> apertures;
  bgfx::TextureHandle window_texture = BGFX_INVALID_HANDLE;
  // Stock compatibility adapter: "ablak" is the authored window section.
  // Do not infer light transmission from arbitrary blended signs or glass cars.
  for (const auto &instance : instances_) {
    if (!instance.geometry || instance.sky || instance.vehicle_material)
      continue;
    const auto &geo = *instance.geometry;
    for (const auto &group : geo.groups) {
      if (group.name != "ablak" ||
          !scx_surface(geo.info.version, group).blend ||
          group.texture_index >= geo.textures.size())
        continue;
      const auto texture = tex_for_osd_icon(geo.textures[group.texture_index]);
      if (!bgfx::isValid(texture) || texture.idx == white_tex_.idx)
        continue;
      // One texture and at most 32 triangles per interior are budgeted. Reject
      // unsupported mixed masks rather than opening an unmasked wall.
      if (bgfx::isValid(window_texture) && texture.idx != window_texture.idx)
        continue;
      window_texture = texture;
      const auto &uv = group.base_map.channel == 2   ? geo.uv3
                       : group.base_map.channel == 1 ? geo.uv2
                                                     : geo.uvs;
      for (size_t i = group.index_start;
           i + 2 < group.index_start + group.index_count &&
           apertures.size() < 32;
           i += 3) {
        bx::Vec3 v[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        float t[3][2];
        bool valid = true;
        for (int k = 0; k < 3; ++k) {
          const auto index = geo.indices[i + k];
          if (index * 3 + 2 >= geo.positions.size() ||
              index * 2 + 1 >= uv.size()) {
            valid = false;
            break;
          }
          const float scale = geo.position_scale;
          v[k] = bx::mul({geo.positions[index * 3] * scale,
                          geo.positions[index * 3 + 1] * scale,
                          -geo.positions[index * 3 + 2] * scale},
                         instance.transform.data());
          t[k][0] =
              uv[index * 2] * group.base_map.scale_u + group.base_map.offset_u;
          t[k][1] = uv[index * 2 + 1] * group.base_map.scale_v +
                    group.base_map.offset_v;
        }
        if (!valid)
          continue;
        const auto a = bx::sub(v[1], v[0]), b = bx::sub(v[2], v[0]);
        const auto cross = bx::cross(a, b);
        if (bx::dot(cross, cross) < 1e-8f)
          continue;
        const auto n = bx::normalize(cross);
        const float basis[16] = {a.x, a.y, a.z, 0, b.x,    b.y,    b.z,    0,
                                 n.x, n.y, n.z, 0, v[0].x, v[0].y, v[0].z, 1};
        Aperture aperture;
        bx::mtxInverse(aperture.inverse.data(), basis);
        aperture.uv = {t[0][0], t[0][1], t[1][0] - t[0][0], t[1][1] - t[0][1]};
        aperture.edge = {t[2][0] - t[0][0], t[2][1] - t[0][1], 0, 0};
        apertures.push_back(aperture);
      }
    }
  }
  if (apertures.empty()) {
    interior_sun_casters_.clear();
    return;
  }
  interior_sun_ = SceneSun{};
  scene_sun_apply_day_cycle(interior_sun_);
  if (interior_sun_.direction[1] >= 0)
    return;
  const uint16_t size=sun_shadow_resolution();
  if(interior_sun_size_!=size) {
    if(bgfx::isValid(interior_sun_fb_)) bgfx::destroy(interior_sun_fb_);
    if(bgfx::isValid(interior_aperture_fb_)) bgfx::destroy(interior_aperture_fb_);
    interior_sun_fb_=BGFX_INVALID_HANDLE;interior_sun_tex_=BGFX_INVALID_HANDLE;
    interior_aperture_fb_=BGFX_INVALID_HANDLE;interior_aperture_tex_=BGFX_INVALID_HANDLE;
    was_active=false;
  }
  interior_sun_size_=size;
  if (!bgfx::isValid(interior_sun_fb_)) {
    const auto depth = bgfx::createTexture2D(
        size, size, false, 1, bgfx::TextureFormat::D32F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(depth))
      return;
    interior_sun_fb_ = bgfx::createFrameBuffer(1, &depth, true);
    if (!bgfx::isValid(interior_sun_fb_)) {
      bgfx::destroy(depth);
      return;
    }
    interior_sun_tex_ = depth;
  }
  if(!bgfx::isValid(interior_aperture_fb_)) {
    const auto transmission=bgfx::createTexture2D(size,size,false,1,bgfx::TextureFormat::RG32F,
        BGFX_TEXTURE_RT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP);
    const auto depth=bgfx::createTexture2D(size,size,false,1,bgfx::TextureFormat::D32F,BGFX_TEXTURE_RT);
    if(!bgfx::isValid(transmission) || !bgfx::isValid(depth)) {
      if(bgfx::isValid(transmission)) bgfx::destroy(transmission);
      if(bgfx::isValid(depth)) bgfx::destroy(depth);
      return;
    }
    const bgfx::TextureHandle attachments[]={transmission,depth};
    interior_aperture_fb_=bgfx::createFrameBuffer(2,attachments,true);
    if(!bgfx::isValid(interior_aperture_fb_)) {bgfx::destroy(transmission);bgfx::destroy(depth);return;}
    interior_aperture_tex_=transmission;
  }
  float center[3]{};
  float radius = 1;
  for (const auto &instance : instances_) {
    if (!instance.geometry || !instance.geometry->ground_polygons)
      continue;
    const auto &geo = *instance.geometry;
    const float scale = geo.position_scale;
    auto c =
        bx::mul({(geo.info.bbox_min[0] + geo.info.bbox_max[0]) * scale * .5f,
                 (geo.info.bbox_min[1] + geo.info.bbox_max[1]) * scale * .5f,
                 -(geo.info.bbox_min[2] + geo.info.bbox_max[2]) * scale * .5f},
                instance.transform.data());
    center[0] = c.x;
    center[1] = c.y;
    center[2] = c.z;
    radius = std::max(radius,
                      std::hypot(geo.info.bbox_max[0] - geo.info.bbox_min[0],
                                 geo.info.bbox_max[2] - geo.info.bbox_min[2]) *
                              scale * .5f +
                          4);
    break;
  }
  // A metre-scale interior field, independent of the outdoor kilometre splits.
  const float travel[3] = {interior_sun_.direction[0],
                           interior_sun_.direction[1],
                           -interior_sun_.direction[2]};
  const auto *caps = bgfx::getCaps();
  const auto projection =
      shadow_projection(center, travel, radius, size, caps->homogeneousDepth,
                        caps->originBottomLeft);
  interior_sun_matrix_ = projection.sample;
  std::vector<ShadowCasterStamp> stamps;
  for (const auto &instance : instances_) {
    if (!instance.geometry || instance.sky || instance.light_flags ||
        instance.emissive_overlay || instance.gpu_skin ||
        (instance.vehicle_material && !debug_overlay_.render_options().shadows))
      continue;
    cache_scene_mesh(instance.geometry, instance.key, false);
    const auto &part = instance_meshes_.at(instance.geometry.get());
    stamps.push_back({instance.geometry.get(), instance.geometry->revision,
                      part.vbh.idx, part.ibh.idx, instance.transform});
  }
  interior_sun_active_ = true;
  if (was_active && interior_sun_mask_ == window_texture.idx &&
      interior_sun_cached_matrix_ == interior_sun_matrix_ &&
      stamps == interior_sun_casters_)
    return;
  constexpr uint16_t view = 65;
  bgfx::setViewFrameBuffer(view, interior_sun_fb_);
  bgfx::setViewRect(view, 0, 0, size, size);
  bgfx::setViewClear(view, BGFX_CLEAR_DEPTH, 0, 1.f, 0);
  bgfx::setViewTransform(view, projection.view.data(),
                         projection.projection.data());
  enc()->touch(view);
  std::array<std::array<float, 16>, 32> matrices{};
  std::array<std::array<float, 4>, 32> uvs{}, edges{};
  for (size_t i = 0; i < apertures.size(); ++i) {
    matrices[i] = apertures[i].inverse;
    uvs[i] = apertures[i].uv;
    edges[i] = apertures[i].edge;
  }
  for (const auto &stamp : stamps) {
    const auto &part = instance_meshes_.at(stamp.source);
    if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh))
      continue;
    // The overlay and the wall may be coincident. Cut only the shell within
    // 8 cm of the actual window plane, never furniture or the opposite wall.
    const float aperture_params[4] = {
        part.geo.ground_polygons ? float(apertures.size()) : 0.f, .08f, 0, 0};
    for (const auto &group : part.geo.groups) {
      const auto surface = scx_surface(part.geo.info.version, group);
      if (surface.blend && group.name != "ablak")
        continue;
      const bool window = surface.blend && group.name == "ablak" &&
          group.texture_index < part.geo.textures.size() &&
          tex_for_osd_icon(part.geo.textures[group.texture_index]).idx == window_texture.idx;
      const float threshold = surface.blend     ? (window ? .5f : -1.f)
                              : !surface.cutout ? -1.f
                              : part.geo.info.version == 4
                                  ? group.alpha_reference / 255.f
                                  : .5f;
      const float params[4] = {0, threshold, float(group.base_map.channel), 0};
      set_uniform(u_static_shadow_params_, params);
      enc()->setUniform(u_aperture_params_, aperture_params);
      if (part.geo.ground_polygons) {
        enc()->setUniform(u_aperture_matrices_, matrices.data(),
                         uint16_t(apertures.size()));
        enc()->setUniform(u_aperture_uv_, uvs.data(),
                         uint16_t(apertures.size()));
        enc()->setUniform(u_aperture_edge_, edges.data(),
                         uint16_t(apertures.size()));
      }
      enc()->setTexture(1, s_aperture_, window_texture,
                       BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
      const auto slot = group.texture_index;
      enc()->setTexture(0, s_tex_,
                       window ? window_texture
                       : surface.blend ? white_tex_
                       : slot < part.tex.size() && bgfx::isValid(part.tex[slot])
                           ? part.tex[slot]
                           : white_tex_);
      enc()->setTransform(stamp.transform.data());
      enc()->setVertexBuffer(0, part.vbh);
      enc()->setIndexBuffer(part.ibh, group.index_start, group.index_count);
      enc()->setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
      submit_counted(view, static_shadow_program_);
    }
  }
  const float none[4]{};
  enc()->setUniform(u_aperture_params_, none);
  // Only authored apertures may admit direct sunlight into this interior.
  // Store their light-space depth as well as transmission, so a window
  // behind the receiver cannot illuminate it. The regular depth field still
  // supplies occlusion by frames, roof, walls, vehicles and furniture.
  bgfx::setViewFrameBuffer(66,interior_aperture_fb_);
  bgfx::setViewRect(66,0,0,size,size);
  bgfx::setViewClear(66,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,uint32_t(0),1.f);
  bgfx::setViewTransform(66,projection.view.data(),projection.projection.data());
  enc()->touch(66);
  for(const auto& stamp:stamps) {
    const auto& part=instance_meshes_.at(stamp.source);
    for(const auto& group:part.geo.groups) {
      if(group.name!="ablak" || !scx_surface(part.geo.info.version,group).blend ||
         group.texture_index>=part.geo.textures.size() ||
         tex_for_osd_icon(part.geo.textures[group.texture_index]).idx!=window_texture.idx) continue;
      bind_baked_light(&group,nullptr);
      const float mode[4]={0,4,0,0};
      const auto& uv=group.base_map;
      const float transform[4]={uv.scale_u,uv.scale_v,uv.offset_u,uv.offset_v};
      enc()->setUniform(u_alpha_test_,mode);
      set_uniform(u_mesh_base_uv_,transform);
      enc()->setTexture(0,s_tex_,window_texture);
      enc()->setTransform(stamp.transform.data());
      enc()->setVertexBuffer(0,part.vbh);enc()->setIndexBuffer(part.ibh,group.index_start,group.index_count);
      enc()->setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_Z|BGFX_STATE_DEPTH_TEST_LESS);
      submit_counted(66,program_);
    }
  }
  const float reset_mode[4]{};enc()->setUniform(u_alpha_test_,reset_mode);
  interior_sun_mask_ = window_texture.idx;
  interior_sun_casters_ = std::move(stamps);
  interior_sun_cached_matrix_ = interior_sun_matrix_;
}

void LibBgfxBackend::update_static_shadow(const float *eye, const VideoOptions &video) {
  ++light_uniform_revision_;
  update_static_shadow_inner(eye, video);
}

// #371: after a sun step, the atlas of the previous direction stays as it
// was (with the matrices its tiles were drawn with) and the receivers fade
// from it to the new atlas over render_debug().shadow_crossfade seconds, so
// a step moves the shadows smoothly. The two atlases swap; the new one is
// drawn in full. A crossfade starts only from a complete atlas of the
// previous frame, and stops if the shadow origin rebases (the previous
// matrices are relative to the old origin).
void LibBgfxBackend::update_shadow_crossfade(const std::array<float, 3>& sun_direction, uint16_t size) {
  const float duration = std::clamp(render_debug().shadow_crossfade, 0.f, 5.f);
  const double now =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const bool stepped = atlas_sun_valid_ && sun_direction != atlas_sun_direction_;
  const bool complete = sun_cascade_frame_ == frames_ - 1 && shadow_cached_origin_ == shadow_origin_ &&
                        std::all_of(cascade_tile_valid_.begin(), cascade_tile_valid_.end(),
                                    [](bool valid) { return valid; });
  if (stepped && duration > 0.f && complete) {
    if (!bgfx::isValid(static_shadow_prev_fb_)) {
      const auto depth = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::D32F,
          BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
      if (bgfx::isValid(depth)) {
        static_shadow_prev_fb_ = bgfx::createFrameBuffer(1, &depth, true);
        if (bgfx::isValid(static_shadow_prev_fb_)) static_shadow_prev_tex_ = depth;
        else bgfx::destroy(depth);
      }
    }
    if (bgfx::isValid(static_shadow_prev_fb_)) {
      std::swap(static_shadow_fb_, static_shadow_prev_fb_);
      std::swap(static_shadow_tex_, static_shadow_prev_tex_);
      shadow_cascade_prev_matrices_ = shadow_cascade_matrices_;
      shadow_blend_start_ = now;
      shadow_blending_ = true;
      cascade_tile_valid_.fill(false);
      shadow_cache_valid_ = false;
    }
  }
  atlas_sun_direction_ = sun_direction;
  atlas_sun_valid_ = true;
  float weight = 0.f;
  if (shadow_blending_) {
    weight = duration > 0.f ? 1.f - float((now - shadow_blend_start_) / duration) : 0.f;
    if (weight <= 0.f || shadow_cached_origin_ != shadow_origin_) {
      shadow_blending_ = false;
      weight = 0.f;
    }
  }
  shadow_blend_ = weight;
}

// #359: the map props of the sun's outer frustum, reused across frames.
// They are static map placements, so the collection depends only on the
// frustum: it is made over the outer box widened by kSunBatchMargin and kept
// while the camera stays within half that margin of where it was made, the
// held sun direction (#371), the world and the shadow origin are unchanged,
// for at most kSunBatchFrames frames (prop LOD follows the main camera). Each
// cascade still culls the placements against its own frustum, so the cached
// superset draws exactly what a fresh collection would.
std::shared_ptr<const std::vector<SceneBatch>> LibBgfxBackend::sun_prop_batches(
    const float* relative_focus, const float* direction, uint16_t resolution) {
  constexpr float kSunBatchMargin = 40.f;
  constexpr int32_t kSunBatchFrames = 60;
  const std::array<float, 3> focus{relative_focus[0] + shadow_origin_[0], relative_focus[1] + shadow_origin_[1],
                                   relative_focus[2] + shadow_origin_[2]};
  const std::array<float, 3> light{direction[0], direction[1], direction[2]};
  const float moved = std::hypot(focus[0] - sun_batch_focus_[0], focus[1] - sun_batch_focus_[1],
                                 focus[2] - sun_batch_focus_[2]);
  if (sun_batch_cache_ && render_debug().sun_batch_cache && moved <= kSunBatchMargin * .5f &&
      light == sun_batch_direction_ && sun_batch_world_ == scene_instances_world() &&
      sun_batch_origin_ == shadow_origin_ && frames_ - sun_batch_frame_ >= 0 &&
      frames_ - sun_batch_frame_ < kSunBatchFrames)
    return sun_batch_cache_;
  const auto* caps = bgfx::getCaps();
  const auto wide = shadow_projection(relative_focus, direction, cascade_radii().back() + kSunBatchMargin,
                                      resolution, caps->homogeneousDepth, caps->originBottomLeft);
  auto world_view = wide.view;
  for (int row = 0; row < 3; ++row)
    for (int axis = 0; axis < 3; ++axis) world_view[12 + row] -= wide.view[axis * 4 + row] * shadow_origin_[axis];
  const auto frustum = view_frustum(world_view.data(), wide.projection.data());
  // A fresh vector: the cascade jobs of earlier frames may still read the
  // previous one.
  auto batches = std::make_shared<std::vector<SceneBatch>>();
  scene_instances_collect_batches(&frustum, *batches);
  sun_batch_cache_ = std::move(batches);
  sun_batch_focus_ = focus;
  sun_batch_direction_ = light;
  sun_batch_world_ = scene_instances_world();
  sun_batch_origin_ = shadow_origin_;
  sun_batch_frame_ = frames_;
  return sun_batch_cache_;
}

// #371: a sun direction that rotates every frame (the game clock advances
// it continuously) rotates the cascade light basis by a tiny angle each
// frame; texel snapping only absorbs translation, so caster edges flip
// texels everywhere. Hold the direction until the sun has moved a few
// hundredths of a degree: a shadow then shifts by millimetres at once,
// every few seconds, and a still camera can reuse the cached atlas.
std::array<float, 3> LibBgfxBackend::shadow_sun_direction(const SceneSun& sun) {
  std::array<float, 3> current{sun.direction[0], sun.direction[1], sun.direction[2]};
  const float length = std::sqrt(current[0] * current[0] + current[1] * current[1] +
                                 current[2] * current[2]);
  if (length > 1e-6f)
    for (auto& axis : current) axis /= length;
  const float step = std::clamp(render_debug().shadow_sun_step, 0.f, 5.f);
  if (shadow_sun_valid_ && step > 0.f) {
    const float cosine = current[0] * shadow_sun_direction_[0] + current[1] * shadow_sun_direction_[1] +
                         current[2] * shadow_sun_direction_[2];
    if (cosine >= std::cos(step * 3.14159265f / 180.f)) return shadow_sun_direction_;
  }
  shadow_sun_direction_ = current;
  shadow_sun_valid_ = true;
  return current;
}

void LibBgfxBackend::update_static_shadow_inner(const float *eye,
                                          const VideoOptions &video) {
  const uint16_t point_shadow_tile=local_shadow_resolution();
  const uint16_t point_shadow_width=point_shadow_tile*3,point_shadow_height=point_shadow_tile*2;
  const uint16_t resolution=scenery_shadow_resolution();
  perf::Scope perf_scope(perf::Zone::Shadows);
  perf::Scope detail_scope(perf::Zone::SunShadows);
  const float no_apertures[4]{};
  enc()->setUniform(u_aperture_params_, no_apertures);
  static_shadow_active_ = false;
  static_shadow_local_ = false;
  shadow_cascades_active_ = false;
  shadow_meshes_.clear();
  point_shadow_active_ = false;
  shadow_blend_ = 0.f;
  if (point_light_ && point_light_->valid() && point_light_->casts_shadows &&
      video.shadows != 0 && debug_overlay_.render_options().map_shadows &&
      bgfx::isValid(static_shadow_program_)) {
    const auto &light = *point_light_;
    if (!bgfx::isValid(static_shadow_fb_) ||
        static_shadow_size_ != point_shadow_width) {
      shadow_cache_valid_ = false;
      if (bgfx::isValid(static_shadow_fb_))
        bgfx::destroy(static_shadow_fb_);
      static_shadow_fb_ = BGFX_INVALID_HANDLE;
      static_shadow_tex_ = BGFX_INVALID_HANDLE;
      destroy_prev_shadow_atlas();
      const auto depth = bgfx::createTexture2D(
          point_shadow_width, point_shadow_height, false, 1,
          bgfx::TextureFormat::D32F,
          BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
      if (!bgfx::isValid(depth))
        return;
      static_shadow_fb_ = bgfx::createFrameBuffer(1, &depth, true);
      if (!bgfx::isValid(static_shadow_fb_)) {
        bgfx::destroy(depth);
        return;
      }
      static_shadow_tex_ = depth;
      static_shadow_size_ = point_shadow_width;
    }
    shadow_origin_ = light.position;
    shadow_origin_world_ = scene_instances_world();
    std::vector<const SceneInstance *> casters;
    std::vector<ShadowCasterStamp> stamps;
    for (const auto &instance : instances_) {
      if (!instance.geometry || instance.sky || instance.light_flags ||
          instance.gpu_skin || instance.emissive_overlay ||
          (instance.vehicle_material &&
           !debug_overlay_.render_options().shadows))
        continue;
      cache_scene_mesh(instance.geometry, instance.key, false);
      const auto &part = instance_meshes_.at(instance.geometry.get());
      if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh))
        continue;
      casters.push_back(&instance);
      shadow_meshes_.insert(instance.geometry.get());
      stamps.push_back({instance.geometry.get(), instance.geometry->revision,
                        part.vbh.idx, part.ibh.idx, instance.transform});
    }
    static_shadow_active_ = true;
    static_shadow_local_ = true;
    point_shadow_active_ = true;
    static_shadow_eye_ = light.position;
    static_shadow_distance_ = light.range;
    // Colour/intensity/ambient updates do not change the depth field.
    const bool same_light = cached_point_light_ &&
                            cached_point_light_->position == light.position &&
                            cached_point_light_->range == light.range;
    if (shadow_cache_valid_ && same_light && stamps == shadow_cached_casters_ &&
        render_debug().shadow_caching)
      return;
    const auto *caps = bgfx::getCaps();
    for (unsigned face = 0; face < 6; ++face) {
      const auto projection = point_shadow_projection(
          face, light.range, caps->homogeneousDepth, caps->originBottomLeft);
      point_shadow_matrices_[face] = projection.sample;
      const auto frustum =
          view_frustum(projection.view.data(), projection.projection.data());
      const auto view = point_shadow_views[face];
      bgfx::setViewFrameBuffer(view, static_shadow_fb_);
      bgfx::setViewRect(view, uint16_t(face % 3 * point_shadow_tile),
                        uint16_t(face / 3 * point_shadow_tile),
                        point_shadow_tile, point_shadow_tile);
      bgfx::setViewClear(view, BGFX_CLEAR_DEPTH, uint32_t(0), 1.f, 0);
      bgfx::setViewTransform(view, projection.view.data(),
                             projection.projection.data());
      enc()->touch(view);
      for (const auto *instance : casters) {
        auto matrix = instance->transform;
        for (int axis = 0; axis < 3; ++axis)
          matrix[12 + axis] -= light.position[axis];
        if (!shadow_bounds_visible(*instance->geometry, matrix, frustum))
          continue;
        const auto &part = instance_meshes_.at(instance->geometry.get());
        for (const auto &group : part.shadow_groups) {
          const auto surface = scx_surface(part.geo.info.version, group);
          if (surface.blend)
            continue;
          const auto slot = group.texture_index;
          const auto texture =
              slot < part.tex.size() && bgfx::isValid(part.tex[slot])
                  ? part.tex[slot]
                  : white_tex_;
          const float threshold = !surface.cutout ? -1.f
                                  : part.geo.info.version == 4
                                      ? group.alpha_reference / 255.f
                                      : .5f;
          const float params[4] = {0, threshold, float(group.base_map.channel),
                                   0};
          set_uniform(u_static_shadow_params_, params);
          enc()->setTexture(0, s_tex_, texture);
          enc()->setTransform(matrix.data());
          enc()->setVertexBuffer(0, part.vbh);
          enc()->setIndexBuffer(part.ibh, group.index_start, group.index_count);
          enc()->setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
          submit_counted(view, static_shadow_program_);
        }
      }
    }
    static_shadow_matrix_ = point_shadow_matrices_[3];
    cached_point_light_ = point_light_;
    shadow_cached_casters_ = std::move(stamps);
    shadow_cache_valid_ = true;
    ++shadow_atlas_revision_;
    return;
  }
  if (cached_point_light_)
    shadow_cache_valid_ = false;
  cached_point_light_.reset();
  if (!eye || video.shadows == 0 || !scene_instances_outdoor()) {
    shadow_cache_valid_ = false;
    if (bgfx::isValid(static_shadow_fb_)) bgfx::destroy(static_shadow_fb_);
    static_shadow_fb_ = BGFX_INVALID_HANDLE;
    static_shadow_tex_ = BGFX_INVALID_HANDLE;
    destroy_prev_shadow_atlas();
    atlas_sun_valid_ = false;
    sun_batch_cache_.reset();  // let an unloaded world's props go
    return;
  }
  if (!debug_overlay_.render_options().map_shadows || !bgfx::isValid(static_shadow_program_))
    return;
  // The selected world's light is independent of visible terrain geometry.
  // Dealers/addons may contain only ordinary SCX meshes, or no ground in view.
  const auto sun = scene_instances_light();
  if (!sun.has_direction) return;
  const auto sun_direction = shadow_sun_direction(sun);
  if (sun_direction[1] >= 0.02f) return;
  const uint16_t size = resolution * 2;
  const bool conservative =
      (bgfx::getCaps()->supported & BGFX_CAPS_CONSERVATIVE_RASTER) != 0;
  if (size != static_shadow_size_ && bgfx::isValid(static_shadow_fb_)) {
    shadow_cache_valid_ = false;
    bgfx::destroy(static_shadow_fb_);
    static_shadow_fb_ = BGFX_INVALID_HANDLE;
    static_shadow_tex_ = BGFX_INVALID_HANDLE;
    destroy_prev_shadow_atlas();
  }
  if (!bgfx::isValid(static_shadow_fb_)) {
    const auto depth = bgfx::createTexture2D(
        size, size, false, 1, bgfx::TextureFormat::D32F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(depth))
      return;
    static_shadow_fb_ = bgfx::createFrameBuffer(1, &depth, true);
    if (!bgfx::isValid(static_shadow_fb_)) {
      bgfx::destroy(depth);
      return;
    }
    static_shadow_tex_ = depth;
    static_shadow_size_ = size;
    cascade_tile_valid_.fill(false);
  }
  const auto *caps = bgfx::getCaps();
  const float focus[3] = {eye[0], eye[1], -eye[2]};
  const float direction[3] = {sun_direction[0], sun_direction[1],
                              -sun_direction[2]};
  static_shadow_eye_ = {focus[0], focus[1], focus[2]};
  // The held sun direction (shadow_sun_direction) moves in small steps;
  // texel-snapped projections remove camera swimming without an
  // eight-metre cache jump.
  if(shadow_origin_world_!=scene_instances_world()) {
    shadow_cache_valid_=false;
    shadow_origin_world_=scene_instances_world();
    shadow_origin_={focus[0],focus[1],focus[2]};
  }
  shadow_rebase_origin(shadow_origin_,focus,direction,resolution);
  update_shadow_crossfade(sun_direction, size);
  const float relative_focus[3]={focus[0]-shadow_origin_[0],focus[1]-shadow_origin_[1],focus[2]-shadow_origin_[2]};
  const auto outer = shadow_projection(relative_focus,direction,cascade_radii().back(),resolution,
      caps->homogeneousDepth,caps->originBottomLeft);
  auto outer_world_view=outer.view;
  for(int row=0;row<3;++row) for(int axis=0;axis<3;++axis)
    outer_world_view[12+row]-=outer.view[axis*4+row]*shadow_origin_[axis];
  const auto outer_frustum=view_frustum(outer_world_view.data(),outer.projection.data());
  // One scene traversal; nested light volumes reuse these immutable draw records.
  // Shared with the cascade jobs (#359), which may run on worker encoders
  // after this function returns; nothing writes them after collection.
  struct SunCasters {
    std::vector<SceneInstance> casters;
    std::shared_ptr<const std::vector<SceneBatch>> batches;
    std::vector<const SceneInstance*> skinned;
    std::vector<float> skinned_distance;
  };
  const auto sun_casters=std::make_shared<SunCasters>();
  {
    perf::Scope collect_scope(perf::Zone::SunCollect);
    sun_casters->casters=scene_instances_collect(&outer_frustum,nullptr,false);
    sun_casters->batches=sun_prop_batches(relative_focus,direction,resolution);
  }
  const auto& casters=sun_casters->casters;
  const auto& outer_batches=*sun_casters->batches;
  for(const auto& instance:casters) {
    if(!instance.geometry || instance.sky || instance.vehicle_material || instance.gpu_skin ||
       instance.light_flags || instance.emissive_overlay) continue;
    shadow_meshes_.insert(instance.geometry.get());
    cache_scene_mesh(instance.geometry,nullptr,false);
  }
  for(const auto& batch:outer_batches) if(batch.geometry) {
    shadow_meshes_.insert(batch.geometry.get());cache_scene_mesh(batch.geometry,nullptr,true);
  }
  // Skinned pedestrians/drivers cast from the main-view records, whose bone
  // palettes are already complete. The outer collection deliberately skips
  // palette completion; recomputing it for off-camera characters would bring
  // back that CPU cost, so off-screen characters do not cast.
  auto& skinned_casters=sun_casters->skinned;
  auto& skinned_distance=sun_casters->skinned_distance;
  for(const auto& instance:instances_) {
    if(!instance.geometry || !instance.gpu_skin || instance.skin.size()<48u*16u ||
       instance.light_flags || instance.emissive_overlay || instance.opacity<.99f) continue;
    const float dx=instance.transform[12]-focus[0],dy=instance.transform[13]-focus[1],
                dz=instance.transform[14]-focus[2];
    const float distance=std::sqrt(dx*dx+dy*dy+dz*dz);
    if(distance>kSkinnedShadowRange) continue;
    const auto found=instance_meshes_.find(instance.geometry.get());
    if(found==instance_meshes_.end() || !bgfx::isValid(found->second.skin_vbh) ||
       !bgfx::isValid(found->second.ibh)) continue;
    skinned_casters.push_back(&instance);
    skinned_distance.push_back(distance);
  }
  std::array<ShadowProjection,4> projections;
  bool same_projection=shadow_cached_origin_==shadow_origin_;
  for(size_t i=0;i<4;++i) {
    projections[i]=shadow_projection(relative_focus,direction,cascade_radii()[i],resolution,
        caps->homogeneousDepth,caps->originBottomLeft);
    same_projection=same_projection && projections[i].sample==shadow_cascade_matrices_[i];
  }
  // Moving sunlight cannot reuse the atlas: avoid building a costly snapshot
  // on that path. Once it stops, one redraw establishes an exact caster snapshot.
  std::vector<ShadowCasterStamp> stamps;
  if(same_projection) {
    auto stamp=[&](const auto& geo,const auto& matrix) {
      const auto& part=instance_meshes_.at(geo.get());
      stamps.push_back({geo.get(),geo->revision,part.vbh.idx,part.ibh.idx,matrix});
    };
    for(const auto& instance:casters)
      if(instance.geometry && !instance.sky && !instance.vehicle_material && !instance.gpu_skin &&
         !instance.light_flags && !instance.emissive_overlay) stamp(instance.geometry,instance.transform);
    for(const auto& batch:outer_batches)
      for(const auto& instance:batch.instances) stamp(batch.geometry,instance.transform);
    // A pose change must redraw the atlas even when the root does not move.
    for(const auto* instance:skinned_casters) {
      stamp(instance->geometry,instance->transform);
      stamps.back().revision^=skin_palette_hash(instance->skin);
    }
  }
  // A frame that did not reach the sun cascades (no sun, point light in the
  // same atlas, shadows off) or a rebased origin leaves no reusable tile.
  if(sun_cascade_frame_!=frames_-1 || shadow_cached_origin_!=shadow_origin_)
    cascade_tile_valid_.fill(false);
  sun_cascade_frame_=frames_;
  const bool unchanged=shadow_cache_valid_ && same_projection && stamps==shadow_cached_casters_ &&
                       render_debug().shadow_caching;
  if(unchanged) {
    static_shadow_active_=true;shadow_cascades_active_=true;return;
  }
  // #361: while the camera moves, the two far cascades (320 m and 1000 m by
  // default) redraw on alternate frames. The skipped one keeps its tile and
  // the matrix it was drawn with; receivers sample it with that matrix, and
  // its 10 % extent margin covers the camera's motion in one frame. A still
  // camera (same_projection) draws them all, as the caster cache expects.
  const bool alternate=render_debug().cascade_alternation && !same_projection;
  for (size_t cascade = 0; cascade < size_t(4); ++cascade) {
    if(alternate && cascade>=2 && cascade_tile_valid_[cascade] && ((size_t(frames_)+cascade)&1u))
      continue;
    const auto& projection=projections[cascade];
    shadow_cascade_matrices_[cascade] = projection.sample;
    auto world_view=projection.view;
    for(int row=0;row<3;++row) for(int axis=0;axis<3;++axis)
      world_view[12+row]-=projection.view[axis*4+row]*shadow_origin_[axis];
    const auto frustum=view_frustum(world_view.data(),projection.projection.data());
    const auto view_id = shadow_views[cascade];
    bgfx::setViewFrameBuffer(view_id, static_shadow_fb_);
    bgfx::setViewRect(view_id, uint16_t(cascade % 2 * resolution),
                      uint16_t(cascade / 2 * resolution),
                      resolution, resolution);
    bgfx::setViewClear(view_id, BGFX_CLEAR_DEPTH, 0, 1.f, 0);
    bgfx::setViewTransform(view_id, projection.view.data(),
                           projection.projection.data());
    // Debug toggle (#243): an off cascade stays cleared (fully lit).
    if (!render_debug().sun_cascades[cascade]) {
      enc()->touch(view_id);
      cascade_tile_valid_[cascade]=false;
      continue;
    }
    cascade_tile_valid_[cascade]=true;
    // One job per cascade view (#359): culling and submission may run on a
    // worker encoder, like the car lamp tiles (#270). The view setup above
    // stays on the API thread; the job only reads the shared casters, the
    // mesh cache (filled above, before any job runs) and its own copies.
    const auto origin=shadow_origin_;
    const float cascade_radius=cascade_radii()[cascade];
    queue_or_run([this,sun_casters,frustum,view_id,origin,cascade_radius,conservative] {
    const auto& casters=sun_casters->casters;
    const auto& outer_batches=*sun_casters->batches;
    const auto& skinned_casters=sun_casters->skinned;
    const auto& skinned_distance=sun_casters->skinned_distance;
    enc()->touch(view_id);
    for (const auto &instance : casters) {
      // Vehicles retain their separate nearby depth map. The scenery depth
      // shader has no skin palette; skinned casters need their own path.
      // Ground POLY also contains buildings, stairs and track structures.
      // It must cast as well as receive; receiver-plane bias handles
      // self-shadow.
      if (!instance.geometry || instance.sky || instance.vehicle_material ||
          instance.gpu_skin || instance.light_flags ||
          instance.emissive_overlay)
        continue;
      if(!shadow_bounds_visible(*instance.geometry,instance.transform,frustum)) continue;
      const auto found = instance_meshes_.find(instance.geometry.get());
      if (found == instance_meshes_.end())
        continue;
      const auto &part = found->second;
      if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh))
        continue;
      for (const auto &group : part.shadow_groups) {
        const bool v4 = part.geo.info.version == 4;
        const auto surface = scx_surface(part.geo.info.version, group);
        if (surface.blend)
          continue;
        const bool cutout = surface.cutout;
        const auto slot = group.texture_index;
        const auto tex = slot < part.tex.size() && bgfx::isValid(part.tex[slot])
                             ? part.tex[slot]
                             : white_tex_;
        const float threshold = !cutout ? -1.f
                                : v4    ? group.alpha_reference / 255.f
                                        : .5f;
        const float params[4] = {
            0.f, threshold, static_cast<float>(group.base_map.channel), 0.f};
        set_uniform(u_static_shadow_params_, params);
        enc()->setTexture(0, s_tex_, tex);
        auto relative=instance.transform;
        for(int axis=0;axis<3;++axis) relative[12+axis]-=origin[axis];
        enc()->setTransform(relative.data());
        enc()->setVertexBuffer(0, part.vbh);
        enc()->setIndexBuffer(part.ibh, group.index_start, group.index_count);
        // Preserve narrow casters that overlap a texel without covering its
        // centre.
        const uint64_t coverage =
            conservative ? BGFX_STATE_CONSERVATIVE_RASTER : 0;
        enc()->setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                       coverage);
        submit_counted(view_id, static_shadow_program_);
      }
    }
    std::vector<SceneBatch> shadow_batches;
    for(const auto& batch:outer_batches) {
      SceneBatch filtered;filtered.geometry=batch.geometry;filtered.lighting=batch.lighting;
      for(const auto& instance:batch.instances)
        if(shadow_bounds_visible(*batch.geometry,instance.transform,frustum)) filtered.instances.push_back(instance);
      if(!filtered.instances.empty()) shadow_batches.push_back(std::move(filtered));
    }
    // Not a lamp-local field: pass that explicitly rather than reading
    // static_shadow_local_ from a worker (#270 rule).
    draw_prop_batches(view_id, shadow_batches, true, conservative, false, 0);
    if(bgfx::isValid(ao_skin_depth_program_) && render_debug().skinned_casters)
      for(size_t i=0;i<skinned_casters.size();++i) {
        // Skinned bounds describe the rest pose, not the animated limbs. Cull
        // by the camera-centred cascade radius instead (plus a body margin).
        if(skinned_distance[i]>cascade_radius+3.f) continue;
        const auto* instance=skinned_casters[i];
        const auto& part=instance_meshes_.at(instance->geometry.get());
        auto relative=instance->transform;
        for(int axis=0;axis<3;++axis) relative[12+axis]-=origin[axis];
        for(const auto& group:part.shadow_groups) {
          const auto surface=scx_surface(part.geo.info.version,group);
          if(surface.blend) continue;
          const float threshold=!surface.cutout ? -1.f
                                : part.geo.info.version==4 ? group.alpha_reference/255.f : .5f;
          const float params[4]={0.f,threshold,static_cast<float>(group.base_map.channel),0.f};
          set_uniform(u_static_shadow_params_,params);
          const auto slot=group.texture_index;
          enc()->setTexture(0,s_tex_,slot<part.tex.size() && bgfx::isValid(part.tex[slot])
                                        ? part.tex[slot] : white_tex_);
          enc()->setTransform(relative.data());
          enc()->setUniform(u_skin_bones_,instance->skin.data(),48);
          enc()->setVertexBuffer(0,part.skin_vbh);
          enc()->setIndexBuffer(part.ibh,group.index_start,group.index_count);
          enc()->setState(BGFX_STATE_WRITE_Z|BGFX_STATE_DEPTH_TEST_LESS|
                         (conservative ? BGFX_STATE_CONSERVATIVE_RASTER : 0));
          submit_counted(view_id,ao_skin_depth_program_);
        }
      }
    });
  }
  ++shadow_atlas_revision_;
  shadow_cached_casters_=std::move(stamps);
  shadow_cached_origin_=shadow_origin_;shadow_cache_valid_=same_projection;
  static_shadow_distance_ = cascade_radii().back();
  static_shadow_matrix_ = shadow_cascade_matrices_[0];
  static_shadow_active_ = true;
  shadow_cascades_active_ = true;
}

void LibBgfxBackend::update_car_depth(const float* position, const VideoOptions& video) {
  perf::Scope perf_scope(perf::Zone::Shadows);
  perf::Scope detail_scope(perf::Zone::CarDepth);
  car_depth_active_ = false;
  if (!position || video.shadows == 0 || !debug_overlay_.render_options().shadows ||
      !bgfx::isValid(static_shadow_program_)) return;

  const uint16_t size=scenery_shadow_resolution();
  if(bgfx::isValid(car_depth_fb_) && car_depth_size_!=size) {
    bgfx::destroy(car_depth_fb_);car_depth_fb_=BGFX_INVALID_HANDLE;car_depth_tex_=BGFX_INVALID_HANDLE;
  }
  car_depth_size_=size;
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
  // The same held direction as the cascades (#371): the car map would
  // otherwise flicker on its own as the sun moves.
  const auto sun_direction = sun.has_direction ? shadow_sun_direction(sun)
                                               : std::array<float, 3>{0.f, -1.f, 0.f};
  const bx::Vec3 direction{sun_direction[0], sun_direction[1], -sun_direction[2]};
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
  enc()->touch(id);

  for (const auto& instance : instances_) {
    if (!instance.vehicle_material || instance.light_flags) continue;
    const auto found = instance_meshes_.find(instance.geometry.get());
    if (found == instance_meshes_.end()) continue;
    const auto& part = found->second;
    if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh)) continue;
    for (const auto& group : part.geo.groups) {
      if ((group.material_flags & 0x40000000u) || group.diffuse[3] < .99f) continue;
      const float opaque[4] = {0.f, -1.f, 0.f, 0.f};
      set_uniform(u_static_shadow_params_, opaque);
      enc()->setTexture(0, s_tex_, white_tex_);
      enc()->setTransform(instance.transform.data());
      enc()->setVertexBuffer(0, part.vbh);
      enc()->setIndexBuffer(part.ibh, group.index_start, group.index_count);
      enc()->setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
      submit_counted(id, static_shadow_program_);
    }
  }
  car_depth_active_ = true;
}

// Smoke shadows (#454): what the tyre smoke lets through of the sun, in an
// orthographic map seen from the sun and fitted every frame to all the
// smoke (at most 200 m a side). RGB multiplies to the transmittance of
// every puff in a texel's column; alpha keeps, by MIN, the light depth of
// the puffs' fronts nearest the sun, so only receivers below the smoke
// darken (scene_shadow.sh smokeShadowTransmit, in fs_mesh and fs_vehicle).
// Debris casts nothing. No sorting: both blends are order-independent.
void LibBgfxBackend::update_smoke_shadow(const VideoOptions& video) {
  smoke_shadow_active_ = false;
  const auto& list = particles();
  const auto& clouds = particle_clouds();
  if (list.empty() || !bgfx::isValid(particle_program_) || video.shadows == 0 ||
      !debug_overlay_.render_options().shadows)
    return;
  const auto sun = scene_instances_light();
  if (!sun.has_direction) return;
  // The cascades' held direction (#371), into render space.
  const auto held = shadow_sun_direction(sun);
  const bx::Vec3 t = bx::normalize(bx::Vec3{held[0], held[1], -held[2]});
  if (t.y >= -.02f) return;  // the sun below the horizon casts nothing
  const bx::Vec3 up_hint = std::abs(t.y) > .98f ? bx::Vec3{0, 0, 1} : bx::Vec3{0, 1, 0};
  const bx::Vec3 right = bx::normalize(bx::cross(up_hint, t));
  const bx::Vec3 up = bx::cross(t, right);
  // The light-space box of every cloud particle.
  std::vector<uint32_t> chosen;
  chosen.reserve(list.size());
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (uint32_t i = 0; i < list.size() && chosen.size() < 16000; ++i) {
    const auto& p = list[i];
    if (p.cloud >= clouds.size() || clouds[p.cloud].type == 1) continue;
    const float life = std::clamp(p.age / p.life, 0.f, 1.f);
    const float radius = (p.size0 + (p.size1 - p.size0) * life) * .5f;
    const bx::Vec3 c{p.position[0], p.position[1], -p.position[2]};
    const float axis[3] = {bx::dot(c, right), bx::dot(c, up), bx::dot(c, t)};
    for (int k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], axis[k] - radius);
      hi[k] = std::max(hi[k], axis[k] + radius);
    }
    chosen.push_back(i);
  }
  if (chosen.empty()) return;
  constexpr float kMaxSide = 200.f;
  float half[2];
  float mid[2];
  for (int k = 0; k < 2; ++k) {
    mid[k] = (lo[k] + hi[k]) * .5f;
    half[k] = std::clamp((hi[k] - lo[k]) * .5f + 1.f, 2.f, kMaxSide * .5f);
  }
  const float depth = std::max(hi[2] - lo[2] + 2.f, 4.f);
  const bx::Vec3 eye = bx::add(bx::add(bx::mul(right, mid[0]), bx::mul(up, mid[1])),
                               bx::mul(t, lo[2] - 1.f));
  const auto* caps = bgfx::getCaps();
  float view[16], proj[16], clip[16];
  bx::mtxLookAt(view, eye, bx::add(eye, t), up);
  bx::mtxOrtho(proj, -half[0], half[0], -half[1], half[1], 0.f, depth, 0.f, caps->homogeneousDepth);
  bx::mtxMul(clip, view, proj);
  const float crop[16] = {
      .5f, 0.f, 0.f, 0.f, 0.f, caps->originBottomLeft ? .5f : -.5f, 0.f, 0.f,
      0.f, 0.f, caps->homogeneousDepth ? .5f : 1.f, 0.f, .5f, .5f,
      caps->homogeneousDepth ? .5f : 0.f, 1.f};
  bx::mtxMul(smoke_shadow_matrix_.data(), clip, crop);
  smoke_shadow_depth_scale_ = 1.f / depth;

  constexpr uint16_t kSize = 1024;
  if (!bgfx::isValid(smoke_shadow_fb_)) {
    smoke_shadow_tex_ = bgfx::createTexture2D(kSize, kSize, false, 1, bgfx::TextureFormat::RGBA16F,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(smoke_shadow_tex_)) return;
    smoke_shadow_fb_ = bgfx::createFrameBuffer(1, &smoke_shadow_tex_, true);
    if (!bgfx::isValid(smoke_shadow_fb_)) {
      bgfx::destroy(smoke_shadow_tex_);
      smoke_shadow_tex_ = BGFX_INVALID_HANDLE;
      return;
    }
  }
  const uint32_t n = static_cast<uint32_t>(chosen.size());
  if (bgfx::getAvailTransientVertexBuffer(n * 4, layout_) < n * 4 ||
      bgfx::getAvailTransientIndexBuffer(n * 6) < n * 6) return;
  bgfx::TransientVertexBuffer vb;
  bgfx::TransientIndexBuffer ib;
  bgfx::allocTransientVertexBuffer(&vb, n * 4, layout_);
  bgfx::allocTransientIndexBuffer(&ib, n * 6);
  auto* out = reinterpret_cast<MeshVertex*>(vb.data);
  auto* index = reinterpret_cast<uint16_t*>(ib.data);
  static constexpr float kCorner[4][2] = {{-1.f, -1.f}, {1.f, -1.f}, {-1.f, 1.f}, {1.f, 1.f}};
  for (uint32_t j = 0; j < n; ++j) {
    const auto& p = list[chosen[j]];
    const float life = std::clamp(p.age / p.life, 0.f, 1.f);
    const float radius = (p.size0 + (p.size1 - p.size0) * life) * .5f;
    // The same alpha as draw_particles.
    const float alpha = std::clamp((p.alpha0 + (p.alpha1 - p.alpha0) * std::pow(life, 1.5f)) *
                                       std::clamp(p.age / .15f, 0.f, 1.f), 0.f, 1.f);
    const uint32_t rgba = 0x00ffffffu | static_cast<uint32_t>(std::lround(alpha * 255.f)) << 24;
    for (int k = 0; k < 4; ++k) {
      MeshVertex v{};
      const float u = kCorner[k][0], w = kCorner[k][1];
      v.x = p.position[0] + (right.x * u + up.x * w) * radius;
      v.y = p.position[1] + (right.y * u + up.y * w) * radius;
      v.z = -p.position[2] + (right.z * u + up.z * w) * radius;
      v.u = u;
      v.v = w;
      v.u2 = radius;
      v.v2 = p.seed;
      v.v3 = 3.f;  // fs_particle: the smoke shadow mode
      v.rgba = rgba;
      out[j * 4 + k] = v;
    }
    const uint16_t base = static_cast<uint16_t>(j * 4);
    const uint16_t tri[6] = {base, uint16_t(base + 1), uint16_t(base + 2),
                             uint16_t(base + 1), uint16_t(base + 3), uint16_t(base + 2)};
    std::copy(std::begin(tri), std::end(tri), index + j * 6);
  }
  bgfx::setViewFrameBuffer(kSmokeShadowView, smoke_shadow_fb_);
  bgfx::setViewRect(kSmokeShadowView, 0, 0, kSize, kSize);
  bgfx::setViewClear(kSmokeShadowView, BGFX_CLEAR_COLOR, 0xffffffffu);
  bgfx::setViewTransform(kSmokeShadowView, view, proj);
  float identity[16];
  bx::mtxIdentity(identity);
  static const auto start = std::chrono::steady_clock::now();
  const float time = static_cast<float>(std::fmod(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 600.0));
  // y: light depth per metre of the fitted box.
  const float params[4] = {time, smoke_shadow_depth_scale_, 0.f, 0.f};
  enc()->setUniform(u_particle_params_, params);
  enc()->setTransform(identity);
  enc()->setVertexBuffer(0, &vb);
  enc()->setIndexBuffer(&ib);
  enc()->setTexture(0, s_tex_, white_tex_);
  enc()->setTexture(1, s_particle_depth_, white_tex_);
  enc()->setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                  BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_DST_COLOR, BGFX_STATE_BLEND_ZERO,
                                                 BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE) |
                  BGFX_STATE_BLEND_EQUATION_SEPARATE(BGFX_STATE_BLEND_EQUATION_ADD,
                                                     BGFX_STATE_BLEND_EQUATION_MIN));
  submit_counted(kSmokeShadowView, particle_program_);
  smoke_shadow_active_ = true;
}

}  // namespace slrr
