// Sun uniforms, baked lightmaps, and the vehicle material pass.
#include "bgfx_lib_backend.hpp"
#include "point_shadows.hpp"
#include "shadow_quality.hpp"
#include "shadow_cascades.hpp"

#include "slrr/part_runtime.hpp"
#include "slrr/skid_marks.hpp"
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
#include <filesystem>
#include <unordered_set>
#include <utility>

namespace slrr {

thread_local LibBgfxBackend::BindState* LibBgfxBackend::t_bind_ = nullptr;
thread_local bgfx::Encoder* LibBgfxBackend::t_encoder_ = nullptr;
bgfx::Encoder* LibBgfxBackend::host_encoder_ = nullptr;

LibBgfxBackend::BindingScope::BindingScope(LibBgfxBackend& b, uint16_t view) : backend(b) {
  auto& st = b.bind();
  st.binding_view = view;
  const bool sequential = view < b.sequential_views_.size() && b.sequential_views_.test(view);
  st.uniform_scope = b.uniform_cache_ && sequential;
  if (st.uniform_scope && (st.uniform_view != view || st.uniform_frame != b.frames_)) {
    ++st.uniform_generation;
    st.uniform_view = view;
    st.uniform_frame = b.frames_;
  }
  st.uniform_pending = false;
  st.texture_run = b.texture_cache_ && sequential;
  b.forget_textures();
  st.texture_set_mask = 0;
}

LibBgfxBackend::BindingScope::~BindingScope() {
  auto& b = backend;
  auto& st = b.bind();
  // Values set after the last submit attach to the next submit anywhere.
  if (st.uniform_pending) b.forget_uniforms();
  st.uniform_pending = false;
  st.uniform_scope = false;
  if (st.texture_run) enc()->discard(BGFX_DISCARD_BINDINGS);
  st.texture_run = false;
  st.texture_set_mask = 0;
  st.binding_view = UINT16_MAX;
}

void LibBgfxBackend::set_uniform(bgfx::UniformHandle handle, const float* value, uint16_t floats) {
  auto& st = bind();
  if (!bgfx::isValid(handle)) return;
  if (handle.idx >= st.uniform_values.size()) st.uniform_values.resize(size_t(handle.idx) + 1);
  auto& slot = st.uniform_values[handle.idx];
  ++st.uniform_sets;
  if (!st.uniform_scope) {
    slot.generation = 0;
    enc()->setUniform(handle, value);
    return;
  }
  const size_t bytes = size_t(std::min<uint16_t>(floats, 16)) * sizeof(float);
  if (slot.generation == st.uniform_generation && std::memcmp(slot.value.data(), value, bytes) == 0) {
    --st.uniform_sets;
    ++st.uniform_skips;
    return;
  }
  std::memcpy(slot.value.data(), value, bytes);
  slot.generation = st.uniform_generation;
  st.uniform_pending = true;
  enc()->setUniform(handle, value);
}

void LibBgfxBackend::set_texture(uint8_t stage, bgfx::UniformHandle sampler,
                                 bgfx::TextureHandle texture, uint32_t flags) {
  auto& st = bind();
  if (!st.texture_run || stage >= kTextureStages) {
    enc()->setTexture(stage, sampler, texture, flags);
    return;
  }
  st.texture_set_mask |= 1u << stage;
  auto& slot = st.texture_bindings[stage];
  if (slot.valid && slot.sampler == sampler.idx && slot.texture == texture.idx && slot.flags == flags) {
    ++st.texture_skips;
    return;
  }
  slot = {true, sampler.idx, texture.idx, flags};
  ++st.texture_sets;
  enc()->setTexture(stage, sampler, texture, flags);
}

void LibBgfxBackend::submit_bound(uint16_t view, bgfx::ProgramHandle program, uint32_t index_count) {
  auto& st = bind();
  uint8_t flags = BGFX_DISCARD_ALL;
  if (st.texture_run) {
    uint32_t kept = 0;
    for (size_t i = 0; i < kTextureStages; ++i)
      if (st.texture_bindings[i].valid) kept |= 1u << i;
    // A stage this draw did not set must stay unbound, as with a full discard.
    if (kept & ~st.texture_set_mask) forget_textures();
    else flags = BGFX_DISCARD_ALL & ~BGFX_DISCARD_BINDINGS;
  }
  submit_counted(view, program, flags);
  st.texture_set_mask = 0;
  st.uniform_pending = false;
  // bgfx drops a draw without indices together with its uniforms.
  if (!index_count) forget_uniforms();
}

namespace {
// Shared sun/local flare billboard primitive. z is the source's NDC depth,
// rather than a screen overlay: local glows test against ordinary scene depth.
void flare_sprite(MeshVertex* vertices, uint16_t* indices, unsigned next,
                  const float* inverse, float aspect, float z,
                  float cx, float cy, float half_x, float half_y, float angle,
                  float u, float v, float w, float h) {
  const float cos_angle = std::cos(angle), sin_angle = std::sin(angle);
  const uint16_t base = static_cast<uint16_t>(next * 4);
  for (unsigned i = 0; i < 4; ++i) {
    const float sx = i & 1 ? half_x : -half_x;
    const float sy = i & 2 ? -half_y : half_y;
    const auto p = bx::mulH({cx + (sx * cos_angle - sy * sin_angle) * aspect,
                             cy + sx * sin_angle + sy * cos_angle, z}, inverse);
    vertices[base + i] = {p.x, p.y, p.z, 0.f, 0.f, 1.f,
                          u + (i & 1 ? w : 0.f), v + (i & 2 ? h : 0.f)};
  }
  constexpr uint16_t quad[] = {0, 1, 2, 1, 3, 2};
  for (unsigned i = 0; i < 6; ++i) indices[next * 6 + i] = base + quad[i];
}
} // namespace

void LibBgfxBackend::draw_sun_flare(const float* view, const float* projection, uint16_t view_id) {
  if (!interior_sun_active_ && std::none_of(instances_.begin(), instances_.end(),
                   [](const SceneInstance& instance) { return instance.sky; }))
    return;
  if (instances_.empty()) return;
  const auto sun = interior_sun_active_ ? interior_sun_ : instances_.front().lighting;
  auto flare = sun.flare;
  if (interior_sun_active_) {
    flare.min_size=.5f; flare.max_size=3.f;
    flare.color=0xe4e4e4ff; flare.flares=8; flare.rays=6;
  }
  if (!sun.has_direction || (!interior_sun_active_ && !flare.texture) || flare.max_size <= 0.f) return;

  // Directional lights point away from the sun. Reflect script Z into bgfx.
  const bx::Vec3 direction{-sun.direction[0], -sun.direction[1], sun.direction[2]};
  const float depth = direction.x * view[2] + direction.y * view[6] + direction.z * view[10];
  if (depth <= 0.f) return;
  const float x = projection[0] * (direction.x * view[0] + direction.y * view[4] +
                                   direction.z * view[8]) / depth;
  const float y = projection[5] * (direction.x * view[1] + direction.y * view[5] +
                                   direction.z * view[9]) / depth;
  const float edge = std::max(std::abs(x), std::abs(y));
  if (edge >= 1.1f) return;
  const float visibility = std::clamp((1.1f - edge) * 5.f, 0.f, 1.f);
  const auto texture = tex_for_osd_icon(interior_sun_active_
      ? scene_instances_daylight_flare_texture() : scene_instances_texture(flare.texture));
  if (texture.idx == white_tex_.idx) return;
  float combined[16], inverse[16], identity[16];
  bx::mtxMul(combined, view, projection);
  bx::mtxInverse(inverse, combined);
  bx::mtxIdentity(identity);

  const uint32_t count = 1 + flare.rays + flare.flares;
  if (bgfx::getAvailTransientVertexBuffer(count * 4, layout_) < count * 4 ||
      bgfx::getAvailTransientIndexBuffer(count * 6) < count * 6) return;
  bgfx::TransientVertexBuffer vb;
  bgfx::TransientIndexBuffer ib;
  bgfx::allocTransientVertexBuffer(&vb, count * 4, layout_);
  bgfx::allocTransientIndexBuffer(&ib, count * 6);
  auto* vertices = reinterpret_cast<MeshVertex*>(vb.data);
  auto* indices = reinterpret_cast<uint16_t*>(ib.data);
  const float aspect = float(height_) / width_;
  unsigned next = 0;
  auto sprite = [&](float cx, float cy, float half_x, float half_y, float angle,
                    float u, float v, float w, float h) {
    // Reversed-Z puts the far plane at 0. The sun sits just in front of it.
    flare_sprite(vertices, indices, next++, inverse, aspect, .001f,
                 cx, cy, half_x, half_y, angle, u, v, w, h);
  };
  const float size = .02f * std::lerp(std::min(flare.min_size, flare.max_size),
                                     flare.max_size, visibility);
  sprite(x, y, size, size, 0, 0, 0, .5f, .5f);
  for (int i = 0; i < flare.rays; ++i) {
    const float angle = 3.14159265f * i / flare.rays;
    sprite(x, y, size * (1.25f + .65f * (i % 3)), size * .10f,
           angle, .5f, 0, .5f, .125f);
  }
  for (int i = 0; i < flare.flares; ++i) {
    const float t = 1.f - 1.8f * (i + 1) / (flare.flares + 1);
    const float radius = size * (.16f + .07f * (i % 4));
    sprite(x * t, y * t, radius, radius, 0, .25f * (i % 4),
           .5f + .25f * ((i / 4) % 2), .25f, .25f);
  }
  const float alpha = .32f * visibility * float(flare.color & 255) / 255.f;
  const float tint[] = {alpha * float(flare.color >> 24) / 255.f,
                        alpha * float((flare.color >> 16) & 255) / 255.f,
                        alpha * float((flare.color >> 8) & 255) / 255.f, 1.f};
  const float settings[] = {0.f, 1.f, 0.f, interior_sun_active_ ? -4.f : 0.f};
  float camera[16]; bx::mtxInverse(camera, view);
  const float camera_position[4] = {camera[12],camera[13],camera[14],0.f};
  bind_baked_light(nullptr, nullptr, BGFX_INVALID_HANDLE, false, nullptr,
                   interior_sun_active_ ? camera_position : nullptr);
  // Gate every ghost/ray with the same camera-to-sun visibility, not its own
  // screen position. This includes the alpha window/frame and room occluders.
  bind_interior_sun(interior_sun_active_, 5);
  enc()->setUniform(u_color_, tint);
  enc()->setUniform(u_alpha_test_, settings);
  enc()->setTransform(identity);
  enc()->setVertexBuffer(0, &vb);
  enc()->setIndexBuffer(&ib);
  set_texture(0, s_tex_, texture);
  enc()->setState(BGFX_STATE_WRITE_RGB |
                 (interior_sun_active_ ? uint64_t(0) : BGFX_STATE_DEPTH_TEST_GREATER) |
                 BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE));
  submit_counted(view_id, program_);
}

// Stock flag-256 FakeSpot: central authored halo only, never a road light.
// Like the sun, this uses the existing atlas primitive, .02 size and .32 tint
// convention. Local size additionally follows camera perspective. Native
// screen-size units and ray/ghost distribution are not recovered; do not add
// guessed streaks, colours or brightness gains. SLRR_rev 2663d686,
// src/render/vehicle-flare.ts documents mode-2's glow tile and cm camera offset.
void LibBgfxBackend::draw_nitro_flares(const float* view, const float* projection,
                                      uint16_t view_id) {
  if (!render_debug().car_lights || !host_enhanced_graphics().car_lights || !(projection[5] > 0.f)) return;
  const auto lights = car_lights_override_ ? *car_lights_override_ : part_car_lights();
  float camera[16], combined[16], inverse[16], identity[16];
  bx::mtxInverse(camera, view);
  bx::mtxMul(combined, view, projection);
  bx::mtxInverse(inverse, combined);
  bx::mtxIdentity(identity);
  BindingScope binding(*this, view_id);
  unsigned drawn = 0;
  for (const auto& source : lights) {
    if (source.kind != CarLightKind::Nitro || drawn >= kCarLights) continue;
    const auto& light = source.light;
    const auto& flare = light.flare;
    if (!light.has_flare || !flare.texture || !(flare.max_size > 0.f) ||
        !(light.range > 0.f) || !(source.power > 0.f) ||
        (light.type != LightResourceType::FakeSpot && light.type != LightResourceType::FakePoint))
      continue;
    float to_camera[3];
    float distance2 = 0.f, direction2 = 0.f, facing = 0.f;
    for (int i = 0; i < 3; ++i) {
      to_camera[i] = camera[12 + i] - source.position[i];
      distance2 += to_camera[i] * to_camera[i];
      direction2 += source.direction[i] * source.direction[i];
      facing += source.direction[i] * to_camera[i];
    }
    const float distance = std::sqrt(distance2);
    if (!(distance > .001f) || distance >= light.range) continue;
    float visibility = std::clamp(source.power, 0.f, 1.f);
    if (light.type == LightResourceType::FakeSpot) {
      if (!(direction2 > 1e-6f) || !(light.phi > 0.f)) continue;
      const float outer = std::cos(std::min(light.phi, 6.2831853f) * .5f);
      const float inner = std::cos(std::clamp(light.theta, 0.f, light.phi) * .5f);
      facing /= distance * std::sqrt(direction2);
      visibility *= inner > outer
          ? std::clamp((facing - outer) / (inner - outer), 0.f, 1.f)
          : facing >= outer ? 1.f : 0.f;
    }
    const auto& a = light.attenuation;
    const float attenuation = a[0] + a[1] * distance + a[2] * distance2;
    if (!(attenuation > 0.f)) continue;
    // The stock range is a bound, not permission for a constant-screen glow.
    visibility *= std::clamp(1.f / attenuation, 0.f, 1.f) * (1.f - distance / light.range);
    if (!(visibility > 0.f)) continue;
    const float offset = std::min(light.glow_offset * .01f, distance * .5f);
    const bx::Vec3 position{source.position[0] + to_camera[0] * offset / distance,
                            source.position[1] + to_camera[1] * offset / distance,
                            source.position[2] + to_camera[2] * offset / distance};
    const float depth = position.x * view[2] + position.y * view[6] +
                        position.z * view[10] + view[14];
    if (!(depth > .001f)) continue;
    const auto screen = bx::mulH(position, combined);
    const float size = .02f * std::lerp(std::min(flare.min_size, flare.max_size),
                                       flare.max_size, visibility) * projection[5] / depth;
    if (!std::isfinite(size) || !(size > 0.f) ||
        std::abs(screen.x) > 1.f + size || std::abs(screen.y) > 1.f + size) continue;
    auto texture_path = scene_instances_texture(flare.texture);
    // Stock particles:1F's lf_texture id is an empty type-14 entry, but the
    // same line explicitly names the actual flare artwork. Do not invent a
    // replacement when both authored sources are absent.
    if (texture_path.empty()) texture_path = light.flare_texture_path;
    const auto texture = tex_for_osd_icon(texture_path);
    if (texture.idx == white_tex_.idx) continue;
    if (bgfx::getAvailTransientVertexBuffer(4, layout_) < 4 ||
        bgfx::getAvailTransientIndexBuffer(6) < 6) break;
    bgfx::TransientVertexBuffer vb;
    bgfx::TransientIndexBuffer ib;
    bgfx::allocTransientVertexBuffer(&vb, 4, layout_);
    bgfx::allocTransientIndexBuffer(&ib, 6);
    const float tile = light.flare_texture_mode == 2 ? .5f : 1.f;
    flare_sprite(reinterpret_cast<MeshVertex*>(vb.data), reinterpret_cast<uint16_t*>(ib.data),
                 0, inverse, projection[0] / projection[5], screen.z,
                 screen.x, screen.y, size, size, 0.f, 0.f, 0.f, tile, tile);
    const float alpha = .32f * visibility * float(flare.color & 255) / 255.f;
    const float tint[4] = {alpha * float(flare.color >> 24) / 255.f,
                           alpha * float((flare.color >> 16) & 255) / 255.f,
                           alpha * float((flare.color >> 8) & 255) / 255.f, 1.f};
    const float settings[4] = {0.f, 1.f, 0.f, 0.f};
    bind_baked_light(nullptr, nullptr, BGFX_INVALID_HANDLE, false, nullptr, nullptr);
    enc()->setUniform(u_color_, tint);
    enc()->setUniform(u_alpha_test_, settings);
    enc()->setTransform(identity);
    enc()->setVertexBuffer(0, &vb);
    enc()->setIndexBuffer(&ib);
    set_texture(0, s_tex_, texture, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    enc()->setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_GREATER |
                   BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE));
    submit_bound(view_id, program_, 6);
    ++drawn;
  }
}

// Tyre skid marks (skid_marks.hpp): one transient batch of world quads that
// darken the road by multiplication, so a mark needs no lighting and never
// lightens a shadowed surface. Colour and opacity are SLRR_rev's road mark
// (0x17191a at 0.62).
void LibBgfxBackend::draw_skid_marks(uint16_t view_id) {
  const auto& marks = skid_mark_vertices();
  const uint32_t quads = static_cast<uint32_t>(marks.size() / 4);
  if (!quads) return;
  const uint32_t vertex_count = quads * 4, index_count = quads * 6;
  if (bgfx::getAvailTransientVertexBuffer(vertex_count, layout_) < vertex_count ||
      bgfx::getAvailTransientIndexBuffer(index_count) < index_count) return;
  bgfx::TransientVertexBuffer vb;
  bgfx::TransientIndexBuffer ib;
  bgfx::allocTransientVertexBuffer(&vb, vertex_count, layout_);
  bgfx::allocTransientIndexBuffer(&ib, index_count);
  auto* out = reinterpret_cast<MeshVertex*>(vb.data);
  constexpr float kOpacity = .62f, kDark = 1.f - float(0x17) / 255.f;
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const auto& m = marks[i];
    const float shade = 1.f - std::clamp(m.alpha, 0.f, 1.f) * kOpacity * kDark;
    const uint32_t c = static_cast<uint32_t>(std::lround(shade * 255.f));
    MeshVertex v{};
    v.x = m.x;
    v.y = m.y;
    v.z = -m.z;  // script space to render space, as part_car_lights
    v.ny = 1.f;
    v.rgba = c | c << 8 | c << 16 | 0xff000000u;
    out[i] = v;
  }
  auto* index = reinterpret_cast<uint16_t*>(ib.data);
  for (uint32_t q = 0; q < quads; ++q) {
    const uint16_t v = static_cast<uint16_t>(q * 4);
    const uint16_t tri[6] = {v, uint16_t(v + 1), uint16_t(v + 2), uint16_t(v + 1), uint16_t(v + 3),
                             uint16_t(v + 2)};
    std::copy(std::begin(tri), std::end(tri), index + q * 6);
  }
  BindingScope binding(*this, view_id);
  float identity[16];
  bx::mtxIdentity(identity);
  // y 2: emission path over a white mask (no sun, shadow or fog);
  // w -3: the diffuse is multiplied by the vertex colour.
  const float settings[4] = {0.f, 2.f, 0.f, -3.f};
  const float white[4] = {1.f, 1.f, 1.f, 1.f};
  bind_baked_light(nullptr, nullptr, BGFX_INVALID_HANDLE, false, nullptr, nullptr);
  enc()->setUniform(u_color_, white);
  enc()->setUniform(u_alpha_test_, settings);
  enc()->setTransform(identity);
  enc()->setVertexBuffer(0, &vb);
  enc()->setIndexBuffer(&ib);
  set_texture(0, s_tex_, white_tex_);
  enc()->setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_GREATER | BGFX_STATE_BLEND_MULTIPLY);
  submit_bound(view_id, program_, index_count);
}

// Fills the per-frame light data texture (#254, layout in scene_shadow.sh).
// Called where the per-view arrays used to be uploaded (#246): on the first
// lit draw of a Sequential view and frame, or every draw elsewhere. The
// texture is only updated when its contents changed; a bgfx texture update
// applies to the whole frame, which is right because light data changes only
// in the update_* functions, before any draw.
void LibBgfxBackend::upload_light_data() {
  if (!bgfx::isValid(light_data_tex_)) return;
  std::vector<float> data(size_t(kLightDataWidth) * kLightDataRows * 4, 0.f);
  const auto texel = [&](int x, int y) { return data.data() + (size_t(y) * kLightDataWidth + x) * 4; };
  const auto put = [&](int x, int y, float a, float b, float c, float d) {
    float* t = texel(x, y);
    t[0] = a; t[1] = b; t[2] = c; t[3] = d;
  };
  const auto put_matrix = [&](int x, int y, const float* m) {
    for (int column = 0; column < 4; ++column)
      std::copy(m + column * 4, m + column * 4 + 4, texel(x + column, y));
  };
  const auto lamps = std::min(scenery_lamps_.size(), kSceneryLamps);
  for (size_t i = 0; i < lamps; ++i) {
    const auto& lamp = scenery_lamps_[i];
    const int x = int(i);
    put(x, 0, lamp.position[0], lamp.position[1], lamp.position[2], lamp.range);
    put(64 + x, 0, lamp.color[0] * lamp.intensity, lamp.color[1] * lamp.intensity,
        lamp.color[2] * lamp.intensity,
        lamp.shadow_tile >= 0 && size_t(lamp.shadow_tile) < headlight_shadow_count_
            ? float(lamp.shadow_tile + 1) : 0.f);
    put(128 + x, 0, lamp.attenuation[0], lamp.attenuation[1], lamp.attenuation[2], lamp.cos_inner);
    put(192 + x, 0, lamp.direction[0], lamp.direction[1], lamp.direction[2], lamp.cos_outer);
    // Row 2 from 128 (after the headlight shadow rects): beam shaping.
    put(128 + x, 2, lamp.above_axis_cut, 0.f, 0.f, 0.f);
  }
  const size_t shadowed = lamp_shadow_atlas_bound_
      ? std::min({lamp_shadow_count_, scenery_lamps_.size(), kLampSpecial}) : size_t(0);
  for (size_t s = 0; s < shadowed; ++s) {
    const int x = int(s);
    put_matrix(4 * x, 1, lamp_shadow_matrices_[s].data());
    const auto& tile = lamp_shadow_tiles_[s];
    put(96 + x, 1, tile[0], tile[1], tile[2], tile[3]);
    put_matrix(120 + 4 * x, 1, lamp_car_matrices_[s].data());
    const auto& car = lamp_car_tiles_[s];
    put(216 + x, 1, car[0], car[1], car[2], car[3]);
  }
  // Headlight shadow tiles (#283), row 2 after the fixtures.
  for (size_t t = 0; t < std::min(headlight_shadow_count_, kHeadlightShadows); ++t) {
    put_matrix(96 + 4 * int(t), 2, headlight_shadow_matrices_[t].data());
    const auto& rect = headlight_shadow_rects_[t];
    put(120 + int(t), 2, rect[0], rect[1], rect[2], rect[3]);
  }
  const auto fixtures = std::min(fixture_lights_.size(), kFixtureLights);
  for (size_t i = 0; i < fixtures; ++i) {
    const auto& light = fixture_lights_[i];
    const int x = int(i);
    put(x, 2, light.position[0], light.position[1], light.position[2], light.range);
    put(12 + x, 2, light.color[0], light.color[1], light.color[2], light.intensity);
    put(24 + x, 2, light.direction[0], light.direction[1], light.direction[2], light.outer_cos);
    put_matrix(36 + 4 * x, 2, fixture_shadow_matrices_[i].data());
  }
  if (data == light_data_) return;
  light_data_ = std::move(data);
  bgfx::updateTexture2D(light_data_tex_, 0, 0, 0, 0, kLightDataWidth, kLightDataRows,
                        bgfx::copy(light_data_.data(), uint32_t(light_data_.size() * sizeof(float))));
  ++light_data_updates_;
}

void LibBgfxBackend::bind_fixture_lights(bool enabled, bool upload_arrays) {
  // The light data texture holds every light; the per-draw counts below
  // decide what a draw reads, so lit and unlit draws share it (#246, #254).
  const auto all_fixtures=std::min(fixture_lights_.size(),kFixtureLights);
  const size_t all_lamp_shadows=lamp_shadow_atlas_bound_
      ? std::min({lamp_shadow_count_,scenery_lamps_.size(),kLampSpecial}) : size_t(0);
  const auto all_lamps=std::min(scenery_lamps_.size(),kSceneryLamps);
  // The light data texture is uploaded in frame setup before workers start
  // (#270); during the parallel window nobody touches light_data_.
  if(upload_arrays && !parallel_window_) upload_light_data();
  const auto count=enabled?all_fixtures:size_t(0);
  const bool shadows=count && fixture_shadow_active_;
  // Street lamp shadows (#237) use the same atlas outside garages.
  const size_t lamp_shadows=enabled?all_lamp_shadows:size_t(0);
  const float params[4]={float(count),1.f/(fixture_shadow_tile_*4),1.f/(fixture_shadow_tile_*3),shadows?1.f:0.f};
  set_uniform(u_fixture_params_,params);
  set_texture(7,s_fixture_shadow_,shadows||lamp_shadows?fixture_shadow_tex_:depth_white_,
      BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP|BGFX_SAMPLER_COMPARE_LEQUAL);
  set_texture(11,s_light_data_,light_data_tex_,
      BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP);
  // Headlight shadow atlas (#283); entries without a tile never sample it.
  const bool headlights=enabled && headlight_shadow_count_ && bgfx::isValid(headlight_shadow_tex_);
  set_texture(12,s_headlight_shadow_,headlights?headlight_shadow_tex_:depth_white_,
      BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP|BGFX_SAMPLER_COMPARE_LEQUAL);
  const float headlight_texel[4]={headlight_shadow_tile_?1.f/(headlight_shadow_tile_*3.f):0.f,
                                  headlight_shadow_tile_?1.f/(headlight_shadow_tile_*2.f):0.f,0.f,0.f};
  set_uniform(u_headlight_shadow_,headlight_texel);
  const float lamp_shadow_params[4]={float(lamp_shadows),1.f/(fixture_shadow_tile_*4),
                                     1.f/(fixture_shadow_tile_*3),
                                     render_debug().lamp_shadow_reference?1.f:0.f};
  set_uniform(u_lamp_shadow_params_,lamp_shadow_params);
  // Scenery lamps (#237) follow the same enable as the fixtures: previews and
  // emissive lamp glass receive none.
  const auto lamps=enabled?all_lamps:size_t(0);
  const float lamp_params[4]={float(lamps),0.f,0.f,0.f};
  set_uniform(u_lamp_params_,lamp_params);
}

void LibBgfxBackend::bind_interior_sun(bool enabled, uint8_t stage) {
  const bool active = enabled && interior_sun_active_;
  const float direction[4] = {-interior_sun_.direction[0], -interior_sun_.direction[1],
                               interior_sun_.direction[2], 1.f/interior_sun_size_};
  const float color[4] = {interior_sun_.diffuse[0], interior_sun_.diffuse[1],
                           interior_sun_.diffuse[2], active ? 1.f : 0.f};
  set_uniform(u_interior_sun_dir_, direction);
  set_uniform(u_interior_sun_color_, color);
  set_uniform(u_interior_sun_mtx_, interior_sun_matrix_.data(), 16);
  set_texture(8,s_sun_aperture_,active?interior_aperture_tex_:white_tex_,
      BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP);
  set_texture(stage, s_interior_sun_, active ? interior_sun_tex_ : depth_white_,
      BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_COMPARE_LEQUAL);
}

  void LibBgfxBackend::bind_scene_shadow(bool enabled, uint8_t stage)
{
    const float filter[4]={float(std::clamp(host_enhanced_graphics().shadow_smoothing,0,2)),0,0,0};
    set_uniform(u_shadow_filter_,filter);
    // View-invariant arrays once per Sequential view and frame (#246).
    const bool upload = !light_arrays_current();
    bind_fixture_lights(enabled, upload);
    if (upload) {
      if (point_shadow_active_)
        enc()->setUniform(u_point_shadow_faces_, point_shadow_matrices_.data(), 6);
      if (shadow_cascades_active_) {
        enc()->setUniform(u_shadow_cascades_, shadow_cascade_matrices_.data(), 4);
        enc()->setUniform(u_shadow_cascades_prev_, shadow_cascade_prev_matrices_.data(), 4);
      }
      ++bind().light_array_uploads;
      if (bind().uniform_scope) bind().uniform_pending = true;
      bind().light_arrays_view = bind().binding_view;
      bind().light_arrays_frame = frames_;
      bind().light_arrays_revision = light_uniform_revision_;
    }
    bind_interior_sun(enabled, stage == 5 ? 6 : 5);
    const bool receive = enabled && static_shadow_active_ &&
                         debug_overlay_.render_options().map_shadows;
    const float params[4] = {receive ? 1.f : 0.f,
                             receive ? 1.f / static_shadow_size_ : 0.f, 0.00012f, static_shadow_local_ ? 1.f : 0.f};
    set_uniform(u_static_shadow_params_, params);
    const bool point = receive && point_shadow_active_;
    const float point_params[4] = {point ? 1.f : 0.f, 1.f / (local_shadow_resolution()*3),
                                   1.f / (local_shadow_resolution()*2), .015f};
    set_uniform(u_point_shadow_params_, point_params);
    const bool cascaded = receive && shadow_cascades_active_;
    const float origin[4]={shadow_origin_[0],shadow_origin_[1],shadow_origin_[2],0.f};
    set_uniform(u_shadow_origin_,origin);
    const float ranges[4] = {cascaded ? cascade_radii()[0] : 0.f, cascade_radii()[1],cascade_radii()[2],cascade_radii()[3]};
    set_uniform(u_shadow_ranges_,ranges);

    if (receive) {
      const float eye[4] = {static_shadow_eye_[0], static_shadow_eye_[1],
                            static_shadow_eye_[2], static_shadow_distance_};
      set_uniform(u_static_shadow_eye_, eye);
      set_uniform(u_static_shadow_mtx_, static_shadow_matrix_.data(), 16);
    }
    constexpr uint32_t shadow_sampler = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
                                        BGFX_SAMPLER_COMPARE_LEQUAL;
    const auto shadow_tex = receive ? static_shadow_tex_
                                    : (bgfx::isValid(depth_white_) ? depth_white_ : static_shadow_tex_);
    set_texture(stage, s_static_shadow_,
                     bgfx::isValid(shadow_tex) ? shadow_tex : white_tex_, shadow_sampler);
    // The previous sun step's atlas during a crossfade (#371), stage 13 in
    // fs_mesh and fs_vehicle.
    const bool fading = cascaded && shadow_blend_ > 0.f && bgfx::isValid(static_shadow_prev_tex_);
    const float blend[4] = {fading ? shadow_blend_ : 0.f, 0.f, 0.f, 0.f};
    set_uniform(u_shadow_blend_, blend);
    set_texture(13, s_static_shadow_prev_,
                fading ? static_shadow_prev_tex_ : bgfx::isValid(depth_white_) ? depth_white_ : white_tex_,
                shadow_sampler);
}

bool LibBgfxBackend::bind_point_light(bool enabled) {
  const bool point = enabled && point_light_ && point_light_->valid();
  const float legacy_color[4] = {1.f, .94f, .8f, 3.f};
  const float none[4]{};
  if (point) {
    const auto &light = *point_light_;
    const float position[4] = {light.position[0], light.position[1],
                               light.position[2], light.intensity>0.f?light.range:0.f};
    const float color[4] = {light.color[0], light.color[1], light.color[2],
                            light.intensity};
    set_uniform(u_local_light_, position);
    set_uniform(u_point_light_color_, color);
  } else {
    set_uniform(u_local_light_,
                     enabled && !point_light_ ? local_light_.data() : none);
    set_uniform(u_point_light_color_, legacy_color);
  }
  return point;
}

  void LibBgfxBackend::bind_baked_light(const ScxGroup* group,
                        const std::vector<bgfx::TextureHandle>* tex, bgfx::TextureHandle mix,
                        bool receive_static_shadow, const SceneSun* sun, const float* sky)
{
    // UI draws must not inherit atlas transforms from the last world mesh.
    if (!group) {
      const float identity_uv[4]={1.f,1.f,0.f,0.f};
      set_uniform(u_mesh_base_uv_,identity_uv);
      set_uniform(u_mesh_mix_uv_,identity_uv);
    }
    const float no_sky[4]{};
    set_uniform(u_local_light_, no_sky);
    set_uniform(u_cloud_sky_, sky ? sky : no_sky);
    if (sky) {
      // Noon is the reference field from the early procedural sky.
      // Keep animation without starting every day kilometres downwind.
      const float cloud_time = scene_sun_time() - 12.f * 3600.f;
      const float wind[4] = {cloud_time * 1.5f, cloud_time * .55f, 0.f, 0.f};
      set_uniform(u_cloud_wind_, wind);
    }
    float direction[4]{};
    if (sun && sun->set && sun->has_direction) {
      direction[0] = -sun->direction[0];
      direction[1] = -sun->direction[1];
      direction[2] = sun->direction[2];
      direction[3] = 1.f;
      const float ambient[4] = {sun->ambient[0], sun->ambient[1], sun->ambient[2], 0.f};
      const float diffuse[4] = {sun->diffuse[0], sun->diffuse[1], sun->diffuse[2], 0.f};
      set_uniform(u_sun_amb_, ambient);
      set_uniform(u_sun_diff_, diffuse);
    }
    set_uniform(u_sun_dir_, direction);
    float u[4] = {0.f, 0.f, 1.f, 0.f};
    float xf[4] = {1.f, 1.f, 0.f, 0.f};
    bgfx::TextureHandle lm = white_tex_;
    (void)tex;
    if (group) u[1] = static_cast<float>(group->base_map.channel);
    if (group && bgfx::isValid(mix))
      u[3] = 1.f + static_cast<float>(group->mix_map.channel);
    // The stock lightmap already contains the sun and its shadows. Leaving
    // it on skips the shadow map in the mesh shader.
    set_uniform(u_lightmap_, u);
    set_uniform(u_lightmap_uv_, xf);
    set_texture(1, s_lightmap_, lm);
    set_texture(2, s_mesh_mix_, bgfx::isValid(mix) ? mix : white_tex_, receive_static_shadow ? world_sampler() : BGFX_SAMPLER_NONE);
    bind_scene_shadow(receive_static_shadow, 3);
    const bool receive_car = receive_static_shadow && car_depth_active_;
    const float car_params[4] = {receive_car ? 1.f : 0.f, 1.f / car_depth_size_, .0006f, 0.f};
    set_uniform(u_car_depth_params_, car_params);
    if (receive_car) set_uniform(u_car_depth_mtx_, car_depth_matrix_.data(), 16);
    set_texture(4, s_car_depth_, receive_car ? car_depth_tex_ : depth_white_,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_COMPARE_LEQUAL);
    if (bind_point_light(receive_static_shadow && !sky)) {
      const float mode[4] = {0, 0, 0, -1};
      const auto &ambient = point_light_->ambient;
      const float amb[4] = {ambient[0], ambient[1], ambient[2], 0};
      set_uniform(u_sun_dir_, mode);
      set_uniform(u_sun_amb_, amb);
    }
    const float fog_color[4] = {fog_color_[0], fog_color_[1], fog_color_[2],
                                fog_enabled_ && render_debug().fog && host_enhanced_graphics().fog ? 1.f : 0.f};
    const float fog_range[4] = {fog_near_, fog_far_, 0.f, 0.f};
    const float fog_eye[4] = {fog_eye_[0], fog_eye_[1], fog_eye_[2], 0.f};
    if (bgfx::isValid(u_fog_color_)) set_uniform(u_fog_color_, fog_color);
    if (bgfx::isValid(u_fog_range_)) set_uniform(u_fog_range_, fog_range);
    if (bgfx::isValid(u_fog_eye_)) set_uniform(u_fog_eye_, fog_eye);
  }
  void LibBgfxBackend::draw_instance(uint16_t view_id,const SceneInstance& instance,const float* matrix,bool preview,int pass)
{
    // The view of this draw: per-view light arrays (#246), uniform values
    // and kept texture stages (#262).
    BindingScope binding(*this, view_id);
    const auto& part=instance_meshes_.at(instance.geometry.get());
    if(!bgfx::isValid(part.vbh)||!bgfx::isValid(part.ibh))return;
    const auto texture = [&](uint16_t slot) {
      for (const auto& [index, path] : instance.texture_replacements)
        if (slot == index && !path.empty()) {
          const auto replacement = resolve_texture(path);
          if (replacement.idx != white_tex_.idx) return replacement;
        }
      return slot < part.tex.size() && bgfx::isValid(part.tex[slot]) ? part.tex[slot] : white_tex_;
    };
    if (instance.vehicle_material && bgfx::isValid(vehicle_program_)) {
      for (const auto& group : part.geo.groups) {
        auto base = texture(group.texture_index);
        const bool painted = instance.paint_slot >= 0 && group.texture_index == instance.paint_slot;
        if (painted && !instance.paint_texture.empty()) base = resolve_texture(instance.paint_texture);
        auto mix = texture(group.mix_index);
        bool mixed = group.mix_index != 0xffff && group.mix_index != group.texture_index && mix.idx != white_tex_.idx;
        auto env = texture(group.reflection_index);
        bool generated = group.reflection_map.channel == 0xfffffffeu;
        const float strength = std::isfinite(group.reflection_strength) ? std::clamp(group.reflection_strength,0.f,1.f) : .5f;
        // 0x40000000 only sends the section through the transparent group.
        // Glass is ColorSpecularSpheremap (0x33) in that group. A grille is
        // ColorMixTex (0x03): texture alpha, not the glass tint.
        // 5 = ColorMixTex with a spheremap (headlight lens).
        // 4 = ColorMixTex. 3 = glass. 2 = chrome (0x33, no color map, or a
        // 0x0c cap whose color slot never loaded). A bare pipe is flags 0.
        // SCX3 blend (flags 0x101) is not SCX4's transparent-group bit, but
        // it blends too: flag-256 exhaust flames and the cockpit_rpm /
        // cockpit_speed pins (pin_small, a needle on a clear DXT5 card).
        // Lamp glows keep their own lamp path.
        const bool scx3_blend = scx_surface(part.geo.info.version, group).blend;
        const bool flame_blend = (instance.light_flags & 256u) && scx3_blend;
        const bool transparent = (group.material_flags & 0x40000000u) != 0 || flame_blend ||
            (scx3_blend && part.geo.info.version == 3 && !instance.light_flags);
        if (transparent != (pass == 1)) continue;
        const uint32_t kind_id = group.material_type;
        const bool glass = transparent && kind_id == 0x33u;
        const bool color_mix_env = kind_id == 0x0bu || kind_id == 0x13u || kind_id == 0x23u ||
                                   kind_id == 0x0fu || kind_id == 0x17u || kind_id == 0x27u;
        const bool color_mix = scx_color_mix_function(kind_id);
        const bool tex_missing = group.texture_index != 0xffff &&
            (group.texture_index >= part.tex.size() || !bgfx::isValid(part.tex[group.texture_index]));
        const bool chrome = !glass && kind_id == 0x33u && group.texture_index == 0xffff;
        const bool bare_cap = !glass && !chrome && tex_missing && group.material_type == 0x0cu &&
                              group.reflection_index != 0xffff;
        // A headlight lens names a reflection slot the render leaves empty.
        // Cabin glass already asks for a generated spheremap. Only the lens
        // keeps the stock alpha, so the housing texture shows through.
        const bool lens = glass && env.idx == white_tex_.idx && !generated;
        // Paint, chrome and glass take the live cubemap, or the sun gradient
        // until that cubemap is ready. The fixed chassis spheremap stays off.
        const bool enhanced_env = !preview && reflection_scene_ &&
            host_enhanced_graphics().dynamic_reflections && !lens &&
            (generated || painted || chrome || bare_cap || glass || color_mix_env ||
             group.reflection_index != 0xffff);
        const bool live_env = enhanced_env && reflection_ready_;
        const float kind = glass ? 3.f : color_mix_env ? 5.f : color_mix ? 4.f
                                : (chrome || bare_cap) ? 2.f : painted ? 1.f : 0.f;
        const bool has_reflection = enhanced_env || env.idx != white_tex_.idx;
        const float reflection_strength = live_env && painted ? std::max(strength, .35f) : strength;
        const float layers[4] = {mixed?1.f:0.f, has_reflection?reflection_strength:0.f, group.material_type==0x15&&mixed?1.f:0.f, kind};
        const float channels[4] = {static_cast<float>(group.base_map.channel),static_cast<float>(group.mix_map.channel),static_cast<float>(group.reflection_map.channel),lens?2.f:generated?1.f:0.f};
        auto map = [&](bgfx::UniformHandle uniform, const ScxTextureMap& uv) {
          const float value[4]={uv.scale_u,uv.scale_v,uv.offset_u,uv.offset_v}; enc()->setUniform(uniform,value);
        };
        enc()->setUniform(u_layers_,layers); enc()->setUniform(u_channels_,channels);
        // Paint replaces the slot, so the body keeps the texture color.
        // Tex / TexSpecular (tyre is 0x04) replace colour 0 with white:
        // the tread is the texture. A second multiply by the file diffuse
        // turns that tread black. Colour classes and bare metal keep it.
        const bool tex_family = group.material_type == 0x00u || group.material_type == 0x04u ||
                                group.material_type == 0x08u || group.material_type == 0x0cu ||
                                group.material_type == 0x10u || group.material_type == 0x14u ||
                                group.material_type == 0x20u || group.material_type == 0x24u;
        const bool has_tex = group.texture_index < part.geo.textures.size();
        float diffuse[4]={1,1,1,group.diffuse[3]};
        if(!has_tex || (!tex_family && !painted))
          std::copy(group.diffuse,group.diffuse+3,diffuse);
        // The create string's colour index. White is "no paint".
        if (instance.color != 0xffffffu) {
          const float paint[3] = {((instance.color >> 16) & 255) / 255.f,
                                  ((instance.color >> 8) & 255) / 255.f,
                                  (instance.color & 255) / 255.f};
          if (has_tex && !painted)
            for (int i = 0; i < 3; ++i) diffuse[i] *= paint[i];
          else if (!painted)
            std::copy(paint, paint + 3, diffuse);
        }
        // A spheremap ColorMixTex outputs texture.a times the env factor.
        if (color_mix_env) diffuse[3] = strength;
        const bool point = point_light_ && point_light_->valid() && !preview &&
                           !instance.light_flags;
        SceneSun sun = instance.lighting;
        if (point)
          std::copy(point_light_->ambient.begin(), point_light_->ambient.end(),
                    sun.ambient);
        const float sun_d[4] = {sun.diffuse[0], sun.diffuse[1], sun.diffuse[2],
                                sun.set ? 1.f : 0.f};
        const float sun_a[4] = {sun.ambient[0], sun.ambient[1], sun.ambient[2], 0.f};
        const float sun_s[4] = {sun.specular[0], sun.specular[1], sun.specular[2],
                                sun.set ? 1.f : 0.f};
        // Game direction is where the light travels. Toward it, with Z
        // flipped into the same space as the rendered vertices.
        const float sun_r[4] = {-sun.direction[0], -sun.direction[1],
                                sun.direction[2],
                                point               ? -1.f
                                : sun.has_direction ? 1.f
                                                    : 0.f};
        set_uniform(u_sun_diff_, sun_d);
        set_uniform(u_sun_amb_, sun_a);
        set_uniform(u_sun_spec_, sun_s);
        set_uniform(u_sun_dir_, sun_r);
        bind_point_light(!preview && !instance.light_flags);
        bind_ambient_occlusion(!preview && !transparent && !instance.light_flags && ao_receiver_view(view_id));
        enc()->setUniform(u_diffuse_,diffuse);
        const float spec[4] = {group.specular[0], group.specular[1], group.specular[2],
                               group.shininess > 0.f ? group.shininess : 1.f};
        enc()->setUniform(u_spec_, spec);
        map(u_base_uv_,group.base_map);map(u_mix_uv_,group.mix_map);map(u_env_uv_,group.reflection_map);
        bgfx::TextureHandle bake = white_tex_;
        float bake_u[4] = {0.f, static_cast<float>(group.base_map.channel), 1.f, 0.f};
        float bake_xf[4] = {1.f, 1.f, 0.f, 0.f};
        if (transparent && color_mix) bake_u[3] = 1.f;
        // A switched-on lamp is the glow texture, not a surface in shade.
        if (instance.light_flags) bake_u[0] = 1.f;
        set_uniform(u_lightmap_, bake_u);
        set_uniform(u_lightmap_uv_, bake_xf);
        set_texture(0,s_tex_,base,world_sampler());set_texture(1,s_mix_,mix,world_sampler());
        // Global environment textures share the image cache with OSD, but
        // material UV transforms need the scene's wrapping sampler here.
        set_texture(2, s_env_, env, BGFX_SAMPLER_NONE);
        set_texture(3,s_bake_,bake);
        // Map shadows affect direct vehicle light, not a car depth map that
        // contains this same car. Emissive lamps and previews stay unshadowed.
        bind_scene_shadow(!preview && !instance.light_flags, 5);
        const bool two_sided = part.geo.info.version != 4 ||
            (group.material_flags & 0x02000000u) != 0;
        const float reflection[4] = {live_env ? 1.f : enhanced_env ? 2.f : 0.f,
                                     two_sided ? 1.f : 0.f, 0.f, 0.f};
        enc()->setUniform(u_dynamic_reflection_, reflection);
        if (live_env) {
          set_texture(4, s_reflection_cube_, reflection_cubes_[reflection_write_ ^ 1]);
          const float probe[4] = {reflection_sample_origin_[0], reflection_sample_origin_[1],
              reflection_sample_origin_[2], reflection_box_valid_ ? 1.f : 0.f};
          const float low[4] = {reflection_box_min_[0], reflection_box_min_[1], reflection_box_min_[2], 0.f};
          const float high[4] = {reflection_box_max_[0], reflection_box_max_[1], reflection_box_max_[2], 0.f};
          enc()->setUniform(u_reflection_probe_, probe);
          enc()->setUniform(u_reflection_box_min_, low);
          enc()->setUniform(u_reflection_box_max_, high);
        }
        enc()->setTransform(matrix);enc()->setVertexBuffer(0,part.vbh);enc()->setIndexBuffer(part.ibh,group.index_start,group.index_count);
        // Lamp glass sits 0.5 mm in front of its lens since #284
        // (snap_lamp_glass): a depth tie with the lens must keep the glass.
        uint64_t state=BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_MSAA|
            (instance.light_flags ? BGFX_STATE_DEPTH_TEST_GEQUAL : BGFX_STATE_DEPTH_TEST_GREATER);
        // Like map SCX4, vehicle materials cull unless explicitly two-sided.
        // Interior backs can protrude through thin outer panels when drawn.
        if (!two_sided) state |= BGFX_STATE_CULL_CW;
        std::string extension = flame_blend && has_tex
            ? std::filesystem::path(part.geo.textures[group.texture_index]).extension().string() : "";
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        // Decoded PNG/PTX pixels are already premultiplied. Do not multiply
        // their soft flame edges by alpha a second time; DDS stays straight.
        const bool premultiplied_flame = flame_blend && has_tex && extension != ".dds";
        if (glass || premultiplied_flame)
          state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
        else if (transparent || group.diffuse[3] < .99f)
          state |= BGFX_STATE_BLEND_ALPHA;
        else
          state |= BGFX_STATE_WRITE_Z;
        enc()->setState(state);submit_bound(view_id,vehicle_program_,group.index_count);
      }
      return;
    }
    auto draw=[&](uint32_t first,uint32_t count,uint16_t texture_index,uint32_t flags,const ScxGroup* group){
      // Night meshes duplicate ordinary facade sections. Only sections with
      // an authored emission mask or colour contribute emissive light.
      if (instance.emissive_overlay &&
          (!group || (group->emission_index == 0xffff &&
           group->emission_color[0] == 0.f && group->emission_color[1] == 0.f &&
           group->emission_color[2] == 0.f))) return;
      // Colour function and alpha coverage are independent. In particular,
      // type-0 rail panels still blend, and type-3 bridge trusses still cut out.
      const auto surface = group ? scx_surface(part.geo.info.version, *group) : ScxSurface{};
      const bool blend = !instance.sky && (instance.opacity < 1.f || instance.emissive_overlay ||
          (!preview && surface.blend) ||
          (part.geo.info.version == 3 && surface.blend));
      if (pass != int(blend)) return;
      const auto tex = texture(texture_index);
      std::string extension = blend && texture_index < part.geo.textures.size()
          ? std::filesystem::path(part.geo.textures[texture_index]).extension().string() : "";
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      // Decoded images are premultiplied; DDS and an absent diffuse map are
      // straight alpha. A white fallback for untextured glass is not an image.
      const bool premultiplied = texture_index != 0xffff && extension != ".dds";
      // SCX4 storefronts use texture alpha to mix their paint colour with
      // the detail texture. Low-alpha texels are solid wall, not holes.
      const bool material_mix = group && instance.cull_backfaces &&
          part.geo.info.version == 4 && surface.color_mix;
      const bool authored_alpha = instance.cull_backfaces && part.geo.info.version == 4;
      // Script RenderRefs (garage stuff) keep the legacy half-alpha cutout,
      // except on ColorMixTex sections (the ROC tent canvas, #330).
      const bool cut = !material_mix && !blend && !preview && !instance.sky &&
          (authored_alpha ? surface.cutout
                          : scx_legacy_cutout(part.geo.info.version, group, flags));
      // V4 map foliage specifies GREATER with an 8-bit reference (default 0x38).
      const float alpha_test = !cut ? 0.f : authored_alpha && group
          ? -(1.f + group->alpha_reference) / 255.f : 1.f;
      // Authored two-sided cutouts, independent of asset directory names.
      const bool foliage_coverage = cut && authored_alpha && view_id < 72 &&
          (reset_flags_ & BGFX_RESET_MSAA_MASK) &&
          (flags & 0x02000000u) != 0;
      const bool vertex_mix = group && group->mix_vertex_color;
      const bool fixture_emitter=group && group->self_illuminated;
      // fs_mesh lets the sun light either face of an authored two-sided card.
      const float two_sided = authored_alpha && (flags & 0x02000000u) ? 4.f : 0.f;
      const float settings[4] = {alpha_test, instance.emissive_overlay ? 2.f : instance.sky || blend || vertex_mix || fixture_emitter ? 1.f : preview ? .5f : 0.f,
                                 two_sided + (foliage_coverage ? 2.f : blend ? 1.f : 0.f), material_mix ? -2.f : vertex_mix ? -1.f :
                                 baked_vertex_light(part.geo.ground_polygons && !preview) ? -3.f :
                                 !blend && preview && tex.idx != white_tex_.idx ? 1.f : 0.f};
      const uint32_t color = preview || instance.cull_backfaces ? instance.color : 0xffffffu;
      float tint[4]={((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,1};
      tint[3] *= instance.opacity;
      if (group && part.geo.info.version == 3 && texture_index == 0xffff)
        for (int i = 0; i < 4; ++i) tint[i] *= group->diffuse[i];
      if (material_mix)
        for (int i = 0; i < 3; ++i) tint[i] *= group->diffuse[i];
      if (blend && !instance.emissive_overlay && group)
        scx_modulate_blend(part.geo.info.version, *group, tint, premultiplied);
      if (instance.emissive_overlay && group && group->emission_index == 0xffff)
        for (int c = 0; c < 3; ++c) tint[c] *= group->emission_color[c];
      const ScxTextureMap identity;
      const ScxTextureMap& base_uv = group ? group->base_map : identity;
      const ScxTextureMap& mix_uv = group ? group->mix_map : identity;
      const float base_xf[4] = {base_uv.scale_u, base_uv.scale_v, base_uv.offset_u, base_uv.offset_v};
      const float mix_xf[4] = {mix_uv.scale_u, mix_uv.scale_v, mix_uv.offset_u, mix_uv.offset_v};
      set_uniform(u_mesh_base_uv_, base_xf);
      set_uniform(u_mesh_mix_uv_, mix_xf);
      enc()->setUniform(u_alpha_test_,settings);enc()->setUniform(u_color_,tint);enc()->setTransform(matrix);
      const bool skinned = instance.gpu_skin && bgfx::isValid(part.skin_vbh) &&
          bgfx::isValid(skin_program_) && instance.skin.size() >= 48u * 16u;
      if (skinned) enc()->setUniform(u_skin_bones_, instance.skin.data(), 48);
      enc()->setVertexBuffer(0, skinned ? part.skin_vbh : part.vbh);
      enc()->setIndexBuffer(part.ibh,first,count);
      set_texture(0, s_tex_, tex, preview ? BGFX_SAMPLER_NONE : world_sampler());
      bgfx::TextureHandle mix = BGFX_INVALID_HANDLE;
      if (group && part.geo.position_scale == 1.f && group->mix_index != 0xffff) {
        const auto candidate = texture(group->mix_index);
        if (candidate.idx != white_tex_.idx) mix = candidate;
      }
      const bool world_mesh = !preview && !instance.sky && !blend && !fixture_emitter;
      // Reflection faces are 256² copies of the city. They keep the sun
      // and receive the same scene shadow taps as the main view.
      const auto& graphics = host_enhanced_graphics();
      const bool procedural_sky = instance.sky && outdoor_lighting_;
      const float sky[4] = {matrix[12], matrix[13], matrix[14],
                            graphics.volumetric_clouds && render_debug().clouds ? 2.f : 1.f};
      const bool scene_light = procedural_sky ||
          (world_mesh && graphics.dynamic_lighting &&
           (outdoor_lighting_ || (garage_ && instance.lighting.set &&
                                  instance.lighting.has_direction)));
      bind_baked_light(group, &part.tex, mix, world_mesh,
                      scene_light ? &instance.lighting : nullptr, procedural_sky ? sky : nullptr);
      bind_ambient_occlusion(world_mesh && ao_receiver_view(view_id));
      if (instance.emissive_overlay && group && group->emission_index != 0xffff) {
        const auto& uv = group->emission_map;
        const float channels[4] = {0.f, static_cast<float>(group->base_map.channel),
                                   static_cast<float>(uv.channel), 0.f};
        const float transform[4] = {uv.scale_u, uv.scale_v, uv.offset_u, uv.offset_v};
        set_uniform(u_lightmap_, channels);
        set_uniform(u_lightmap_uv_, transform);
        // Missing masks are black, never a full-bright wall fallback.
        const auto mask = texture(group->emission_index);
        if (mask.idx == white_tex_.idx) return;
        set_texture(1, s_lightmap_, mask);
      }

      uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA;
      if (foliage_coverage) state |= BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
      if (instance.cull_backfaces && !(flags & 0x02000000u))
        state |= BGFX_STATE_CULL_CW;
      if (instance.emissive_overlay) {
        // Authored self-illumination is coplanar with the daytime facade.
        // Reverse-Z equality keeps it visible; never replace the opaque depth.
        state |= BGFX_STATE_DEPTH_TEST_GEQUAL |
                 BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
      } else if (blend) {
        state |= BGFX_STATE_DEPTH_TEST_GREATER | (premultiplied
            ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA)
            : BGFX_STATE_BLEND_ALPHA);
      } else if (!instance.sky) {
        state |= BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_GREATER;
      }
      enc()->setState(state);
      submit_bound(view_id, skinned ? skin_program_ : program_, count);
    };
    if(part.geo.groups.empty())draw(0,static_cast<uint32_t>(part.geo.index_count),0,0,nullptr);
    else for(const auto&g:part.geo.groups)draw(g.index_start,g.index_count,g.texture_index,g.material_flags,&g);
  }

bool LibBgfxBackend::blended_prop_instance(const SceneInstance& instance) {
  if (!instance.ground_prop || instance.emissive_overlay || instance.sky || instance.gpu_skin ||
      instance.vehicle_material || instance.light_flags || instance.opacity < 1.f ||
      !instance.cull_backfaces || !instance.texture_replacements.empty() || !instance.geometry)
    return false;
  const auto& mesh = *instance.geometry;
  const int version = static_cast<int>(mesh.info.version);
  if (mesh.ground_polygons || (version != 3 && version != 4)) return false;
  for (const auto& group : mesh.groups)
    if (scx_surface(version, group).blend) return true;
  return false;
}

void LibBgfxBackend::draw_props_instanced(uint16_t view_id, const ScenePart& part,
                                          const SceneSun& lighting, bool emissive, int pass,
                                          const PropPlacement* placements, size_t count) {
  if (!count || !bgfx::isValid(inst_program_) || !bgfx::isValid(part.vbh) ||
      !bgfx::isValid(part.ibh))
    return;
  BindingScope binding(*this, view_id);
  float identity[16];
  bx::mtxIdentity(identity);
  const auto texture = [&](uint16_t slot) {
    return slot < part.tex.size() && bgfx::isValid(part.tex[slot]) ? part.tex[slot] : white_tex_;
  };
  struct PropInstance {
    float matrix[16];
    float color[4];
  };
  static_assert(sizeof(PropInstance) == 80);
  const auto stride = static_cast<uint16_t>(sizeof(PropInstance));
  const int version = static_cast<int>(part.geo.info.version);
  // draw_instance's draw lambda for a map placement: cull_backfaces, no
  // preview, no sky, opacity 1, no skin and no texture replacement.
  const auto draw = [&](uint32_t first, uint32_t index_count, uint16_t texture_index,
                        uint32_t flags, const ScxGroup* group) {
    if (emissive &&
        (!group || (group->emission_index == 0xffff && group->emission_color[0] == 0.f &&
                    group->emission_color[1] == 0.f && group->emission_color[2] == 0.f)))
      return;
    const auto surface = group ? scx_surface(version, *group) : ScxSurface{};
    const bool blend = emissive || surface.blend;
    if (pass != int(blend)) return;
    const auto tex = texture(texture_index);
    std::string extension = blend && texture_index < part.geo.textures.size()
        ? std::filesystem::path(part.geo.textures[texture_index]).extension().string() : "";
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    const bool premultiplied = texture_index != 0xffff && extension != ".dds";
    const bool material_mix = group && version == 4 && surface.color_mix;
    const bool authored_alpha = version == 4;
    const bool cut = !material_mix && !blend &&
        (authored_alpha ? surface.cutout : version != 3 || (flags & 1));
    const float alpha_test = !cut ? 0.f : authored_alpha && group
        ? -(1.f + group->alpha_reference) / 255.f : 1.f;
    const bool foliage_coverage = cut && authored_alpha && view_id < 72 &&
        (reset_flags_ & BGFX_RESET_MSAA_MASK) && (flags & 0x02000000u) != 0;
    const bool vertex_mix = group && group->mix_vertex_color;
    const bool fixture_emitter = group && group->self_illuminated;
    const float two_sided = authored_alpha && (flags & 0x02000000u) ? 4.f : 0.f;
    const float settings[4] = {alpha_test,
                               emissive ? 2.f : blend || vertex_mix || fixture_emitter ? 1.f : 0.f,
                               two_sided + (foliage_coverage ? 2.f : blend ? 1.f : 0.f),
                               material_mix ? -2.f : vertex_mix ? -1.f :
                               baked_vertex_light(part.geo.ground_polygons) ? -3.f : 0.f};
    // The tint draw_instance builds from the placement colour, computed in
    // the same order. Its RGB rides per instance (v_tint, with u_color.rgb
    // 1), its alpha never depends on the placement and stays in u_color.
    const auto tint_of = [&](uint32_t color, float (&tint)[4]) {
      tint[0] = ((color >> 16) & 255) / 255.f;
      tint[1] = ((color >> 8) & 255) / 255.f;
      tint[2] = (color & 255) / 255.f;
      tint[3] = 1.f;
      tint[3] *= 1.f;  // opacity
      if (group && version == 3 && texture_index == 0xffff)
        for (int i = 0; i < 4; ++i) tint[i] *= group->diffuse[i];
      if (material_mix)
        for (int i = 0; i < 3; ++i) tint[i] *= group->diffuse[i];
      if (blend && !emissive && group) scx_modulate_blend(version, *group, tint, premultiplied);
      if (emissive && group && group->emission_index == 0xffff)
        for (int ch = 0; ch < 3; ++ch) tint[ch] *= group->emission_color[ch];
    };
    float first_tint[4];
    tint_of(placements[0].color, first_tint);
    const float color_uniform[4] = {1.f, 1.f, 1.f, first_tint[3]};
    const ScxTextureMap identity_map;
    const ScxTextureMap& base_uv = group ? group->base_map : identity_map;
    const ScxTextureMap& mix_uv = group ? group->mix_map : identity_map;
    const float base_xf[4] = {base_uv.scale_u, base_uv.scale_v, base_uv.offset_u, base_uv.offset_v};
    const float mix_xf[4] = {mix_uv.scale_u, mix_uv.scale_v, mix_uv.offset_u, mix_uv.offset_v};
    bgfx::TextureHandle mix = BGFX_INVALID_HANDLE;
    if (group && part.geo.position_scale == 1.f && group->mix_index != 0xffff) {
      const auto candidate = texture(group->mix_index);
      if (candidate.idx != white_tex_.idx) mix = candidate;
    }
    const bool world_mesh = !blend && !fixture_emitter;
    const auto& graphics = host_enhanced_graphics();
    const bool scene_light = world_mesh && graphics.dynamic_lighting &&
        (outdoor_lighting_ || (garage_ && lighting.set && lighting.has_direction));
    bgfx::TextureHandle mask = BGFX_INVALID_HANDLE;
    if (emissive && group && group->emission_index != 0xffff) {
      // Missing masks are black, never a full-bright wall fallback.
      mask = texture(group->emission_index);
      if (mask.idx == white_tex_.idx) return;
    }
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA;
    if (foliage_coverage) state |= BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
    if (!(flags & 0x02000000u)) state |= BGFX_STATE_CULL_CW;
    if (emissive) {
      state |= BGFX_STATE_DEPTH_TEST_GEQUAL |
               BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
    } else if (blend) {
      state |= BGFX_STATE_DEPTH_TEST_GREATER | (premultiplied
          ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA)
          : BGFX_STATE_BLEND_ALPHA);
    } else {
      state |= BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_GREATER;
    }
    for (size_t base = 0; base < count;) {
      const auto want = static_cast<uint32_t>(std::min<size_t>(kPropInstanceChunk, count - base));
      if (!bgfx::getAvailInstanceDataBuffer(want, stride)) break;
      bgfx::InstanceDataBuffer idb;
      // Another encoder may allocate in between (#270): use what was granted.
      bgfx::allocInstanceDataBuffer(&idb, want, stride);
      const uint32_t chunk = idb.num;
      if (!chunk) break;
      auto* dst = reinterpret_cast<PropInstance*>(idb.data);
      for (uint32_t i = 0; i < chunk; ++i) {
        const auto& placement = placements[base + i];
        std::memcpy(dst[i].matrix, placement.matrix, sizeof(dst[i].matrix));
        float tint[4];
        tint_of(placement.color, tint);
        dst[i].color[0] = tint[0];
        dst[i].color[1] = tint[1];
        dst[i].color[2] = tint[2];
        dst[i].color[3] = emissive ? 2.f : 1.f;  // vs_mesh_inst: 2 = overlay offset
      }
      set_uniform(u_mesh_base_uv_, base_xf);
      set_uniform(u_mesh_mix_uv_, mix_xf);
      enc()->setUniform(u_alpha_test_, settings);
      enc()->setUniform(u_color_, color_uniform);
      enc()->setTransform(identity);
      enc()->setVertexBuffer(0, part.vbh);
      enc()->setIndexBuffer(part.ibh, first, index_count);
      set_texture(0, s_tex_, tex, world_sampler());
      bind_baked_light(group, &part.tex, mix, world_mesh, scene_light ? &lighting : nullptr,
                       nullptr);
      bind_ambient_occlusion(world_mesh && ao_receiver_view(view_id));
      if (bgfx::isValid(mask)) {
        const auto& uv = group->emission_map;
        const float channels[4] = {0.f, static_cast<float>(group->base_map.channel),
                                   static_cast<float>(uv.channel), 0.f};
        const float transform[4] = {uv.scale_u, uv.scale_v, uv.offset_u, uv.offset_v};
        set_uniform(u_lightmap_, channels);
        set_uniform(u_lightmap_uv_, transform);
        set_texture(1, s_lightmap_, mask);
      }
      enc()->setState(state);
      enc()->setInstanceDataBuffer(&idb);
      submit_bound(view_id, inst_program_, index_count);
      base += chunk;
    }
  };
  if (part.geo.groups.empty())
    draw(0, static_cast<uint32_t>(part.geo.index_count), 0, 0, nullptr);
  else
    for (const auto& g : part.geo.groups)
      draw(g.index_start, g.index_count, g.texture_index, g.material_flags, &g);
}

void LibBgfxBackend::draw_prop_batches(uint16_t view_id,
                                       const std::vector<SceneBatch>& batches,
                                       bool shadow, bool conservative, bool reversed_depth,
                                       int shadow_local) {
  const bool local = shadow_local < 0 ? static_shadow_local_ : shadow_local != 0;
  const auto program = shadow ? inst_shadow_program_ : inst_program_;
  if (!bgfx::isValid(program) || batches.empty()) return;
  BindingScope binding(*this, view_id);
  float identity[16];
  bx::mtxIdentity(identity);
  const auto& graphics = host_enhanced_graphics();
  struct PropInstance {
    float matrix[16];
    float color[4];
  };
  static_assert(sizeof(PropInstance) == 80);
  const auto drawable = [&](const SceneBatch& batch) -> const ScenePart* {
    if (batch.instances.empty() || !batch.geometry) return nullptr;
    const auto found = instance_meshes_.find(batch.geometry.get());
    if (found == instance_meshes_.end()) return nullptr;
    const auto& part = found->second;
    if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh)) return nullptr;
    if (shadow && part.shadow_groups.empty()) return nullptr;
    return &part;
  };
  // One transient allocation for the whole list (#270): bgfx takes its
  // resource lock per allocation, and the worker encoders allocate too.
  const auto stride = static_cast<uint16_t>(sizeof(PropInstance));
  size_t total = 0;
  for (const auto& batch : batches)
    if (drawable(batch)) total += batch.instances.size();
  if (!total) return;
  bgfx::InstanceDataBuffer idb;
  {
    const uint32_t want = bgfx::getAvailInstanceDataBuffer(uint32_t(std::min<size_t>(total, UINT32_MAX)), stride);
    if (!want) return;
    // Another encoder may allocate in between: use what was granted.
    bgfx::allocInstanceDataBuffer(&idb, want, stride);
    if (!idb.num) return;
  }
  uint32_t cursor = 0;
  for (const auto& batch : batches) {
    const ScenePart* found_part = drawable(batch);
    if (!found_part) continue;
    const auto& part = *found_part;
    const auto& groups = shadow ? part.shadow_groups : part.geo.groups;
    const uint32_t group_count = groups.empty() ? 1u : static_cast<uint32_t>(groups.size());
    for (size_t base = 0; base < batch.instances.size();) {
      // One submit per group for up to kPropInstanceChunk placements (#262).
      // Instance data comes from the per-frame transient vertex buffer (6 MB
      // by default, 80 bytes per placement), whatever the chunk size: a
      // larger chunk only means fewer submits and fewer stride paddings.
      const auto want = static_cast<uint32_t>(std::min<size_t>(kPropInstanceChunk, batch.instances.size() - base));
      const uint32_t count = std::min(want, idb.num - cursor);
      if (!count) break;
      auto* dst = reinterpret_cast<PropInstance*>(idb.data) + cursor;
      const uint32_t first_instance = cursor;
      cursor += count;
      for (uint32_t i = 0; i < count; ++i) {
        const auto& src = batch.instances[base + i];
        std::memcpy(dst[i].matrix, src.transform.data(), sizeof(dst[i].matrix));
        if(shadow && !local)
          for(int axis=0;axis<3;++axis) dst[i].matrix[12+axis]-=shadow_origin_[axis];
        dst[i].color[0] = ((src.color >> 16) & 255) / 255.f;
        dst[i].color[1] = ((src.color >> 8) & 255) / 255.f;
        dst[i].color[2] = (src.color & 255) / 255.f;
        dst[i].color[3] = 1.f;
      }
      for (uint32_t group_index = 0; group_index < group_count; ++group_index) {
        const ScxGroup* group = groups.empty() ? nullptr : &groups[group_index];
        const uint32_t first = group ? group->index_start : 0;
        const uint32_t index_count = group ? group->index_count
                                           : static_cast<uint32_t>(part.geo.index_count);
        const uint16_t slot = group ? group->texture_index : 0;
        const uint32_t flags = group ? group->material_flags : 0;
        const auto tex = slot < part.tex.size() && bgfx::isValid(part.tex[slot])
                             ? part.tex[slot] : white_tex_;
        // The same alpha rules as a single placement (draw_instance and the
        // static shadow pass). Batched meshes have no blended sections.
        const bool v4 = part.geo.info.version == 4;
        const auto surface = group ? scx_surface(part.geo.info.version, *group) : ScxSurface{};
        const bool material_mix = surface.color_mix;
        const bool cut = surface.cutout || (part.geo.info.version != 3 && !v4);
        if (shadow) {
          const float threshold = !cut || !group ? -1.f
              : v4 ? group->alpha_reference / 255.f : .5f;
          const float params[4] = {0.f, threshold,
                                   group ? static_cast<float>(group->base_map.channel) : 0.f, 0.f};
          set_uniform(u_static_shadow_params_, params);
          set_texture(0, s_tex_, tex);
          enc()->setTransform(identity);
          enc()->setVertexBuffer(0, part.vbh);
          enc()->setIndexBuffer(part.ibh, first, index_count);
          // Like a single caster in update_static_shadow: both faces cast.
          uint64_t state = BGFX_STATE_WRITE_Z | (reversed_depth ? BGFX_STATE_DEPTH_TEST_GREATER : BGFX_STATE_DEPTH_TEST_LESS);
          if (conservative) state |= BGFX_STATE_CONSERVATIVE_RASTER;
          enc()->setState(state);
        } else {
          const float alpha_test = !cut ? 0.f : v4 && group
              ? -(1.f + group->alpha_reference) / 255.f : 1.f;
          const bool foliage_coverage = cut && v4 && view_id < 72 &&
              (reset_flags_ & BGFX_RESET_MSAA_MASK) &&
              (flags & 0x02000000u) != 0;
          const bool vertex_mix = group && group->mix_vertex_color;
          const float two_sided = v4 && (flags & 0x02000000u) ? 4.f : 0.f;
          const float settings[4] = {alpha_test, vertex_mix ? 1.f : 0.f,
                                     two_sided + (foliage_coverage ? 2.f : 0.f),
                                     material_mix ? -2.f : vertex_mix ? -1.f : 0.f};
          bgfx::TextureHandle mix = BGFX_INVALID_HANDLE;
          if (group && part.geo.position_scale == 1.f && group->mix_index != 0xffff &&
              group->mix_index < part.tex.size() && bgfx::isValid(part.tex[group->mix_index]) &&
              part.tex[group->mix_index].idx != white_tex_.idx)
            mix = part.tex[group->mix_index];
          const float white[4] = {1.f, 1.f, 1.f, 1.f};
          const ScxTextureMap identity_uv;
          const auto& base_uv = group ? group->base_map : identity_uv;
          const auto& mix_uv = group ? group->mix_map : identity_uv;
          const float base_xf[4] = {base_uv.scale_u, base_uv.scale_v, base_uv.offset_u, base_uv.offset_v};
          const float mix_xf[4] = {mix_uv.scale_u, mix_uv.scale_v, mix_uv.offset_u, mix_uv.offset_v};
          set_uniform(u_mesh_base_uv_, base_xf);
          set_uniform(u_mesh_mix_uv_, mix_xf);
          enc()->setUniform(u_alpha_test_, settings);
          // The placement colour comes per instance; ColorMixTex multiplies
          // it by the section diffuse in fs_mesh, like draw_instance's tint.
          const float mix_tint[4] = {group && material_mix ? group->diffuse[0] : 1.f,
                                     group && material_mix ? group->diffuse[1] : 1.f,
                                     group && material_mix ? group->diffuse[2] : 1.f, 1.f};
          enc()->setUniform(u_color_, material_mix ? mix_tint : white);
          enc()->setTransform(identity);
          enc()->setVertexBuffer(0, part.vbh);
          enc()->setIndexBuffer(part.ibh, first, index_count);
          set_texture(0, s_tex_, tex, world_sampler());
          bind_baked_light(group, &part.tex, mix, true,
                           outdoor_lighting_ && graphics.dynamic_lighting ? &batch.lighting : nullptr,
                           nullptr);
          // Batched props occlude in the AO prepass, so they must also
          // receive it. Otherwise fs_mesh reads whatever u_aoSampling the
          // previous draw left and stages 9/10 are unbound.
          bind_ambient_occlusion(ao_receiver_view(view_id));

          uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA |
                           BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_GREATER;
          if (foliage_coverage) state |= BGFX_STATE_BLEND_ALPHA_TO_COVERAGE;
          if (!(flags & 0x02000000u)) state |= BGFX_STATE_CULL_CW;
          enc()->setState(state);
        }
        enc()->setInstanceDataBuffer(&idb, first_instance, count);
        submit_bound(view_id, program, index_count);
      }
      base += count;
    }
  }
}

}  // namespace slrr
