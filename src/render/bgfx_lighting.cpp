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
#include <unordered_set>
#include <utility>

namespace slrr {

  void LibBgfxBackend::bind_baked_light(const ScxGroup* group,
                        const std::vector<bgfx::TextureHandle>* tex)
{
    float u[4] = {0.f, 0.f, 1.f, 0.f};
    float xf[4] = {1.f, 1.f, 0.f, 0.f};
    bgfx::TextureHandle lm = white_tex_;
    if (group) u[1] = static_cast<float>(group->base_map.channel);
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
  }
  void LibBgfxBackend::draw_instance(uint16_t view_id,const SceneInstance& instance,const float* matrix,bool preview,int pass)
{
    const auto& part=instance_meshes_.at(instance.geometry.get());
    if(!bgfx::isValid(part.vbh)||!bgfx::isValid(part.ibh))return;
    if (instance.vehicle_material && bgfx::isValid(vehicle_program_)) {
      for (const auto& group : part.geo.groups) {
        auto texture = [&](uint16_t slot) {
          return slot < part.tex.size() && bgfx::isValid(part.tex[slot]) ? part.tex[slot] : white_tex_;
        };
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
      auto tex=texture_index<part.tex.size()?part.tex[texture_index]:white_tex_;if(!bgfx::isValid(tex))tex=white_tex_;
      bool cut=!preview&&!instance.sky&&(part.geo.info.version!=3||(flags&1));
      const float settings[4]={cut?1.f:0.f,instance.sky?1.f:preview?.5f:0.f,0.f,preview&&tex.idx!=white_tex_.idx?1.f:0.f};
      uint32_t color=preview?instance.color:0xffffff;
      const float tint[4]={((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,1};
      bgfx::setUniform(u_alpha_test_,settings);bgfx::setUniform(u_color_,tint);bgfx::setTransform(matrix);
      bgfx::setVertexBuffer(0,part.vbh);bgfx::setIndexBuffer(part.ibh,first,count);bgfx::setTexture(0,s_tex_,tex);
      bind_baked_light(group,&part.tex);
      bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_MSAA|
        (instance.sky?0:BGFX_STATE_WRITE_Z|BGFX_STATE_DEPTH_TEST_LESS));bgfx::submit(view_id,program_);
    };
    if (pass != 0) return;
    if(part.geo.groups.empty())draw(0,static_cast<uint32_t>(part.geo.indices.size()),0,0,nullptr);
    else for(const auto&g:part.geo.groups)draw(g.index_start,g.index_count,g.texture_index,g.material_flags,&g);
  }

}  // namespace slrr
