// Menu, garage and FMV overlay.
#include "bgfx_lib_backend.hpp"

#include "slrr/part_runtime.hpp"
#include "slrr/dds_image.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/ptx_image.hpp"
#include "slrr/rpk_index.hpp"
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

#ifdef _WIN32
  bgfx::TextureHandle LibBgfxBackend::tex_for_osd_icon(const std::string& path)
{
    if (path.empty()) return white_tex_;
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
    bgfx::setTransform(mtx);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setTexture(0, s_tex_, tex_for_osd_icon(w.icon_path));
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
    bgfx::setUniform(u_alpha_test_, u);
    const float tint[4] = {cr, cg, cb, ca};
    bgfx::setUniform(u_color_, tint);
    // PTX RGB is already multiplied by alpha. SRC_ALPHA would darken the
    // menu plate a second time (rgb * a * a). Premultiplied blend keeps
    // the gold frame and lets prime.avi through the transparent center.
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,
                                         BGFX_STATE_BLEND_INV_SRC_ALPHA) |
                   BGFX_STATE_MSAA);
    bgfx::submit(osd_view_, program_);
  }
#endif
#ifdef _WIN32
  bgfx::TextureHandle LibBgfxBackend::tex_for_font(void* font_key)
{
    if (!font_key) return white_tex_;
    auto it = font_tex_.find(font_key);
    if (it != font_tex_.end()) return it->second;
    const SlrrFont* font = slrr_font_get(font_key);
    if (!font || font->rgba.empty() || font->atlas_w <= 0 || font->atlas_h <= 0)
      return white_tex_;
    const bgfx::Memory* mem = bgfx::copy(
        font->rgba.data(),
        static_cast<uint32_t>(font->rgba.size()));
    bgfx::TextureHandle th = bgfx::createTexture2D(
        static_cast<uint16_t>(font->atlas_w),
        static_cast<uint16_t>(font->atlas_h), false, 1,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
            BGFX_SAMPLER_MIP_POINT,
        mem);
    if (!bgfx::isValid(th)) return white_tex_;
    font_tex_[font_key] = th;
    return th;
  }
#endif
#ifdef _WIN32
  void LibBgfxBackend::draw_osd_texts(int32_t only_layer, bool filter_layer, bool behind_ui)
{
    const auto& texts = slrr_osd_texts();
    if (texts.empty() || width_ <= 0 || height_ <= 0) return;
    const float sw = static_cast<float>(width_);
    const float sh = static_cast<float>(height_);

    // Text-menu hover: stock HL_LABELCOLOR → COLOR_SELECTED (full white).
    const OsdWidget* hover = osd_hover_widget();

    for (const SlrrOsdText& t : texts) {
      if (!t.visible || !t.font_key || t.text.empty()) continue;
      if (t.behind_ui != behind_ui) continue;
      if (filter_layer && t.layer != only_layer) continue;
      const SlrrFont* font = slrr_font_get(t.font_key);
      if (!font || !font->ready) continue;
      bgfx::TextureHandle tex = tex_for_font(t.font_key);
      if (!bgfx::isValid(tex)) continue;

      float width_px = slrr_font_measure_px(t.font_key, t.text.c_str());
      const float width_osd = 2.f * width_px / sw;
      float pen_x = t.x;
      if (t.align == 1) pen_x -= width_osd * 0.5f;  // CENTER
      else if (t.align == 0) pen_x -= width_osd;     // RIGHT
      // Host OSD: y+ down (top = -1). Glyph extends downward.
      const float top_y = t.y;

      uint32_t argb = t.color;
      const bool glow = hover && hover->label_src == t.text_key;
      // changeSelection2(-1) focuses the first row (NEW CAREER) in yellow
      // even when the pointer is elsewhere. That color is the hover state.
      if (argb == 0xFFFFCB00u && !glow) argb = 0xFFC0C0C0u;
      if (glow) argb = 0xFFFFFFFFu;
      const float cr = ((argb >> 16) & 0xff) / 255.f;
      const float cg = ((argb >> 8) & 0xff) / 255.f;
      const float cb = (argb & 0xff) / 255.f;
      const float ca = ((argb >> 24) & 0xff) / 255.f;
      const float col[4] = {cr, cg, cb, ca};
      bgfx::setUniform(u_color_, col);
      const float u_at[4] = {0.f, 1.f, 1.f, 0.f};
      bgfx::setUniform(u_alpha_test_, u_at);

      const float to_x = 2.f / sw;
      const float to_y = 2.f / sh;
      for (unsigned char ch : t.text) {
        const size_t gi = static_cast<size_t>(ch);
        const bool has = gi < font->glyphs.size();
        const SlrrFontGlyph* g = has ? &font->glyphs[gi] : nullptr;
        const float adv_px =
            (g && g->adv_px > 0.f) ? g->adv_px
                                   : static_cast<float>(font->px_height) * 0.35f;
        const bool draw =
            g && (g->u1 > g->u0 + 1e-6f) && (g->v1 > g->v0 + 1e-6f);
        const float adv_osd = adv_px * to_x;
        if (draw) {
          // Mesh order: TL, TR, BL, BR. Quad winding matches the old UV box.
          const int corner[4] = {0, 1, 3, 2};
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
          for (int i = 0; i < 4; ++i) {
            const int c = corner[i];
            setv(v[i], pen_x + g->px[c] * to_x, top_y + g->py[c] * to_y,
                 g->tu[c], g->tv[c]);
          }
          const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
          if (bgfx::getAvailTransientVertexBuffer(4, layout_) < 4 ||
              bgfx::getAvailTransientIndexBuffer(6) < 6)
            break;
          bgfx::TransientVertexBuffer tvb;
          bgfx::TransientIndexBuffer tib;
          bgfx::allocTransientVertexBuffer(&tvb, 4, layout_);
          bgfx::allocTransientIndexBuffer(&tib, 6);
          std::memcpy(tvb.data, v, sizeof(v));
          std::memcpy(tib.data, idx, sizeof(idx));
          float mtx[16];
          bx::mtxIdentity(mtx);
          bgfx::setTransform(mtx);
          bgfx::setVertexBuffer(0, &tvb);
          bgfx::setIndexBuffer(&tib);
          bgfx::setTexture(0, s_tex_, tex);
          bind_baked_light(nullptr, nullptr);
          bgfx::setUniform(u_color_, col);
          bgfx::setUniform(u_alpha_test_, u_at);
          bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                         BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
          bgfx::submit(osd_view_, program_);
        }
        pen_x += adv_osd;
      }
    }
    const float white_c[4] = {1.f, 1.f, 1.f, 1.f};
    bgfx::setUniform(u_color_, white_c);
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
    bgfx::setUniform(u_color_, white_c);
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
    bgfx::touch(view_id);

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
    bgfx::setTransform(mtx);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setTexture(0, s_tex_, fmv_tex_);
    bind_baked_light(nullptr, nullptr);
    const float white[4] = {1.f, 1.f, 1.f, 1.f};
    bgfx::setUniform(u_color_, white);
    // Unlit. The mesh light (~0.44 on a flat quad) was the black filter
    // on intros and on prime.avi.
    const float u_at[4] = {0.f, 1.f, 0.f, 0.f};
    bgfx::setUniform(u_alpha_test_, u_at);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_MSAA);
    bgfx::submit(view_id, program_);
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
                        bool color_mix = false) {
    if (!count || count > 65535 || !index_count || mesh.textures.empty() ||
        mesh.uvs.size() < (begin + count) * 2 ||
        bgfx::getAvailTransientVertexBuffer(static_cast<uint32_t>(count), layout_) < count ||
        bgfx::getAvailTransientIndexBuffer(static_cast<uint32_t>(index_count)) < index_count)
      return;
    const auto& path = mesh.textures.front();
    const auto texture = tex_for_osd_icon(path);
    if (texture.idx == white_tex_.idx) return;
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
                     mesh.uvs[v * 2], mesh.uvs[v * 2 + 1]};
    }
    std::memcpy(ib.data, indices, index_count * sizeof(uint16_t));
    float identity[16];
    bx::mtxIdentity(identity);
    std::string extension = std::filesystem::path(path).extension().string();
    for (char& c : extension)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool premultiplied = !opaque && extension != ".dds";
    const float opacity = float(color >> 24) / 255.f;
    const float rgb_scale = premultiplied ? opacity / 255.f : 1.f / 255.f;
    const float tint[] = {float((color >> 16) & 255) * rgb_scale,
                          float((color >> 8) & 255) * rgb_scale,
                          float(color & 255) * rgb_scale, opacity};
    const float alpha[] = {0.f, 1.f, opaque ? 0.f : 1.f,
                           color_mix && premultiplied ? 2.f : 0.f};
    bgfx::setTransform(identity);
    bgfx::setVertexBuffer(0, &vb);
    bgfx::setIndexBuffer(&ib);
    bgfx::setTexture(0, s_tex_, texture);
    bind_baked_light(nullptr, nullptr);
    bgfx::setUniform(u_color_, tint);
    bgfx::setUniform(u_alpha_test_, alpha);
    const uint64_t blend = premultiplied
        ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA)
        : BGFX_STATE_BLEND_ALPHA;
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | blend);
    bgfx::submit(osd_view_, program_);
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

    std::shared_ptr<const ScxGeometry> resource;
    const ScxGeometry *mesh = nullptr;
    bool opaque = false;
    if (hud.preview_directory && override.defined) {
      // The loose OSDPACK fixture has no RPK entries for its two custom
      // render IDs. Resolve those from the SCX and PNG beside cfg.txt.
      const bool font = item.resource == 0x0002baa4 && slot == HudSlot::Rpm;
      const bool pin = item.resource == 0x0002baa5 && slot == HudSlot::RpmPin;
      if (font || pin) {
        opaque = pin; // The loose fixture's fff.PNG has zero alpha everywhere.
        const std::filesystem::path dir(*hud.preview_directory);
        const auto path = (dir / "meshes" / (font ? "font.scx" : "osdpin.SCX")).string();
        auto [entry, fresh] = hud_preview_meshes_.try_emplace(path);
        if (fresh) {
          entry->second = load_scx_geometry(path);
          entry->second.textures = {(dir / "textures" / (font ? "font.png" : "fff.PNG")).string()};
        }
        mesh = &entry->second;
      } else if ((item.resource >> 16) == 2) {
        resource = scene_instances_geometry(cars_resource(item.resource & 0xffff));
      }
    } else {
      resource = scene_instances_geometry(item.resource);
    }
    if (!mesh) mesh = resource.get();
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
      uint32_t color = item.color;
      if (damage) {
        const float condition = std::clamp(hud.condition, 0.f, 1.f);
        const auto red = static_cast<uint32_t>(255.f * std::min(1.f, 2.f * (1.f - condition)));
        const auto green = static_cast<uint32_t>(255.f * std::min(1.f, 2.f * condition));
        color = 0xff000000u | (red << 16) | (green << 8);
      }
      const float y = override.defined ? item.position[1] : stock_y(i);
      if (damage) {
        // The stock font stores the car outline and its damage blips as
        // separate glyphs in one shared coordinate system. Draw the complete
        // assembly so the engine, wheels, skirts and body sections are visible.
        for (const unsigned char part : "AELOTabcdhlpqrtuv") {
          if (!part) continue;
          const size_t part_start = size_t(part) * 4;
          if ((part_start + 4) * 3 > mesh->positions.size()) continue;
          draw(*mesh, part_start, 4, quad, 6, x, y,
               sy * pixel_aspect, sy, 0, p[0], p[1], color);
        }
      } else {
        draw(*mesh, start, 4, quad, 6, x, y,
             sy * pixel_aspect, sy, 0, p[0], p[1], color);
      }
      continue;
    }
    if (slot == HudSlot::Rpm || slot == HudSlot::Speed ||
        slot == HudSlot::Gear || slot == HudSlot::Mileage) {
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
      if (std::abs(x) > 1.f || item.position[2] != 0.f) {
        x *= .42f;
        y *= -.82f;
      }
      const float sy = .00024f * (override.defined ? 1.f : stock_scale);
      const float sx = sy * pixel_aspect;
      for (const unsigned char ch : std::string(buffer)) {
        const size_t start = size_t(ch) * 4;
        if ((start + 4) * 3 > mesh->positions.size()) continue;
        const float *p = mesh->positions.data() + start * 3;
        const float w = p[3] - p[0], h = p[7] - p[1];
        if (w <= 0 || h <= 0 || w > 2000 || h > 2000) continue;
        draw(*mesh, start, 4, quad, 6, x, y, sx, sy, 0, p[0], p[1], item.color);
        x += (w + 20.f) * sx;
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
    const float yscale = override.defined ? -.82f : -.406f * stock_scale;
    const float xscale = -yscale * pixel_aspect;
    const float x = override.defined ? item.position[0] * xscale : item.position[0];
    const float y = override.defined ? item.position[1] * yscale : stock_y(i);
    draw(*mesh, 0, count, indices.data(), indices.size(),
         x, y,
         xscale * mesh->position_scale, yscale * mesh->position_scale,
         angle, 0, 0, item.color, opaque,
         slot == HudSlot::Gauge && !override.defined && !mesh->groups.empty() &&
             mesh->groups.front().material_type == 3u);
  }
}

  void LibBgfxBackend::draw_osd()
{
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
      bgfx::touch(view);
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
    if (!have_bg_tex && osd_overlay_ && !garage_ && !video_fmv_is_playing()) {
      OsdWidget plate;
      plate.x = 0.f;
      plate.y = 0.f;
      plate.half_w = 1.f;
      plate.half_h = 1.f;
      plate.background = true;
      // White tex + darken via color uniform — draw after setting color.
      const float dark[4] = {0.05f, 0.05f, 0.07f, 0.82f};
      bgfx::setUniform(u_color_, dark);
      const float u_at[4] = {0.f, 1.f, 1.f, 0.f};
      bgfx::setUniform(u_alpha_test_, u_at);
      draw_osd_quad(plate);
      const float white_c[4] = {1.f, 1.f, 1.f, 1.f};
      bgfx::setUniform(u_color_, white_c);
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
    auto plate_behind_preview = [&](const OsdWidget& w) {
      if (w.half_w > 0.9f && w.half_h > 0.9f) return false;
      const float left = (w.x - w.half_w + 1.f) * 0.5f;
      const float top = (w.y - w.half_h + 1.f) * 0.5f;
      const float right = (w.x + w.half_w + 1.f) * 0.5f;
      const float bot = (w.y + w.half_h + 1.f) * 0.5f;
      for (const auto& p : previews_) {
        const float x0 = std::max(left, p.viewport.vp_x);
        const float y0 = std::max(top, p.viewport.vp_y);
        const float x1 = std::min(right, p.viewport.vp_x + p.viewport.vp_w);
        const float y1 = std::min(bot, p.viewport.vp_y + p.viewport.vp_h);
        if (x1 - x0 > 0.01f && y1 - y0 > 0.01f) return true;
      }
      return false;
    };

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
        if (w.layer != layer || w.hidden || w.dead || w.disabled || !w.background)
          continue;
        if (w.icon_path.empty()) continue;  // RRT_NONE — no mesh (Osd.java)
        if ((opening_retired_ || movie || other_plate) &&
            is_opening(w.icon_path))
          continue;
        // A modal dialog (BUY PART) overlaps the catalog previews. Sending
        // that plate to view 2 put it under the parts and under their names.
        osd_view_ = (!osd_owner_modal(w.owner) &&
                     (layer == layers.front() || plate_behind_preview(w)))
                        ? kOsdUnder
                        : kOsdView;
        draw_osd_quad(w);
      }
      osd_view_ = kOsdView;
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
      auto draw_icon = [&](const OsdWidget& w, int pass) {
        if (w.layer != layer || w.hidden || w.dead) return;
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
        if (w.background || w.hit_only) return;
        if (w.disabled && !color_chip) return;
        if (blank && !color_chip) return;
        if ((local == 0x12bu || local == 0x13cu) && !osd_focus_plate_visible(w)) return;
        const bool knob = local == 0x0091u;
        // Gadget.focus builds this plate 1.1× and slightly behind the icon.
        // Creation order painted it on top, so hover hid the button.
        const bool glow = local == 0x012Bu || local == 0x013Cu;
        const int which = knob ? 2 : (glow ? 0 : 1);
        if (which != pass) return;
        if(glow) {
          OsdWidget plate=w;
          const auto& path=osd_highlight_disabled(w)?osd_disabled_highlight_path():osd_highlight_path();
          if(!path.empty())plate.icon_path=path;
          if(!plate.icon_path.empty())draw_osd_quad(plate);
        } else if (color_chip) {
          OsdWidget chip = w;
          chip.icon_path.clear();
          draw_osd_quad(chip);
        } else if (!w.icon_path.empty()) draw_osd_quad(w);
      };
      for (const OsdWidget& w : widgets) draw_icon(w, 0);
      for (const OsdWidget& w : widgets) draw_icon(w, 1);
      for (const OsdWidget& w : widgets) draw_icon(w, 2);
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
                     int align, uint32_t argb)
{
    if (!font_key || text.empty() || width_ <= 0 || height_ <= 0) return;
    const SlrrFont* font = slrr_font_get(font_key);
    if (!font || !font->ready) return;
    bgfx::TextureHandle tex = tex_for_font(font_key);
    if (!bgfx::isValid(tex)) return;
    const float sw = static_cast<float>(width_);
    const float sh = static_cast<float>(height_);
    float pen_x = x;
    const float width_osd = 2.f * slrr_font_measure_px(font_key, text.c_str()) / sw;
    if (align == 1) pen_x -= width_osd * 0.5f;
    else if (align == 0) pen_x -= width_osd;
    const float cr = ((argb >> 16) & 0xff) / 255.f;
    const float cg = ((argb >> 8) & 0xff) / 255.f;
    const float cb = (argb & 0xff) / 255.f;
    const float ca = ((argb >> 24) & 0xff) / 255.f;
    const float col[4] = {cr, cg, cb, ca};
    const float u_at[4] = {0.f, 1.f, 1.f, 0.f};
    const float to_x = 2.f / sw;
    const float to_y = 2.f / sh;
    for (unsigned char ch : text) {
      const size_t gi = static_cast<size_t>(ch);
      const SlrrFontGlyph* g =
          gi < font->glyphs.size() ? &font->glyphs[gi] : nullptr;
      const float adv_px = (g && g->adv_px > 0.f)
                               ? g->adv_px
                               : static_cast<float>(font->px_height) * 0.35f;
      const bool draw =
          g && (g->u1 > g->u0 + 1e-6f) && (g->v1 > g->v0 + 1e-6f);
      if (draw) {
        const int corner[4] = {0, 1, 3, 2};
        MeshVertex v[4];
        auto setv = [](MeshVertex& o, float px, float py, float uu, float vv) {
          o.x = px;
          o.y = py;
          o.z = 0.f;
          o.nx = 0;
          o.ny = 0;
          o.nz = 1;
          o.u = uu;
          o.v = vv;
        };
        for (int i = 0; i < 4; ++i) {
          const int c = corner[i];
          setv(v[i], pen_x + g->px[c] * to_x, y + g->py[c] * to_y, g->tu[c],
               g->tv[c]);
        }
        const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
        if (bgfx::getAvailTransientVertexBuffer(4, layout_) < 4 ||
            bgfx::getAvailTransientIndexBuffer(6) < 6)
          break;
        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientVertexBuffer(&tvb, 4, layout_);
        bgfx::allocTransientIndexBuffer(&tib, 6);
        std::memcpy(tvb.data, v, sizeof(v));
        std::memcpy(tib.data, idx, sizeof(idx));
        float mtx[16];
        bx::mtxIdentity(mtx);
        bgfx::setTransform(mtx);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(&tib);
        bgfx::setTexture(0, s_tex_, tex);
        bind_baked_light(nullptr, nullptr);
        bgfx::setUniform(u_color_, col);
        bgfx::setUniform(u_alpha_test_, u_at);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                       BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
        bgfx::submit(osd_view_, program_);
      }
      pen_x += adv_px * to_x;
    }
  }
#endif
#ifdef _WIN32
  void LibBgfxBackend::draw_osd_string(float x, float y, int32_t align, uint32_t argb,
                       const std::string& text, void* font_key)
{
    if (text.empty() || !font_key || width_ <= 0 || height_ <= 0) return;
    const SlrrFont* font = slrr_font_get(font_key);
    if (!font || !font->ready) return;
    bgfx::TextureHandle tex = tex_for_font(font_key);
    if (!bgfx::isValid(tex)) return;
    const float sw = static_cast<float>(width_);
    const float sh = static_cast<float>(height_);
    float width_px = slrr_font_measure_px(font_key, text.c_str());
    const float width_osd = 2.f * width_px / sw;
    float pen_x = x;
    if (align == 1) pen_x -= width_osd * 0.5f;
    else if (align == 0) pen_x -= width_osd;
    const float top_y = y;
    const float cr = ((argb >> 16) & 0xff) / 255.f;
    const float cg = ((argb >> 8) & 0xff) / 255.f;
    const float cb = (argb & 0xff) / 255.f;
    const float ca = ((argb >> 24) & 0xff) / 255.f;
    const float col[4] = {cr, cg, cb, ca};
    const float u_at[4] = {0.f, 1.f, 1.f, 0.f};
    bgfx::setUniform(u_color_, col);
    bgfx::setUniform(u_alpha_test_, u_at);
    const float to_x = 2.f / sw;
    const float to_y = 2.f / sh;
    for (unsigned char ch : text) {
      const size_t gi = static_cast<size_t>(ch);
      const bool has = gi < font->glyphs.size();
      const SlrrFontGlyph* g = has ? &font->glyphs[gi] : nullptr;
      const float adv_px =
          (g && g->adv_px > 0.f) ? g->adv_px
                                 : static_cast<float>(font->px_height) * 0.35f;
      const bool draw =
          g && (g->u1 > g->u0 + 1e-6f) && (g->v1 > g->v0 + 1e-6f);
      const float adv_osd = adv_px * to_x;
      if (draw) {
        const int corner[4] = {0, 1, 3, 2};
        MeshVertex v[4];
        auto setv = [](MeshVertex& o, float px, float py, float uu, float vv) {
          o.x = px;
          o.y = py;
          o.z = 0.f;
          o.nx = 0;
          o.ny = 0;
          o.nz = 1;
          o.u = uu;
          o.v = vv;
        };
        for (int i = 0; i < 4; ++i) {
          const int c = corner[i];
          setv(v[i], pen_x + g->px[c] * to_x, top_y + g->py[c] * to_y, g->tu[c],
               g->tv[c]);
        }
        const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
        if (bgfx::getAvailTransientVertexBuffer(4, layout_) < 4 ||
            bgfx::getAvailTransientIndexBuffer(6) < 6)
          break;
        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientVertexBuffer(&tvb, 4, layout_);
        bgfx::allocTransientIndexBuffer(&tib, 6);
        std::memcpy(tvb.data, v, sizeof(v));
        std::memcpy(tib.data, idx, sizeof(idx));
        float mtx[16];
        bx::mtxIdentity(mtx);
        bgfx::setTransform(mtx);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(&tib);
        bgfx::setTexture(0, s_tex_, tex);
        bind_baked_light(nullptr, nullptr);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                       BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
        bgfx::submit(osd_view_, program_);
      }
      pen_x += adv_osd;
    }
    const float white_c[4] = {1.f, 1.f, 1.f, 1.f};
    bgfx::setUniform(u_color_, white_c);
  }
#endif

}  // namespace slrr
