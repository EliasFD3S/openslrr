// Menu, garage and FMV overlay.
#include "bgfx_lib_backend.hpp"
#include "slrr/frame_profiler.hpp"

#include "slrr/part_runtime.hpp"
#include "slrr/vehicle_hud_damage.hpp"
#include "slrr/dds_image.hpp"
#include "slrr/paint_texture.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/ptx_image.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/video_fmv.hpp"

#include <bx/math.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <unordered_set>
#include <utility>

namespace slrr {

#ifdef _WIN32
  bgfx::TextureHandle LibBgfxBackend::tex_for_osd_icon(const std::string& path)
{
    if (path.empty()) return white_tex_;
    if (const uint32_t paint = paint_texture_from_path(path))
      return paint_texture_handle(paint);
    auto it = osd_tex_.find(path);
    if (it != osd_tex_.end()) return it->second;
    DdsImage img = load_texture_file(path);
    bgfx::TextureHandle th = BGFX_INVALID_HANDLE;
    if (img.ok) {
      bgfx::TextureFormat::Enum fmt = bgfx::TextureFormat::Unknown;
      if (img.format == DdsFormat::DXT1) fmt = bgfx::TextureFormat::BC1;
      else if (img.format == DdsFormat::DXT3) fmt = bgfx::TextureFormat::BC2;
      else if (img.format == DdsFormat::DXT5) fmt = bgfx::TextureFormat::BC3;
      else if (img.format == DdsFormat::RGBA8) {
        std::string ext=path.size()>=4?path.substr(path.size()-4):"";
        for(char& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        fmt=ext==".dds"?bgfx::TextureFormat::BGRA8:bgfx::TextureFormat::RGBA8;
      }
      if (fmt != bgfx::TextureFormat::Unknown) {
        const bgfx::Memory* mem =
            bgfx::copy(img.data.data(), img.mip0_size);
        th = bgfx::createTexture2D(
            static_cast<uint16_t>(img.width),
            static_cast<uint16_t>(img.height), false, 1, fmt,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem);
      }
    }
    if (!bgfx::isValid(th)) {
      std::printf("[bgfx] OSD icon miss %s (%s)\n", path.c_str(),
                  img.error.c_str());
      th = white_tex_;
    } else {
      std::printf("[bgfx] OSD icon %ux%u %s\n", img.width, img.height,
                  path.c_str());
    }
    osd_tex_[path] = th;
    return th;
  }
#endif
#ifdef _WIN32
  bgfx::TextureHandle LibBgfxBackend::paint_texture_handle(uint32_t id)
{
    const PaintImage* image = paint_texture_image(id);
    if (!image || !image->valid() || image->width > 4096 || image->height > 4096)
      return white_tex_;
    auto& gpu = paint_tex_[id];
    const uint32_t revision = paint_texture_revision(id);
    if (bgfx::isValid(gpu.handle) && gpu.revision == revision) return gpu.handle;
    const auto pow2 = [](uint32_t v) { return v && !(v & (v - 1)); };
    const bool mips = pow2(image->width) && pow2(image->height);
    if (bgfx::isValid(gpu.handle) &&
        (gpu.width != image->width || gpu.height != image->height || gpu.mips != mips)) {
      bgfx::destroy(gpu.handle);
      gpu.handle = BGFX_INVALID_HANDLE;
    }
    if (!bgfx::isValid(gpu.handle)) {
      gpu.handle = bgfx::createTexture2D(static_cast<uint16_t>(image->width),
                                         static_cast<uint16_t>(image->height), mips, 1,
                                         bgfx::TextureFormat::RGBA8,
                                         BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, nullptr);
      if (!bgfx::isValid(gpu.handle)) {
        paint_tex_.erase(id);
        return white_tex_;
      }
      gpu.width = image->width;
      gpu.height = image->height;
      gpu.mips = mips;
    }
    // Box-filtered mip chain; a painted panel is mostly seen smaller than
    // its texture, and a texture without mips shimmers.
    std::vector<uint8_t> level = image->rgba;
    // Match PNG/PTX uploads: OSD uses premultiplied blending. Painted body
    // textures are opaque; restored decal resources may have transparent RGB.
    for (size_t i = 0; i < level.size(); i += 4)
      for (int c = 0; c < 3; ++c)
        level[i+c] = static_cast<uint8_t>((level[i+c] * level[i+3] + 127) / 255);
    uint32_t w = image->width, h = image->height;
    for (uint8_t mip = 0;; ++mip) {
      bgfx::updateTexture2D(gpu.handle, 0, mip, 0, 0, static_cast<uint16_t>(w),
                            static_cast<uint16_t>(h),
                            bgfx::copy(level.data(), static_cast<uint32_t>(level.size())));
      if (!mips || (w == 1 && h == 1)) break;
      const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
      std::vector<uint8_t> next(size_t(nw) * nh * 4);
      for (uint32_t y = 0; y < nh; ++y)
        for (uint32_t x = 0; x < nw; ++x)
          for (int c = 0; c < 4; ++c) {
            const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
            const uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
            const uint32_t sum = level[(size_t(y0) * w + x0) * 4 + c] + level[(size_t(y0) * w + x1) * 4 + c] +
                                 level[(size_t(y1) * w + x0) * 4 + c] + level[(size_t(y1) * w + x1) * 4 + c];
            next[(size_t(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
          }
      level.swap(next);
      w = nw;
      h = nh;
    }
    gpu.revision = revision;
    return gpu.handle;
  }

  void LibBgfxBackend::release_paint_textures(bool all)
{
    if (all) paint_texture_take_released();
    for (auto it = paint_tex_.begin(); it != paint_tex_.end();) {
      if (all || !paint_texture_exists(it->first)) {
        if (bgfx::isValid(it->second.handle)) bgfx::destroy(it->second.handle);
        it = paint_tex_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void LibBgfxBackend::draw_osd_quad(const OsdWidget& w, float cr, float cg,
                     float cb, float ca)
{
    MeshVertex v[4];
    const float x0 = w.x - w.half_w, x1 = w.x + w.half_w;
    const float y0 = w.y - w.half_h, y1 = w.y + w.half_h;
    // FE_upper_stripe: chrome at V=1 (bottom → under icons). alkatreszcsik:
    // chrome at V=0 (top of lower bar). Stock UV, no V-flip.
    const float v0 = 0.f;
    const float v1 = 1.f;
    auto setv = [](MeshVertex& o, float x, float y, float u, float vv) {
      o.x = x;
      o.y = y;
      o.z = 0.f;
      o.nx = 0;
      o.ny = 0;
      o.nz = 1;
      o.u = u;
      o.v = vv;
    };
    setv(v[0], x0, y0, 0, v0);
    setv(v[1], x1, y0, 1, v0);
    setv(v[2], x1, y1, 1, v1);
    setv(v[3], x0, y1, 0, v1);
    const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    if (bgfx::getAvailTransientVertexBuffer(4, layout_) < 4 ||
        bgfx::getAvailTransientIndexBuffer(6) < 6)
      return;
    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientIndexBuffer tib;
    bgfx::allocTransientVertexBuffer(&tvb, 4, layout_);
    bgfx::allocTransientIndexBuffer(&tib, 6);
    std::memcpy(tvb.data, v, sizeof(v));
    std::memcpy(tib.data, idx, sizeof(idx));
    float mtx[16];
    bx::mtxIdentity(mtx);
    enc()->setTransform(mtx);
    enc()->setVertexBuffer(0, &tvb);
    enc()->setIndexBuffer(&tib);
    enc()->setTexture(0, s_tex_, tex_for_osd_icon(w.icon_path));
    bind_baked_light(nullptr, nullptr);
    // Icons: cutout low-alpha (kills green JPEG plate).
    // OpeningPic.ptx is a JPEG with no alpha. Blending with tex.a hides it
    // when the channel is 0. Other plates (GPS frame over FMV) keep tex.a.
    // Button icons cut out the JPEG plate. The modal darken and dialog
    // plates keep tex.a so the scene stays visible through them.
    bool splash_plate = false;
    if (w.background && !w.icon_path.empty()) {
      std::string low = w.icon_path;
      for (char& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      splash_plate = low.find("opening") != std::string::npos ||
                     low.find("loading") != std::string::npos;
    }
    // PTX RGB is premultiplied. Discarding a<0.5 chewed the icon edges.
    // tex.a keeps the antialiasing. Splash/loading have no usable alpha.
    const float u[4] = {0.f, 1.f, splash_plate ? 0.f : 1.f, 0.f};
    enc()->setUniform(u_alpha_test_, u);
    const float tint[4] = {cr, cg, cb, ca};
    enc()->setUniform(u_color_, tint);
    // PTX RGB is already multiplied by alpha. SRC_ALPHA would darken the
    // menu plate a second time (rgb * a * a). Premultiplied blend keeps
    // the gold frame and lets prime.avi through the transparent center.
    enc()->setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,
                                         BGFX_STATE_BLEND_INV_SRC_ALPHA) |
                   BGFX_STATE_MSAA);
    submit_counted(osd_view_, program_);
  }
#endif
#ifdef _WIN32
  bgfx::TextureHandle LibBgfxBackend::tex_for_font(void* font_key)
{
    if (!font_key) return white_tex_;
    const SlrrFont* font = slrr_font_get(font_key);
    if (!font || font->rgba.empty() || font->atlas_w <= 0 || font->atlas_h <= 0)
      return white_tex_;
    auto it = font_tex_.find(font->cache_key);
    if (it != font_tex_.end()) return it->second;
    const bgfx::Memory* mem = bgfx::copy(
        font->rgba.data(),
        static_cast<uint32_t>(font->rgba.size()));
    bgfx::TextureHandle th = bgfx::createTexture2D(
        static_cast<uint16_t>(font->atlas_w),
        static_cast<uint16_t>(font->atlas_h), false, 1,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
            (font->distance_field ? uint64_t(0) :
             BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT),
        mem);
    if (!bgfx::isValid(th)) return white_tex_;
    font_tex_[font->cache_key] = th;
    return th;
  }
#endif
#ifdef _WIN32
  void LibBgfxBackend::draw_osd_texts(int32_t only_layer, bool filter_layer, bool behind_ui)
{
    const auto& texts = slrr_osd_texts();
    if (texts.empty() || width_ <= 0 || height_ <= 0) return;
    // Text-menu hover: stock HL_LABELCOLOR → COLOR_SELECTED (full white).
    const OsdWidget* hover = osd_hover_widget();

    for (const SlrrOsdText& t : texts) {
      if (!t.visible || !t.font_key || t.text.empty()) continue;
      if (t.behind_ui != behind_ui) continue;
      if (filter_layer && t.layer != only_layer) continue;
      const SlrrFont* font = slrr_font_get(t.font_key);
      if (!font || !font->ready) continue;
      uint32_t argb = t.color;
      const bool glow = hover && hover->label_src == t.text_key;
      // changeSelection2(-1) focuses the first row (NEW CAREER) in yellow
      // even when the pointer is elsewhere. That color is the hover state.
      if (argb == 0xFFFFCB00u && !glow) argb = 0xFFC0C0C0u;
      if (glow) argb = 0xFFFFFFFFu;
      draw_osd_line(t.font_key, t.text, t.x, t.y, t.align, argb,
                    t.custom_effects ? &t.effects : nullptr);
    }
    const float white_c[4] = {1.f, 1.f, 1.f, 1.f};
    enc()->setUniform(u_color_, white_c);
  }
#endif
#ifdef _WIN32
  void LibBgfxBackend::draw_tooltip()
{
    if (width_ <= 0 || height_ <= 0) return;
    // The city button under the cursor still owns its tip while the dealer
    // picture is up. That plate is the black square on the loading screen.
    if (osd_loading_dialog() || !osd_loading_overlay().empty()) {
      tip_hover_ = -2;
      return;
    }
    if (host_input().rmb_down) {
      tip_hover_ = -2;
      return;
    }
    const int32_t idx = osd_hover_index();
    const OsdWidget* h = osd_hover_widget();
    if (!h) {
      const OsdWidget* raw = osd_widget_at(idx);
      if (raw && raw->disabled && !raw->hidden) h = raw;
    }
    const bool showable = h && !h->row_label && !h->string_input &&
                          !h->background && !h->tip.empty();
    const float now = gameref_now();
    if (!showable || idx != tip_hover_) {
      tip_hover_ = showable ? idx : -2;
      tip_since_ = now;
      return;
    }
    if (now - tip_since_ < 1.f) return;

    void* font_key = slrr_font_ui_small();
    if (!font_key) return;
    if (!slrr_font_ready(font_key) && !tip_font_tried_) {
      tip_font_tried_ = true;
      slrr_font_load(font_key, "slii11");
    }
    const SlrrFont* font = slrr_font_get(font_key);
    if (!font || !font->ready || font->px_height <= 0) return;

    const float sw = static_cast<float>(width_);
    const float sh = static_cast<float>(height_);
    const float text_w =
        2.f * slrr_font_measure_px(font_key, h->tip.c_str()) / sw;
    const float text_h = 2.f * static_cast<float>(font->px_height) / sh;
    // Osd.animate: border = line height * 0.6, then grow the plate.
    const float border = text_h * 0.6f;
    const float box_w = text_w + border;
    const float box_h = text_h + border;
    if (box_w < 0.02f || box_h < 0.02f) return;

    float cx = 0.f, cy = 0.f;
    osd_pixel_to_text(mouse_x_, mouse_y_, width_, height_, &cx, &cy);
    float bx = cx + box_w * 0.5f + 0.05f;
    float by = cy + box_h * 0.5f + 0.1f;
    const float hw = box_w * 0.5f;
    const float hh = box_h * 0.5f;
    if (bx + hw > 1.f) bx = 1.f - hw;
    if (by + hh > 1.f) by = 1.f - hh;
    if (bx - hw < -1.f) bx = -1.f + hw;
    if (by - hh < -1.f) by = -1.f + hh;

    OsdWidget plate;
    plate.x = bx;
    plate.y = by;
    plate.half_w = hw;
    plate.half_h = hh;
    draw_osd_quad(plate, 0.f, 0.f, 0.f, 1.f);
    const float px = 1.f / sw;
    const float py = 1.f / sh;
    OsdWidget edge;
    edge.x = bx;
    edge.y = by - hh;
    edge.half_w = hw;
    edge.half_h = py;
    draw_osd_quad(edge, 1.f, 1.f, 1.f, 1.f);
    edge.y = by + hh;
    draw_osd_quad(edge, 1.f, 1.f, 1.f, 1.f);
    edge.x = bx - hw;
    edge.y = by;
    edge.half_w = px;
    edge.half_h = hh;
    draw_osd_quad(edge, 1.f, 1.f, 1.f, 1.f);
    edge.x = bx + hw;
    draw_osd_quad(edge, 1.f, 1.f, 1.f, 1.f);

    draw_osd_line(font_key, h->tip, bx, by - text_h * 0.5f, 1, 0xFFFFFFFFu);
    const float white_c[4] = {1.f, 1.f, 1.f, 1.f};
    enc()->setUniform(u_color_, white_c);
  }
#endif
#ifdef _WIN32
  void LibBgfxBackend::draw_fmv(uint16_t view_id)
{
    if (!video_fmv_is_playing()) return;
    if (!video_fmv_frame_ready() || !bgfx::isValid(program_)) return;
    const uint8_t* bgra = video_fmv_bgra();
    const int32_t fw = video_fmv_width();
    const int32_t fh = video_fmv_height();
    if (!bgra || fw <= 0 || fh <= 0) return;

    if (!bgfx::isValid(fmv_tex_) || fmv_tex_w_ != fw || fmv_tex_h_ != fh) {
      if (bgfx::isValid(fmv_tex_)) bgfx::destroy(fmv_tex_);
      fmv_tex_ = bgfx::createTexture2D(
          static_cast<uint16_t>(fw), static_cast<uint16_t>(fh), false, 1,
          bgfx::TextureFormat::BGRA8,
          BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
          nullptr);
      fmv_tex_w_ = fw;
      fmv_tex_h_ = fh;
      fmv_tex_serial_ = 0;
    }
    if (!bgfx::isValid(fmv_tex_)) return;
    const uint64_t serial = video_fmv_frame_serial();
    if (serial == 0 || serial != fmv_tex_serial_) {
      const bgfx::Memory* mem =
          bgfx::copy(bgra, static_cast<uint32_t>(fw * fh * 4));
      bgfx::updateTexture2D(fmv_tex_, 0, 0, 0, 0, static_cast<uint16_t>(fw),
                            static_cast<uint16_t>(fh), mem);
      fmv_tex_serial_ = serial;
    }

    // The video-options columns are placed on the full viewport. A 4:3
    // island left those labels on the empty side bars.
    float x0 = -1.f, x1 = 1.f, y0 = -1.f, y1 = 1.f;
    if (video_fmv_block_osd()) {
      float left, top, w, h;
      video_fmv_letterbox(width_, height_, &left, &top, &w, &h);
      x0 = 2.f * left / width_ - 1.f;
      x1 = 2.f * (left + w) / width_ - 1.f;
      y0 = 2.f * top / height_ - 1.f;
      y1 = 2.f * (top + h) / height_ - 1.f;
    }

    float ortho[16];
    bx::mtxOrtho(ortho, -1.f, 1.f, 1.f, -1.f, 0.f, 100.f, 0.f,
                 bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewRect(view_id, 0, 0, static_cast<uint16_t>(width_),
                      static_cast<uint16_t>(height_));
    bgfx::setViewTransform(view_id, nullptr, ortho);
    enc()->touch(view_id);

    MeshVertex v[4];
    auto setv = [](MeshVertex& o, float x, float y, float uu, float vv) {
      o.x = x;
      o.y = y;
      o.z = 0.f;
      o.nx = 0;
      o.ny = 0;
      o.nz = 1;
      o.u = uu;
      o.v = vv;
    };
    setv(v[0], x0, y0, 0.f, 0.f);
    setv(v[1], x1, y0, 1.f, 0.f);
    setv(v[2], x1, y1, 1.f, 1.f);
    setv(v[3], x0, y1, 0.f, 1.f);
    const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    if (bgfx::getAvailTransientVertexBuffer(4, layout_) < 4 ||
        bgfx::getAvailTransientIndexBuffer(6) < 6)
      return;
    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientIndexBuffer tib;
    bgfx::allocTransientVertexBuffer(&tvb, 4, layout_);
    bgfx::allocTransientIndexBuffer(&tib, 6);
    std::memcpy(tvb.data, v, sizeof(v));
    std::memcpy(tib.data, idx, sizeof(idx));
    float mtx[16];
    bx::mtxIdentity(mtx);
    enc()->setTransform(mtx);
    enc()->setVertexBuffer(0, &tvb);
    enc()->setIndexBuffer(&tib);
    enc()->setTexture(0, s_tex_, fmv_tex_);
    bind_baked_light(nullptr, nullptr);
    const float white[4] = {1.f, 1.f, 1.f, 1.f};
    enc()->setUniform(u_color_, white);
    // Unlit. The mesh light (~0.44 on a flat quad) was the black filter
    // on intros and on prime.avi.
    const float u_at[4] = {0.f, 1.f, 0.f, 0.f};
    enc()->setUniform(u_alpha_test_, u_at);
    enc()->setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_MSAA);
    submit_counted(view_id, program_);
  }
#endif
#ifdef _WIN32
void LibBgfxBackend::draw_vehicle_hud(const VehicleHudState& hud) {
  const uint32_t gps_id = rpk_resolve_resource("frontend.rpk", 0x83);
  const auto &widgets = osd_widgets();
  if (std::none_of(widgets.begin(), widgets.end(), [&](const OsdWidget& w) {
        return hud.owner && w.owner == hud.owner &&
               (w.icon_id == gps_id || w.icon_id == 0x83) &&
               !w.hidden && !w.dead && !w.icon_path.empty();
      })) {
    OsdWidget gps;
    if (gps_hud_path_.empty()) gps_hud_path_ = scene_instances_texture(gps_id);
    gps.icon_path = gps_hud_path_;
    gps.x = -.75f;
    gps.y = .715f;
    gps.half_w = .275f;
    gps.half_h = .335f;
    if (!gps.icon_path.empty()) draw_osd_quad(gps);
  }
  if (!hud.layout) return;

  const auto cars_resource = [](uint32_t local) {
    return rpk_resolve_resource("cars.rpk", local);
  };
  const auto draw = [&](const ScxGeometry& mesh, size_t begin, size_t count,
                        const uint16_t *indices, size_t index_count,
                        float x, float y, float sx, float sy, float angle,
                        float min_x, float min_y, uint32_t color, bool opaque = false,
                        bool color_mix = false, const float* glyph_uv = nullptr) {
    if (!count || count > 65535 || !index_count || mesh.textures.empty() ||
        mesh.uvs.size() < (begin + count) * 2 ||
        bgfx::getAvailTransientVertexBuffer(static_cast<uint32_t>(count), layout_) < count ||
        bgfx::getAvailTransientIndexBuffer(static_cast<uint32_t>(index_count)) < index_count)
      return;
    const auto& path = mesh.textures.front();
    const auto texture = tex_for_osd_icon(path);
    // Build 606 binds a 4x4 0x80808080 texture for a texture it could not
    // load (0x4B9788, SetTexture wrapper 0x4C0180): the item still draws,
    // as half-transparent grey times its colour.
    const bool missing = texture.idx == white_tex_.idx;
    bgfx::TransientVertexBuffer vb;
    bgfx::TransientIndexBuffer ib;
    bgfx::allocTransientVertexBuffer(&vb, static_cast<uint32_t>(count), layout_);
    bgfx::allocTransientIndexBuffer(&ib, static_cast<uint32_t>(index_count));
    auto *vertices = reinterpret_cast<MeshVertex*>(vb.data);
    const float c = std::cos(angle), s = std::sin(angle);
    for (size_t i = 0; i < count; ++i) {
      const size_t v = begin + i;
      const float px = mesh.positions[v * 3] - min_x;
      const float py = mesh.positions[v * 3 + 1] - min_y;
      vertices[i] = {x + sx * (c * px - s * py),
                     y + sy * (s * px + c * py), 0.f, 0.f, 0.f, 1.f,
                     glyph_uv ? glyph_uv[i * 2] : mesh.uvs[v * 2],
                      glyph_uv ? glyph_uv[i * 2 + 1] : mesh.uvs[v * 2 + 1]};
    }
    std::memcpy(ib.data, indices, index_count * sizeof(uint16_t));
    float identity[16];
    bx::mtxIdentity(identity);
    std::string extension = std::filesystem::path(path).extension().string();
    for (char& c : extension)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool premultiplied = !opaque && extension != ".dds";
    const float opacity = float(color >> 24) / 255.f * (missing ? .5f : 1.f);
    const float rgb_scale = premultiplied ? opacity / 255.f : 1.f / 255.f;
    const float grey = missing ? .5f : 1.f;
    const float tint[] = {float((color >> 16) & 255) * rgb_scale * grey,
                          float((color >> 8) & 255) * rgb_scale * grey,
                          float(color & 255) * rgb_scale * grey, opacity};
    const float alpha[] = {0.f, 1.f, opaque ? 0.f : 1.f,
                           color_mix && premultiplied ? 2.f : 0.f};
    enc()->setTransform(identity);
    enc()->setVertexBuffer(0, &vb);
    enc()->setIndexBuffer(&ib);
    enc()->setTexture(0, s_tex_, texture);
    bind_baked_light(nullptr, nullptr);
    enc()->setUniform(u_color_, tint);
    enc()->setUniform(u_alpha_test_, alpha);
    const uint64_t blend = premultiplied
        ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA)
        : BGFX_STATE_BLEND_ALPHA;
    enc()->setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | blend);
    submit_counted(osd_view_, program_);
  };
  const auto safe_numeric_format = [](const std::string& fmt) {
    if (fmt.size() < 2 || fmt.size() > 6 || fmt.front() != '%' || fmt.back() != 'f')
      return false;
    const auto body = fmt.substr(1, fmt.size() - 2);
    const auto dot = body.find('.');
    const auto width = body.substr(0, dot);
    return width.size() <= 2 && width.find_first_not_of("0123456789") == std::string::npos &&
           (dot == std::string::npos || (body.size() == dot + 2 && body.back() >= '0' && body.back() <= '3'));
  };

  // Stock assets and their panel-relative placement in build 606. Center the
  // odometer in its box and keep the full car-health glyph inside its frame.
  const VehicleHudLayout stock{{
      {true, cars_resource(0x55), {.742f, .994f, 0}},
      {true, cars_resource(0x56), {.326f, 1.781f, 0}, {.48f, -.0001f, -.48f, .48f}},
      {}, {},
      {true, cars_resource(0x58), {.222f, .887f, 0}, {}, 0xff4fe015, "%03.0f"},
      {true, cars_resource(0x58), {.407f, .889f, 0}, {}, 0xff4fe015, "N123456R"},
      {true, cars_resource(0x58), {-.357f, .767f, 0}, {}, 0xff4fe015, "%07.0f"},
      {true, cars_resource(0x63), {-.227f, .925f, 0}},
      {true, cars_resource(0x57), {.646f, .631f, 0}},
      {true, cars_resource(0x5c), {-.126f, .843f, 0}, {}, 0xff000000},
      {true, cars_resource(0x5c), {-.126f, .948f, 0}, {}, 0xff000000},
  }};
  const float pixel_aspect = static_cast<float>(height_) / width_;
  // Size the stock strip from the viewport width while preserving its aspect.
  const float stock_scale = .5625f / pixel_aspect;
  const auto stock_y = [&](size_t slot) {
    // The anchors above are measured at 16:9. Keep every component attached
    // to the same panel origin when the strip grows or shrinks vertically.
    const float origin = stock[0].position[1];
    return origin + (stock[slot].position[1] - origin) * stock_scale;
  };
  constexpr uint16_t quad[] = {0, 1, 2, 1, 3, 2};
  for (size_t i = 0; i < stock.size(); ++i) {
    const HudSlot slot = static_cast<HudSlot>(i);
    const auto &override = (*hud.layout)[i];
    const HudElement& item = override.defined ? override : stock[i];
    if (!item.resource || (slot == HudSlot::Handbrake && !hud.handbrake)) continue;

    // A loose --vehicle-osd cfg has no owning RPK. Car packs list cars.rpk as
    // dependency slot 2, so qualify those ids exactly as an installed car cfg
    // would; custom renders then come from the installed (pack) cars.rpk.
    const uint32_t render = hud.preview_directory && override.defined
        ? ((item.resource >> 16) == 2 ? cars_resource(item.resource & 0xffff) : 0)
        : item.resource;
    if (!render) continue;
    const std::shared_ptr<const ScxGeometry> resource = scene_instances_geometry(render);
    const ScxGeometry *mesh = resource.get();
    if (!mesh || !mesh->info.ok) continue;
    if (slot == HudSlot::Damage || slot == HudSlot::Nitro || slot == HudSlot::Turbo) {
      const bool damage = slot == HudSlot::Damage;
      const float fill = slot == HudSlot::Nitro ? hud.nitro : hud.turbo;
      const unsigned char glyph = damage ? 'A'
          : static_cast<unsigned char>('0' + std::lround(std::clamp(fill, 0.f, 1.f) * 8));
      const size_t start = size_t(glyph) * 4;
      if ((start + 4) * 3 > mesh->positions.size()) continue;
      const float *p = mesh->positions.data() + start * 3;
      const float sy = (damage ? (override.defined ? .00014f : .00036f) : .00032f) * stock_scale;
      const float x = override.defined && damage && item.position[0] < 0.f
          ? -item.position[0] : item.position[0];
      const uint32_t color = item.color;
      const float y = override.defined ? item.position[1] : stock_y(i);
      if (damage) {
        // The stock font stores the car outline ('A') and one blip per car
        // region as separate glyphs in one shared coordinate system, front
        // up and the car's left on the left. Quarterpanel script instances
        // exclusively own p/q/u/v at their rest-installed corner. Other
        // regions keep spatial aggregation, and absent parts have no blip.
        // Drivetrain blips take engine parts; wheels keep their slot corner.
        using Kind = VehicleHudPart::Kind;
        using Blip = HudDamageBlip;
        const auto bounds = [&](unsigned char glyph, Blip &out) {
          const size_t first = size_t(glyph) * 4;
          if ((first + 4) * 3 > mesh->positions.size()) return false;
          const float *corner = mesh->positions.data() + first * 3;
          out.x0 = out.x1 = corner[0];
          out.y0 = out.y1 = corner[1];
          for (int k = 1; k < 4; ++k) {
            out.x0 = std::min(out.x0, corner[k * 3]);
            out.x1 = std::max(out.x1, corner[k * 3]);
            out.y0 = std::min(out.y0, corner[k * 3 + 1]);
            out.y1 = std::max(out.y1, corner[k * 3 + 1]);
          }
          return out.x1 > out.x0 && out.y1 > out.y0;
        };
        Blip outline{'A', Kind::Body};
        if (!bounds('A', outline)) continue;
        std::array<Blip, 16> blips{};
        size_t blip_count = 0;
        const auto add = [&](const char *glyphs, Kind kind) {
          for (const char *g = glyphs; *g; ++g) {
            Blip blip{static_cast<unsigned char>(*g), kind};
            if (bounds(blip.glyph, blip)) blips[blip_count++] = blip;
          }
        };
        add("ELT", Kind::Engine);
        add("Ohlpqrtuv", Kind::Body);
        add("abcd", Kind::Wheel);
        std::array<float, 8> left_fender_uv{};
        const bool stock_fenders = item.resource == cars_resource(0x57) &&
            hud_stock_left_fender_uv(*mesh, left_fender_uv);
        hud_assign_damage(std::span(blips.data(), blip_count),
                          std::span(hud.parts.data(), std::min(hud.part_count, hud.parts.size())),
                          outline, stock_fenders);
        const float scale_x = sy * pixel_aspect;
        // The outline is the chassis: green when intact, like every blip.
        draw(*mesh, size_t('A') * 4, 4, quad, 6, x, y, scale_x, sy, 0, p[0], p[1],
             hud_damage_color(hud.chassis_condition));
        for (size_t b = 0; b < blip_count; ++b) {
          const Blip &blip = blips[b];
          if (!blip.present) continue;
          draw(*mesh, size_t(blip.glyph) * 4, 4, quad, 6, x, y, scale_x, sy, 0, p[0], p[1],
               hud_damage_color(blip.condition), false, false,
               stock_fenders && blip.glyph == 'q' ? left_fender_uv.data() : nullptr);
        }
      } else {
        draw(*mesh, start, 4, quad, 6, x, y,
             sy * pixel_aspect, sy, 0, p[0], p[1], color);
      }
      continue;
    }
    // Only a font render (RSD flags bit 4) spells the readout; any other render
    // on these slots falls through and is drawn whole like osd_gauge.
    if ((slot == HudSlot::Rpm || slot == HudSlot::Speed ||
         slot == HudSlot::Gear || slot == HudSlot::Mileage) &&
        hud_render_draws_text(rpk_payload_prop(render, "flags"))) {
      char buffer[128]{};
      if (slot == HudSlot::Gear) {
        const std::string labels = item.format.empty() ? "N123456R" : item.format;
        if (hud.gear >= 0 && size_t(hud.gear) < labels.size()) buffer[0] = labels[hud.gear];
      } else {
        const char *format = safe_numeric_format(item.format) ? item.format.c_str() : "%.0f";
        const double value = slot == HudSlot::Rpm ? hud.rpm :
                             slot == HudSlot::Speed ? hud.speed * hud.unit_scale
                                                    : hud.mileage * hud.unit_scale;
        std::snprintf(buffer, sizeof(buffer), format, value);
      }
      float x = item.position[0];
      float y = override.defined ? item.position[1] : stock_y(i);
      // Build 606 draws CFG font readouts in screen space (0x526D10 ->
      // 0x4F4D80 -> 0x4F4DE0): the glyph cell's top-left sits at NDC (x, y),
      // y down, glyph c is SCX quad c*4 at 1 pixel per 10 SCX units at any
      // resolution, and the pen advances by the glyph width only. The stock
      // table keeps its own 16:9 calibration.
      if (!override.defined && (std::abs(x) > 1.f || item.position[2] != 0.f)) {
        x *= .42f;
        y *= -.82f;
      }
      const float sy = override.defined ? .2f / static_cast<float>(height_)
                                        : .00024f * stock_scale;
      const float sx = override.defined ? .2f / static_cast<float>(width_)
                                        : sy * pixel_aspect;
      for (const unsigned char ch : std::string(buffer)) {
        const size_t start = size_t(ch) * 4;
        if ((start + 4) * 3 > mesh->positions.size()) continue;
        const float *p = mesh->positions.data() + start * 3;
        const float w = p[3] - p[0], h = p[7] - p[1];
        if (w <= 0 || h <= 0 || w > 2000 || h > 2000) continue;
        draw(*mesh, start, 4, quad, 6, x, y, sx, sy, 0, p[0], p[1], item.color);
        x += (override.defined ? w : w + 20.f) * sx;
      }
      continue;
    }

    float angle = 0;
    if (slot == HudSlot::RpmPin || slot == HudSlot::SpeedPin) {
      const auto &a = item.needle;
      const double value = slot == HudSlot::RpmPin ? hud.rpm : hud.speed;
      angle = std::clamp(a[0] + a[1] * static_cast<float>(value),
                         std::min(a[2], a[3]), std::max(a[2], a[3]));
    }
    const size_t count = mesh->positions.size() / 3;
    if (count > 65535) continue;
    const std::vector<uint16_t> indices(mesh->indices.begin(), mesh->indices.end());
    // CFG mesh items sit in the Osd scene: camera at z = 5.48 (Osd.CAM_OFFSET),
    // 30 deg vertical view, so the z = 0 plane spans +-1.469 (Osd.SCALE_3D)
    // vertically and +-1.469*aspect horizontally; items nearer (z > 0) grow.
    const float depth = override.defined
        ? 5.48f / std::max(5.48f - item.position[2], .1f) : 1.f;
    const float yscale = override.defined ? -depth / 1.469f : -.406f * stock_scale;
    const float xscale = -yscale * pixel_aspect;
    const float x = override.defined ? item.position[0] * xscale : item.position[0];
    const float y = override.defined ? item.position[1] * yscale : stock_y(i);
    draw(*mesh, 0, count, indices.data(), indices.size(),
         x, y,
         xscale * mesh->position_scale, yscale * mesh->position_scale,
         angle, 0, 0, item.color, false,
         slot == HudSlot::Gauge && !override.defined && !mesh->groups.empty() &&
             mesh->groups.front().material_type == 3u);
  }
}

  void LibBgfxBackend::draw_osd()
{
    perf::Scope perf_scope(perf::Zone::Osd);
    osd_sync_button_pictures();
    const auto& widgets = osd_widgets();
    const auto& texts = slrr_osd_texts();
    const bool loading = !osd_loading_overlay().empty();
    VehicleHudState hud;
    const bool have_hud = part_vehicle_hud(hud);
    if ((widgets.empty() && texts.empty() && !loading && !have_hud) ||
        !bgfx::isValid(program_))
      return;
    float ortho[16];
    // OSD text space: x right, y down (top = -1). Plates stay under the car
    // preview (views 8+). Buttons and text sit on kOsdView, above it.
    bx::mtxOrtho(ortho, -1.f, 1.f, 1.f, -1.f, 0.f, 100.f, 0.f,
                 bgfx::getCaps()->homogeneousDepth);
    auto setup_osd = [&](uint16_t view) {
      bgfx::setViewRect(view, 0, 0, static_cast<uint16_t>(width_),
                        static_cast<uint16_t>(height_));
      bgfx::setViewTransform(view, nullptr, ortho);
      bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
      enc()->touch(view);
    };
    setup_osd(kOsdUnder);
    setup_osd(kOsdView);
    osd_view_ = kOsdUnder;

    // Menu plate: dark translucent fullscreen when GENERALBG path missing.
    bool have_bg_tex = false;
    for (const OsdWidget& w : widgets) {
      if (!w.hidden && w.background && !w.icon_path.empty()) {
        have_bg_tex = true;
        break;
      }
    }
    // Fallback veil only when there is no plate and no movie. Over prime.avi
    // it reads as a black opacity layer on top of the FMV.
    // A driving HUD has no menu plate. Its active world camera must remain
    // visible instead of receiving the menu-only fallback veil.
    if (!have_bg_tex && osd_overlay_ && !garage_ &&
        !video_fmv_is_playing() && !fullscreen_script_camera()) {
      OsdWidget plate;
      plate.x = 0.f;
      plate.y = 0.f;
      plate.half_w = 1.f;
      plate.half_h = 1.f;
      plate.background = true;
      // White tex + darken via color uniform — draw after setting color.
      // draw_osd_quad owns the tint uniform; pass the color into the draw.
      const float u_at[4] = {0.f, 1.f, 1.f, 0.f};
      enc()->setUniform(u_alpha_test_, u_at);
      draw_osd_quad(plate, 0.05f, 0.05f, 0.07f, 0.82f);
      const float white_c[4] = {1.f, 1.f, 1.f, 1.f};
      enc()->setUniform(u_color_, white_c);
    }

    // Viewport priority: menu, then the darken shield, then the dialog plate.
    // Within a layer, plates sit under icons and those sit under that layer's
    // text, so the gold frame is not covered by the black veil.
    std::vector<int32_t> layers;
    auto note_layer = [&](int32_t layer) {
      for (int32_t have : layers)
        if (have == layer) return;
      layers.push_back(layer);
    };
    for (const OsdWidget& w : widgets) {
      if (!w.hidden) note_layer(w.layer);
    }
    for (const SlrrOsdText& t : texts) {
      if (t.visible && !t.text.empty()) note_layer(t.layer);
    }
    if (have_hud) note_layer(18);
    std::sort(layers.begin(), layers.end());

    // Openingpic (frontend:0x0005) is the splash still, drawn opaque on the
    // OSD view, which sits above the movie. Once the menu plate or a real
    // prime.avi frame is up, that still must not fill the frame hole.
    auto is_opening = [](const std::string& path) {
      std::string low = path;
      for (char& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return low.find("opening") != std::string::npos;
    };
    bool other_plate = false;
    for (const OsdWidget& w : widgets) {
      if (w.hidden || !w.background || w.icon_path.empty()) continue;
      if (!is_opening(w.icon_path)) other_plate = true;
    }
    const bool movie = video_fmv_is_playing() && video_fmv_frame_ready();
    // Once the menu (or the movie) has covered the splash, do not bring
    // Openingpic back when a section hides its plate. Back from the track
    // selector was showing that still instead of the loading picture.
    if (other_plate || movie) opening_retired_ = true;

    // The parts bar is an opaque plate. On a later OSD layer it is submitted
    // above the slot cameras and the cans disappear. A plate that only covers
    // a preview slot stays under that camera. A fullscreen plate still covers
    // the preview, so a dialog does not open a hole onto the car.
    // Viewport priority still decides between the plate and the camera:
    // the GPS frame (Player Osd, priority 18) must cover the navigator
    // (viewport 12), while the slot cameras share the parts bar's viewport.
    enum class PreviewOverlap { None, Behind, Above };
    auto plate_preview_overlap = [&](const OsdWidget& w) {
      if (w.half_w > 0.9f && w.half_h > 0.9f) return PreviewOverlap::None;
      const float left = (w.x - w.half_w + 1.f) * 0.5f;
      const float top = (w.y - w.half_h + 1.f) * 0.5f;
      const float right = (w.x + w.half_w + 1.f) * 0.5f;
      const float bot = (w.y + w.half_h + 1.f) * 0.5f;
      PreviewOverlap overlap = PreviewOverlap::None;
      for (const auto& p : previews_) {
        const float x0 = std::max(left, p.viewport.vp_x);
        const float y0 = std::max(top, p.viewport.vp_y);
        const float x1 = std::min(right, p.viewport.vp_x + p.viewport.vp_w);
        const float y1 = std::min(bot, p.viewport.vp_y + p.viewport.vp_h);
        if (x1 - x0 <= 0.01f || y1 - y0 <= 0.01f) continue;
        if (p.viewport.vp_pri >= w.layer) return PreviewOverlap::Behind;
        overlap = PreviewOverlap::Above;
      }
      return overlap;
    };

    struct IconDraw { const OsdWidget* widget; bool glow, color_chip; };
    std::unordered_map<int32_t, std::array<std::vector<IconDraw>, 3>> icon_passes;
    // Classify once per frame. Keep registry order within each layer/pass.
    for (const auto& w : widgets) {
      if (w.hidden || w.dead || w.plain) continue;
      const uint32_t local = w.icon_id & 0xFFFFu;
      // frontend:0x00AF is Empty.ptx. A disabled pager keeps that texture
      // and shows frontend:0x013C only while Gadget.focus is on it.
      std::string low = w.icon_path;
      for (char& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const bool blank = local == 0x00AFu ||
                         low.find("empty.ptx") != std::string::npos;
      // Chassis leaves this button disabled. Only a complete vehicle
      // offers the color chip, and it starts white.
      const bool color_chip =
          w.cmd == 116 && blank && !w.hit_only && !w.disabled;
      if (w.background || w.hit_only) continue;
      if (w.disabled && !color_chip) continue;
      if (blank && !color_chip) continue;
      if ((local == 0x12bu || local == 0x13cu) && !osd_focus_plate_visible(w)) continue;
      const bool knob = local == 0x0091u;
      // Gadget.focus builds this plate 1.1× and slightly behind the icon.
      // Creation order painted it on top, so hover hid the button.
      const bool glow = local == 0x012Bu || local == 0x013Cu;
      const int which = knob ? 2 : (glow ? 0 : 1);
      icon_passes[w.layer][which].push_back({&w, glow, color_chip});
    }
    // Cursor tooltips/text can add a lower layer. They must not promote
    // the page background above preview cameras by changing layers.front().
    const int32_t page_background_layer = osd_page_background_layer();
    for (int32_t layer : layers) {
      // Credits first, then plates and icons, then labels. The frame and
      // button textures cover the scroller; the header and BACK stay readable.
      // Plates under the car. Labels and buttons above it, so the color chip
      // is not covered and the chassis stays visible.
      osd_view_ = kOsdView;
      if (have_hud && layer == 18) draw_vehicle_hud(hud);
      draw_osd_texts(layer, true, true);
      // The page background is the lowest layer. A dialog plate is higher
      // and has to cover the car, so it stays with the buttons. The parts
      // strip is the exception: the slot cameras draw into it.
      for (const OsdWidget& w : widgets) {
        if (w.layer != layer || w.hidden || w.dead || w.disabled || !w.background ||
            w.plain)
          continue;
        if (w.icon_path.empty()) continue;  // RRT_NONE — no mesh (Osd.java)
        if ((opening_retired_ || movie || other_plate) &&
            is_opening(w.icon_path))
          continue;
        // A modal dialog (BUY PART) overlaps the catalog previews. Sending
        // that plate to view 2 put it under the parts and under their names.
        const PreviewOverlap overlap = plate_preview_overlap(w);
        osd_view_ = (!osd_owner_modal(w.owner) &&
                     overlap != PreviewOverlap::Above &&
                     (layer == page_background_layer ||
                      overlap == PreviewOverlap::Behind))
                        ? kOsdUnder
                        : kOsdView;
        draw_osd_quad(w);
      }
      osd_view_ = kOsdView;
      // OSD v2 (docs/ui/osd-v2.md): an attached Osd's rectangles draw as
      // written, back to front by their matrix z (Osd priority), each with
      // its OsdHost.tint. Empty.ptx with a tint is a solid colour quad.
      {
        std::vector<const OsdWidget*> plain;
        for (const OsdWidget& w : widgets)
          if (w.plain && w.layer == layer && !w.hidden && !w.dead && !w.hit_only)
            plain.push_back(&w);
        std::stable_sort(plain.begin(), plain.end(),
                         [](const OsdWidget* a, const OsdWidget* b) { return a->z < b->z; });
        for (const OsdWidget* w : plain) {
          OsdWidget quad = *w;
          std::string low = quad.icon_path;
          for (char& c : low)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          const bool blank = (quad.icon_id & 0xFFFFu) == 0x00AFu ||
                             low.find("empty.ptx") != std::string::npos;
          if (blank) {
            if (quad.tint == 0xFFFFFFFFu) continue;
            quad.icon_path.clear();
          } else if (quad.icon_path.empty()) {
            continue;
          }
          const float a = float(quad.tint >> 24) / 255.f * quad.fade;
          draw_osd_quad(quad, float((quad.tint >> 16) & 255) / 255.f * a,
                        float((quad.tint >> 8) & 255) / 255.f * a,
                        float(quad.tint & 255) / 255.f * a, a);
        }
      }
      if (const OsdWidget* hw = osd_hover_widget()) {
        // Text buttons hover the label, same as the main menu. The hl
        // texture stays on icon buttons. Sliders retain their own track/knob.
        const bool text_button =
            !hw->string_input && !hw->row_label && hw->label_src;
        if (hw->layer == layer && !osd_widget_is_slider(*hw) &&
            !hw->string_input && !text_button &&
            !hw->icon_path.empty()) {
          OsdWidget hl = *hw;
          // Square icons: stock ×1.1. Slider bars are wide and opaque, so the
          // same plate stays hidden unless the short axis grows past the track.
          if (hw->row_label && hw->half_w > hw->half_h * 2.5f) {
            hl.half_w *= 1.02f;
            hl.half_h *= 2.4f;
          } else {
            hl.half_w *= kOsdHighlightScale;
            hl.half_h *= kOsdHighlightScale;
          }
          hl.icon_path = osd_highlight_path();
          if (!hl.icon_path.empty()) draw_osd_quad(hl);
        }
      }
      const auto icons = icon_passes.find(layer);
      if (icons != icon_passes.end())
        for (const auto& pass : icons->second)
          for (const auto& entry : pass) {
            const auto& w = *entry.widget;
            if(entry.glow) {
              OsdWidget plate=w;
              const auto& path=osd_highlight_disabled(w)?osd_disabled_highlight_path():osd_highlight_path();
              if(!path.empty())plate.icon_path=path;
              if(!plate.icon_path.empty())draw_osd_quad(plate);
            } else if (entry.color_chip) {
              OsdWidget chip = w;
              chip.icon_path.clear();
              draw_osd_quad(chip);
            } else if (!w.icon_path.empty()) draw_osd_quad(w);
          }
      // Current button pictures are foreground icons, drawn once after the
      // highlight pass. Never repaint a cached hit-box glyph on hover.
      draw_osd_texts(layer, true, false);
    }
    osd_view_ = kOsdView;
    draw_tooltip();
    // After every layer, so a hidden menu group cannot sit on Loading.dds.
    if (loading) {
      OsdWidget plate;
      plate.half_w = 1.f;
      plate.half_h = 1.f;
      plate.background = true;
      plate.icon_path = osd_loading_overlay();
      osd_view_ = kOsdView;
      draw_osd_quad(plate);
    }
  }
#endif

#ifdef _WIN32
  void LibBgfxBackend::draw_osd_line(void* font_key, const std::string& text, float x, float y,
                     int align, uint32_t argb, const SlrrFontEffects* effects)
{
    if (!font_key || text.empty() || width_ <= 0 || height_ <= 0 ||
        !bgfx::isValid(text_program_)) return;
    const SlrrFont* font = slrr_font_get(font_key);
    if (!font || !font->ready) return;
    const auto tex = tex_for_font(font_key);
    if (!bgfx::isValid(tex)) return;
    OsdTextRun run(*font, text, x, y, static_cast<float>(width_),
                   static_cast<float>(height_), align,
                   slrr_font_measure_px(font_key, text.c_str()));
    const float col[4] = {((argb >> 16) & 255) / 255.f,
                         ((argb >> 8) & 255) / 255.f,
                         (argb & 255) / 255.f, ((argb >> 24) & 255) / 255.f};
    const auto& style = effects ? *effects : font->effects;
    const float params[8] = {style.outline_px, style.shadow_x, style.shadow_y,
                            font->distance_field ? 1.f : 0.f,
                            1.f / font->atlas_w, 1.f / font->atlas_h, 0.f, 0.f};
    auto rgba = [](uint32_t color) {
      return std::array<float, 4>{((color >> 16) & 255) / 255.f,
                                 ((color >> 8) & 255) / 255.f,
                                 (color & 255) / 255.f, ((color >> 24) & 255) / 255.f};
    };
    const auto outline = rgba(style.outline_color), shadow = rgba(style.shadow_color);
    for (;;) {
      const auto capacity = osd_text_capacity(
          bgfx::getAvailTransientVertexBuffer(kOsdTextBatchGlyphs * 4, text_layout_),
          bgfx::getAvailTransientIndexBuffer(kOsdTextBatchGlyphs * 6));
      if (!capacity || !run.next(capacity, text_vertices_, text_indices_)) break;
      bgfx::TransientVertexBuffer tvb;
      bgfx::TransientIndexBuffer tib;
      bgfx::allocTransientVertexBuffer(&tvb, static_cast<uint32_t>(text_vertices_.size()), text_layout_);
      bgfx::allocTransientIndexBuffer(&tib, static_cast<uint32_t>(text_indices_.size()));
      std::memcpy(tvb.data, text_vertices_.data(), text_vertices_.size() * sizeof(OsdTextVertex));
      std::memcpy(tib.data, text_indices_.data(), text_indices_.size() * sizeof(uint16_t));
      float mtx[16]; bx::mtxIdentity(mtx);
      enc()->setTransform(mtx);
      enc()->setVertexBuffer(0, &tvb); enc()->setIndexBuffer(&tib);
      enc()->setTexture(0, s_tex_, tex);
      enc()->setUniform(u_color_, col);
      enc()->setUniform(u_font_params_, params, 2);
      enc()->setUniform(u_font_outline_, outline.data());
      enc()->setUniform(u_font_shadow_, shadow.data());
      enc()->setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                     BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
      submit_counted(osd_view_, text_program_);
    }
    const float white[4] = {1.f, 1.f, 1.f, 1.f};
    enc()->setUniform(u_color_, white);
  }
#endif


}  // namespace slrr
