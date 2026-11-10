// Sun uniforms, baked lightmaps, and the vehicle material pass.
#include "bgfx_lib_backend.hpp"

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
#include <filesystem>
#include <unordered_set>
#include <utility>

namespace slrr {

void LibBgfxBackend::draw_sun_flare(const float* view, const float* projection, uint16_t view_id) {
  if (std::none_of(instances_.begin(), instances_.end(),
                   [](const SceneInstance& instance) { return instance.sky; }))
    return;
  const auto sun = instances_.front().lighting;
  const auto& flare = sun.flare;
  if (!sun.has_direction || !flare.texture || flare.max_size <= 0.f) return;

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
  const auto texture = tex_for_osd_icon(scene_instances_texture(flare.texture));
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
    const float cos_angle = std::cos(angle), sin_angle = std::sin(angle);
    const uint16_t base = static_cast<uint16_t>(next * 4);
    for (unsigned i = 0; i < 4; ++i) {
      const float sx = i & 1 ? half_x : -half_x;
      const float sy = i & 2 ? -half_y : half_y;
      const auto p = bx::mulH({cx + (sx * cos_angle - sy * sin_angle) * aspect,
                               cy + sx * sin_angle + sy * cos_angle, .9999f}, inverse);
      vertices[base + i] = {p.x, p.y, p.z, 0.f, 0.f, 1.f,
                            u + (i & 1 ? w : 0.f), v + (i & 2 ? h : 0.f)};
    }
    constexpr uint16_t quad[] = {0, 1, 2, 1, 3, 2};
    for (unsigned i = 0; i < 6; ++i) indices[next * 6 + i] = base + quad[i];
    ++next;
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
  const float settings[] = {0.f, 1.f, 0.f, 0.f};
  bind_baked_light(nullptr, nullptr);
  bgfx::setUniform(u_color_, tint);
  bgfx::setUniform(u_alpha_test_, settings);
  bgfx::setTransform(identity);
  bgfx::setVertexBuffer(0, &vb);
  bgfx::setIndexBuffer(&ib);
  bgfx::setTexture(0, s_tex_, texture);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL |
                 BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE));
  bgfx::submit(view_id, program_);
}

  void LibBgfxBackend::bind_baked_light(const ScxGroup* group,
                        const std::vector<bgfx::TextureHandle>* tex, bgfx::TextureHandle mix,
                        bool receive_static_shadow, const SceneSun* sun)
{
    float direction[4]{};
    if (sun && sun->set && sun->has_direction) {
      direction[0] = -sun->direction[0];
      direction[1] = -sun->direction[1];
      direction[2] = sun->direction[2];
      direction[3] = 1.f;
      const float ambient[4] = {sun->ambient[0], sun->ambient[1], sun->ambient[2], 0.f};
      const float diffuse[4] = {sun->diffuse[0], sun->diffuse[1], sun->diffuse[2], 0.f};
      bgfx::setUniform(u_sun_amb_, ambient);
      bgfx::setUniform(u_sun_diff_, diffuse);
    }
    bgfx::setUniform(u_sun_dir_, direction);
    float u[4] = {0.f, 0.f, 1.f, 0.f};
    float xf[4] = {1.f, 1.f, 0.f, 0.f};
    bgfx::TextureHandle lm = white_tex_;
    if (group) u[1] = static_cast<float>(group->base_map.channel);
    if (group && bgfx::isValid(mix))
      u[3] = 1.f + static_cast<float>(group->mix_map.channel);
    if (group && tex && group->lightmap_index != 0xffff &&
        group->lightmap_index < tex->size() &&
        bgfx::isValid((*tex)[group->lightmap_index])) {
      lm = (*tex)[group->lightmap_index];
      u[0] = 1.f;
      u[2] = static_cast<float>(group->lightmap_map.channel);
      xf[0] = group->lightmap_map.scale_u;
      xf[1] = group->lightmap_map.scale_v;
      xf[2] = group->lightmap_map.offset_u;
      xf[3] = group->lightmap_map.offset_v;
    }
    bgfx::setUniform(u_lightmap_, u);
    bgfx::setUniform(u_lightmap_uv_, xf);
    bgfx::setTexture(1, s_lightmap_, lm);
    bgfx::setTexture(2, s_mesh_mix_, bgfx::isValid(mix) ? mix : white_tex_);
    const bool receive = receive_static_shadow && static_shadow_active_ &&
                         debug_overlay_.render_options().map_shadows;
    const float params[4] = {receive ? 1.f : 0.f,
                             receive ? 1.f / static_shadow_size_ : 0.f, 0.00045f, 0.f};
    bgfx::setUniform(u_static_shadow_params_, params);
    if (receive) {
      const float eye[4] = {static_shadow_eye_[0], static_shadow_eye_[1],
                            static_shadow_eye_[2], static_shadow_distance_};
      bgfx::setUniform(u_static_shadow_eye_, eye);
      bgfx::setUniform(u_static_shadow_mtx_, static_shadow_matrix_.data());
    }
    bgfx::setTexture(3, s_static_shadow_, receive ? static_shadow_tex_ : white_tex_);
  }
  void LibBgfxBackend::draw_instance(uint16_t view_id,const SceneInstance& instance,const float* matrix,bool preview,int pass)
{
    const auto& part=instance_meshes_.at(instance.geometry.get());
    if(!bgfx::isValid(part.vbh)||!bgfx::isValid(part.ibh))return;
    const auto texture = [&](uint16_t slot) {
      for (const auto& [index, path] : instance.texture_replacements)
        if (slot == index && !path.empty()) {
          const auto replacement = tex_for_osd_icon(path);
          if (replacement.idx != white_tex_.idx) return replacement;
        }
      return slot < part.tex.size() && bgfx::isValid(part.tex[slot]) ? part.tex[slot] : white_tex_;
    };
    if (instance.vehicle_material && bgfx::isValid(vehicle_program_)) {
      for (const auto& group : part.geo.groups) {
        auto base = texture(group.texture_index);
        const bool painted = instance.paint_slot >= 0 && group.texture_index == instance.paint_slot;
        if (painted && !instance.paint_texture.empty()) base = tex_for_osd_icon(instance.paint_texture);
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
        const bool transparent = (group.material_flags & 0x40000000u) != 0;
        if (transparent != (pass == 1)) continue;
        const uint32_t kind_id = group.material_type;
        const bool glass = transparent && kind_id == 0x33u;
        const bool color_mix_env = kind_id == 0x0bu || kind_id == 0x13u || kind_id == 0x23u ||
                                   kind_id == 0x0fu || kind_id == 0x17u || kind_id == 0x27u;
        const bool color_mix = color_mix_env || kind_id == 0x03u || kind_id == 0x1bu ||
                               kind_id == 0x07u || kind_id == 0x1fu;
        const bool tex_missing = group.texture_index != 0xffff &&
            (group.texture_index >= part.tex.size() || !bgfx::isValid(part.tex[group.texture_index]));
        const bool chrome = !glass && kind_id == 0x33u && group.texture_index == 0xffff;
        const bool bare_cap = !glass && !chrome && tex_missing && group.material_type == 0x0cu &&
                              group.reflection_index != 0xffff;
        // A headlight lens names a reflection slot the render leaves empty.
        // Cabin glass already asks for a generated spheremap. Only the lens
        // keeps the stock alpha, so the housing texture shows through.
        const bool lens = glass && env.idx == white_tex_.idx && !generated;
        // A missing reflection slot is the global envmap, and the exe
        // builds its UVs from the reflection vector, not the mesh.
        if (env.idx == white_tex_.idx &&
            (painted || generated || chrome || bare_cap || glass)) {
          auto path = scene_instances_envmap();
          if(!path.empty()) env = tex_for_osd_icon(path);
          generated = true;
        }
        const float kind = glass ? 3.f : color_mix_env ? 5.f : color_mix ? 4.f
                                : (chrome || bare_cap) ? 2.f : painted ? 1.f : 0.f;
        const float layers[4] = {mixed?1.f:0.f, env.idx!=white_tex_.idx?strength:0.f, group.material_type==0x15&&mixed?1.f:0.f, kind};
        const float channels[4] = {static_cast<float>(group.base_map.channel),static_cast<float>(group.mix_map.channel),static_cast<float>(group.reflection_map.channel),lens?2.f:generated?1.f:0.f};
        auto map = [&](bgfx::UniformHandle uniform, const ScxTextureMap& uv) {
          const float value[4]={uv.scale_u,uv.scale_v,uv.offset_u,uv.offset_v}; bgfx::setUniform(uniform,value);
        };
        bgfx::setUniform(u_layers_,layers); bgfx::setUniform(u_channels_,channels);
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
        const SceneSun sun = instance.lighting;
        const float sun_d[4] = {sun.diffuse[0], sun.diffuse[1], sun.diffuse[2],
                                sun.set ? 1.f : 0.f};
        const float sun_a[4] = {sun.ambient[0], sun.ambient[1], sun.ambient[2], 0.f};
        const float sun_s[4] = {sun.specular[0], sun.specular[1], sun.specular[2],
                                sun.set ? 1.f : 0.f};
        // Game direction is where the light travels. Toward it, with Z
        // flipped into the same space as the rendered vertices.
        const float sun_r[4] = {-sun.direction[0], -sun.direction[1],
                                sun.direction[2], sun.has_direction ? 1.f : 0.f};
        bgfx::setUniform(u_sun_diff_, sun_d);
        bgfx::setUniform(u_sun_amb_, sun_a);
        bgfx::setUniform(u_sun_spec_, sun_s);
        bgfx::setUniform(u_sun_dir_, sun_r);
        bgfx::setUniform(u_diffuse_,diffuse);
        const float spec[4] = {group.specular[0], group.specular[1], group.specular[2],
                               group.shininess > 0.f ? group.shininess : 1.f};
        bgfx::setUniform(u_spec_, spec);
        map(u_base_uv_,group.base_map);map(u_mix_uv_,group.mix_map);map(u_env_uv_,group.reflection_map);
        bgfx::TextureHandle bake = white_tex_;
        float bake_u[4] = {0.f, static_cast<float>(group.base_map.channel), 1.f, 0.f};
        float bake_xf[4] = {1.f, 1.f, 0.f, 0.f};
        if (group.lightmap_index != 0xffff &&
            group.lightmap_index < part.tex.size() &&
            bgfx::isValid(part.tex[group.lightmap_index])) {
          bake = part.tex[group.lightmap_index];
          bake_u[0] = 1.f;
          bake_u[2] = static_cast<float>(group.lightmap_map.channel);
          bake_xf[0] = group.lightmap_map.scale_u;
          bake_xf[1] = group.lightmap_map.scale_v;
          bake_xf[2] = group.lightmap_map.offset_u;
          bake_xf[3] = group.lightmap_map.offset_v;
        }
        if (transparent && color_mix) bake_u[3] = 1.f;
        // A switched-on lamp is the glow texture, not a surface in shade.
        if (instance.light_flags) bake_u[0] = 1.f;
        bgfx::setUniform(u_lightmap_, bake_u);
        bgfx::setUniform(u_lightmap_uv_, bake_xf);
        bgfx::setTexture(0,s_tex_,base);bgfx::setTexture(1,s_mix_,mix);
        // Global environment textures share the image cache with OSD, but
        // material UV transforms need the scene's wrapping sampler here.
        bgfx::setTexture(2, s_env_, env, BGFX_SAMPLER_NONE);
        bgfx::setTexture(3,s_bake_,bake);
        bgfx::setTransform(matrix);bgfx::setVertexBuffer(0,part.vbh);bgfx::setIndexBuffer(part.ibh,group.index_start,group.index_count);
        uint64_t state=BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_MSAA|BGFX_STATE_DEPTH_TEST_LESS;
        if (glass)
          state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
        else if (transparent || group.diffuse[3] < .99f)
          state |= BGFX_STATE_BLEND_ALPHA;
        else
          state |= BGFX_STATE_WRITE_Z;
        bgfx::setState(state);bgfx::submit(view_id,vehicle_program_);
      }
      return;
    }
    auto draw=[&](uint32_t first,uint32_t count,uint16_t texture_index,uint32_t flags,const ScxGroup* group){
      // SCX3 alpha-blended sprites, including map markers, belong above
      // the opaque tiles. Their JPEG/PNG alpha is already premultiplied.
      // SCX4 glass requests texture alpha in the material word. The
      // transparent function bit alone also appears on opaque rail frames.
      const bool glass = group && !preview && part.geo.info.version == 4 &&
          (flags & 0x40000000u) && (group->material_type & 0x04u);
      const bool blend = !instance.sky && (glass ||
          (part.geo.info.version == 3 && (flags & 0x101u) == 0x101u));
      if (pass != int(blend)) return;
      const auto tex = texture(texture_index);
      // SCX4 storefronts use texture alpha to mix their paint colour with
      // the detail texture. Low-alpha texels are solid wall, not holes.
      const bool material_mix = group && instance.cull_backfaces &&
          part.geo.info.version == 4 && group->material_type == 0x03u;
      const bool authored_alpha = instance.cull_backfaces && part.geo.info.version == 4;
      const bool cut = !material_mix && !blend && !preview && !instance.sky &&
          (authored_alpha ? (flags & 0x20000000u) != 0
                          : part.geo.info.version != 3 || (flags & 1));
      // V4 map foliage specifies GREATER with an 8-bit reference (default 0x38).
      const float alpha_test = !cut ? 0.f : authored_alpha && group
          ? -(1.f + group->alpha_reference) / 255.f : 1.f;
      const bool vertex_mix = group && group->mix_vertex_color;
      const float settings[4] = {alpha_test, instance.sky || blend || vertex_mix ? 1.f : preview ? .5f : 0.f,
                                 blend ? 1.f : 0.f, material_mix ? -2.f : vertex_mix ? -1.f :
                                 outdoor_lighting_ && !preview && part.geo.ground_polygons ? -3.f :
                                 !blend && preview && tex.idx != white_tex_.idx ? 1.f : 0.f};
      const uint32_t color = preview || instance.cull_backfaces ? instance.color : 0xffffffu;
      float tint[4]={((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,1};
      if (material_mix)
        for (int i = 0; i < 3; ++i) tint[i] *= group->diffuse[i];
      bgfx::setUniform(u_alpha_test_,settings);bgfx::setUniform(u_color_,tint);bgfx::setTransform(matrix);
      bgfx::setVertexBuffer(0,part.vbh);bgfx::setIndexBuffer(part.ibh,first,count);
      bgfx::setTexture(0, s_tex_, tex, BGFX_SAMPLER_NONE);
      bgfx::TextureHandle mix = BGFX_INVALID_HANDLE;
      if (group && part.geo.position_scale == 1.f && group->mix_index != 0xffff) {
        const auto candidate = texture(group->mix_index);
        if (candidate.idx != white_tex_.idx) mix = candidate;
      }
      const bool world_mesh = !preview && !instance.sky && !blend;
      bind_baked_light(group, &part.tex, mix, world_mesh,
                       world_mesh && outdoor_lighting_ ? &instance.lighting : nullptr);
      uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA;
      if (instance.cull_backfaces && !(flags & 0x02000000u))
        state |= BGFX_STATE_CULL_CW;
      if (blend) {
        std::string extension = texture_index < part.geo.textures.size()
            ? std::filesystem::path(part.geo.textures[texture_index]).extension().string() : "";
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        state |= BGFX_STATE_DEPTH_TEST_LESS | (extension == ".dds" ? BGFX_STATE_BLEND_ALPHA
            : BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA));
      } else if (!instance.sky) {
        state |= BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
      }
      bgfx::setState(state);
      bgfx::submit(view_id, program_);
    };
    if(part.geo.groups.empty())draw(0,static_cast<uint32_t>(part.geo.indices.size()),0,0,nullptr);
    else for(const auto&g:part.geo.groups)draw(g.index_start,g.index_count,g.texture_index,g.material_flags,&g);
  }

}  // namespace slrr
