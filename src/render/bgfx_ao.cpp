#include "bgfx_lib_backend.hpp"
#include <algorithm>
#include <bx/math.h>
#include <cmath>
namespace slrr {
void LibBgfxBackend::destroy_ao_targets() {
  for (auto *fb : {&ao_depth_fb_, &ao_raw_fb_, &ao_filter_fb_, &ao_geometry_fb_}) {
    if (bgfx::isValid(*fb))
      bgfx::destroy(*fb);
    *fb = BGFX_INVALID_HANDLE;
  }
  ao_depth_tex_ = ao_raw_tex_ = ao_filter_tex_ = BGFX_INVALID_HANDLE;
  ao_linear_tex_ = ao_normal_tex_ = BGFX_INVALID_HANDLE;
  ao_width_ = ao_height_ = ao_screen_width_ = ao_screen_height_ = 0;
  ao_active_ = false;
  ao_world_view_ = UINT16_MAX;
}
void LibBgfxBackend::bind_ambient_occlusion(bool enabled) {
  const auto &options = host_enhanced_graphics();
  const bool active = enabled && ao_active_ && options.ao_method > 0;
  const float sampling[4] = {
      active ? std::clamp(options.ao_strength, 0, 200) * .01f : 0.f,
      ao_screen_width_ ? 1.f / ao_screen_width_ : 0.f,
      ao_screen_height_ ? 1.f / ao_screen_height_ : 0.f, 0};
  const float field[4] = {ao_width_ ? 1.f / ao_width_ : 0.f,
                          ao_height_ ? 1.f / ao_height_ : 0.f,
                          std::clamp(options.ao_radius, 25, 300) * .01f, 0};
  set_uniform(u_ao_sampling_, sampling);
  set_uniform(u_ao_field_size_, field);
  set_uniform(u_ao_projection_, ao_projection_.data());
  constexpr uint32_t point = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
                             BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
  set_texture(9, s_ao_field_, active ? ao_raw_tex_ : white_tex_, point);
  set_texture(10, s_ao_depth_, active ? ao_depth_tex_ : depth_white_,
                   point);
}
void LibBgfxBackend::update_ambient_occlusion(const float *view,
                                              const float *projection,
                                              uint16_t width, uint16_t height,
                                              uint16_t world_view) {
  const auto &options = host_enhanced_graphics();
  if (options.ao_method <= 0 || options.ao_strength <= 0 || !width || !height ||
      !bgfx::isValid(ao_program_) || !render_debug().ambient_occlusion) {
    destroy_ao_targets();
    return;
  }
  const int quality = std::clamp(options.ao_quality, 0, 2);
  // Every quality runs at half resolution (#248); High spends the saving on
  // more slices/steps (fs_ao.sc) and the depth-aware 2x2 upsample in
  // ambient_occlusion.sh rebuilds full-resolution edges. The debug scale
  // (#243) multiplies the half size: 2 is full resolution, for comparison.
  const float scale = std::clamp(render_debug().ao_resolution_scale, .25f, 2.f);
  const uint16_t aw = uint16_t(std::clamp(((width + 1) / 2) * scale, 16.f, float(width)));
  const uint16_t ah = uint16_t(std::clamp(((height + 1) / 2) * scale, 16.f, float(height)));
  if (ao_screen_width_ != width || ao_screen_height_ != height ||
      ao_width_ != aw || ao_height_ != ah)
    destroy_ao_targets();
  auto target = [&](bgfx::TextureFormat::Enum format, uint16_t w, uint16_t h,
                    bgfx::TextureHandle &texture, bgfx::FrameBufferHandle &fb) {
    texture = bgfx::createTexture2D(w, h, false, 1, format,
                                    BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP |
                                        BGFX_SAMPLER_V_CLAMP);
    if (!bgfx::isValid(texture))
      return false;
    fb = bgfx::createFrameBuffer(1, &texture, true);
    if (bgfx::isValid(fb))
      return true;
    bgfx::destroy(texture);
    texture = BGFX_INVALID_HANDLE;
    return false;
  };
  // Linear depth + normal at AO resolution (#256), one framebuffer.
  auto geometry_target = [&] {
    const uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
                           BGFX_SAMPLER_POINT;
    ao_linear_tex_ = bgfx::createTexture2D(aw, ah, false, 1, bgfx::TextureFormat::R32F, flags);
    ao_normal_tex_ = bgfx::createTexture2D(aw, ah, false, 1, bgfx::TextureFormat::RGBA32F, flags);
    if (!bgfx::isValid(ao_linear_tex_) || !bgfx::isValid(ao_normal_tex_)) return false;
    const bgfx::TextureHandle attachments[] = {ao_linear_tex_, ao_normal_tex_};
    ao_geometry_fb_ = bgfx::createFrameBuffer(2, attachments, true);
    return bgfx::isValid(ao_geometry_fb_);
  };
  if (!bgfx::isValid(ao_depth_fb_)) {
    if (!target(bgfx::TextureFormat::D32F, width, height, ao_depth_tex_,
                ao_depth_fb_) ||
        !target(bgfx::TextureFormat::R8, aw, ah, ao_raw_tex_, ao_raw_fb_) ||
        !target(bgfx::TextureFormat::R8, aw, ah, ao_filter_tex_,
                ao_filter_fb_) ||
        !geometry_target()) {
      destroy_ao_targets();
      return;
    }
    ao_screen_width_ = width;
    ao_screen_height_ = height;
    ao_width_ = aw;
    ao_height_ = ah;
  }
  const auto *caps = bgfx::getCaps();
  float depth_projection[16];
  std::copy(projection, projection + 16, depth_projection);
  for (int i : {2, 6, 10, 14})
    depth_projection[i] = caps->homogeneousDepth
                              ? -projection[i]
                              : projection[i + 1] - projection[i];
  ao_projection_ = {depth_projection[10], depth_projection[14],
                    caps->homogeneousDepth ? 1.f : 0.f,
                    caps->originBottomLeft ? 1.f : 0.f};
  ao_world_view_ = world_view;
  ao_active_ = true;
  // A camera-matched, single-sample reversed-Z depth prepass. No swapchain
  // read, MSAA depth resolve, post-darkening of direct light or UI processing.
  bgfx::setViewFrameBuffer(140, ao_depth_fb_);
  bgfx::setViewRect(140, 0, 0, width, height);
  bgfx::setViewClear(140, BGFX_CLEAR_DEPTH, 0, 0.f);
  bgfx::setViewTransform(140, view, depth_projection);
  // The prepass draws are one job (#270): it may run on a worker encoder.
  queue_or_run([this] {
  enc()->touch(140);
  const float no_apertures[4]{};
  enc()->setUniform(u_alpha_test_, no_apertures);
  enc()->setUniform(u_aperture_params_, no_apertures);
  auto draw = [&](const ScenePart &part, const float *matrix,
                  const SceneInstance *object) {
    if (!bgfx::isValid(part.vbh) || !bgfx::isValid(part.ibh))
      return;
    const bool skin = object && object->gpu_skin && !object->skin.empty() &&
                      bgfx::isValid(part.skin_vbh);
    if (object && object->gpu_skin && !skin)
      return;
    for (const auto &group : part.shadow_groups) {
      const auto surface = scx_surface(part.geo.info.version, group);
      const float threshold = !surface.cutout ? -1.f
                              : part.geo.info.version == 4
                                  ? group.alpha_reference / 255.f
                                  : .5f;
      const float params[4] = {0, threshold, float(group.base_map.channel), 0};
      set_uniform(u_static_shadow_params_, params);
      enc()->setTransform(matrix);
      if (skin)
        enc()->setUniform(u_skin_bones_, object->skin.data(), 48);
      enc()->setVertexBuffer(0, skin ? part.skin_vbh : part.vbh);
      enc()->setIndexBuffer(part.ibh, group.index_start, group.index_count);
      const auto slot = group.texture_index;
      enc()->setTexture(0, s_tex_,
                       slot < part.tex.size() && bgfx::isValid(part.tex[slot])
                           ? part.tex[slot]
                           : white_tex_);
      enc()->setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_GREATER);
      submit_counted(140, skin ? ao_skin_depth_program_ : static_shadow_program_);
    }
  };
  for (const auto &object : instances_) {
    if (!object.geometry || object.sky || object.emissive_overlay ||
        object.opacity < .99f)
      continue;
    const auto found = instance_meshes_.find(object.geometry.get());
    if (found != instance_meshes_.end())
      draw(found->second, object.transform.data(), &object);
  }
  float identity[16];
  bx::mtxIdentity(identity);
  if (garage_)
    for (const auto &part : parts_)
      draw(part, identity, nullptr);
  // Local (camera-relative) instance matrices, without toggling
  // static_shadow_local_, which the world's receivers read meanwhile.
  draw_prop_batches(140, prop_batches_, true, false, true, 1);
  });
  const float screen[4] = {1.f / width, 1.f / height, projection[0],
                           projection[5]};
  const float config[4] = {float(std::clamp(options.ao_method, 1, 2)),
                           float(quality),
                           std::clamp(options.ao_radius, 25, 300) * .01f,
                           render_debug().ao_reference ? 1.f : 0.f};
  constexpr uint32_t sampler = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
                               BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
  // View 141: geometry (shader pass 3); views 142-144: AO and the two filters.
  for (uint16_t step = 0; step < 4; ++step) {
    const uint16_t id = 141 + step;
    const bool geometry = step == 0;
    const uint16_t pass = geometry ? 3 : uint16_t(step - 1);
    const auto fb = geometry ? ao_geometry_fb_ : pass == 1 ? ao_filter_fb_ : ao_raw_fb_;
    bgfx::setViewFrameBuffer(id, fb);
    bgfx::setViewRect(id, 0, 0, aw, ah);
    bgfx::setViewClear(id, BGFX_CLEAR_COLOR, geometry ? 0x00000000 : 0xffffffff);
    enc()->touch(id);
    if (bgfx::getAvailTransientVertexBuffer(3, layout_) < 3) {
      ao_active_ = false;
      return;
    }
    bgfx::TransientVertexBuffer vb;
    bgfx::allocTransientVertexBuffer(&vb, 3, layout_);
    auto *vertices = reinterpret_cast<MeshVertex *>(vb.data);
    const float v0 = caps->originBottomLeft ? 0.f : 1.f,
                v1 = caps->originBottomLeft ? 2.f : -1.f;
    vertices[0] = {-1, -1, 0, 0, 0, 1, 0, v0};
    vertices[1] = {3, -1, 0, 0, 0, 1, 2, v0};
    vertices[2] = {-1, 3, 0, 0, 0, 1, 0, v1};
    const float params[4] = {float(pass), 1.f / aw, 1.f / ah, 0};
    enc()->setUniform(u_ao_screen_, screen);
    set_uniform(u_ao_projection_, ao_projection_.data());
    enc()->setUniform(u_ao_config_, config);
    enc()->setUniform(u_ao_params_, params);
    enc()->setTexture(0, s_ao_depth_, ao_depth_tex_, sampler);
    enc()->setTexture(1, s_ao_input_,
                     pass == 1   ? ao_raw_tex_
                     : pass == 2 ? ao_filter_tex_
                                 : white_tex_,
                     sampler);
    // The geometry pass writes these; it must not read them.
    enc()->setTexture(2, s_ao_linear_, geometry ? white_tex_ : ao_linear_tex_, sampler);
    enc()->setTexture(3, s_ao_normal_, geometry ? white_tex_ : ao_normal_tex_, sampler);
    enc()->setVertexBuffer(0, &vb);
    enc()->setState(geometry ? BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A : BGFX_STATE_WRITE_RGB);
    submit_counted(id, ao_program_);
  }
}
} // namespace slrr
