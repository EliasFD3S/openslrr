#pragma once

// Internal to the libbgfx renderer. The public surface stays
// slrr/render_backend.hpp. Method bodies live in the sibling cpp files.

#include "slrr/render_backend.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/gameref.hpp"
#include "slrr/scene_sun.hpp"
#include "slrr/osd_registry.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace slrr {
struct VehicleHudState;

struct MeshVertex {
  float x, y, z;
  float nx, ny, nz;
  float u, v;
  float u2 = 0, v2 = 0, u3 = 0, v3 = 0;
};

class LibBgfxBackend final : public IRenderBackend {

  // Views 0..3 are shadow maps (their own framebuffers). The world is view 4,
  // or view 5 when an FMV backdrop occupies 4. Previews start at 8. The
  // interface stays on kOsdView, above those rects.
  static constexpr int kShadowSlots = 4;
  static constexpr uint16_t kShadowSize = 1024;
  static constexpr uint16_t kWorldView = 4;
  static constexpr uint16_t kFmvLayer = 5;
  static constexpr uint16_t kOsdUnder = 6;
  static constexpr uint16_t kPreview0 = 8;
  static constexpr uint16_t kOsdView = 70;

 public:
  const char* name() const override { return "bgfx"; }

  bool open(int32_t width, int32_t height, const char* title) override;


  void close() override;


  bool ready() const override { return ready_; }

  void pump(int32_t /*ms*/) override;


  bool quit_requested() const override { return quit_; }
  void request_quit() override { quit_ = true; }
  bool request_screenshot(const std::string& path) override;

  void flush() override;


  int32_t width() const override { return width_; }
  int32_t height() const override { return height_; }

  void set_scx_smoke(const std::string& path);


  void set_garage_viewport(bool enabled, int32_t osd_buttons);


  void set_osd_overlay(bool enabled) { osd_overlay_ = enabled; }
  bool osd_draw_enabled() const { return garage_ || osd_overlay_; }

  void* native_hwnd() const { return hwnd_; }

  void set_scx_mesh(int32_t version, int32_t groups, int32_t vertices,
                    int32_t triangles, const float bbox_min[3],
                    const float bbox_max[3]);


  void set_scx_geometry(ScxGeometry geo);


  void clear_scene();


  void add_scx_geometry(ScxGeometry geo);


 private:
  struct ScenePart {
    ScxGeometry geo;
    std::shared_ptr<const ScxGeometry> source;
    bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle ibh = BGFX_INVALID_HANDLE;
    std::vector<bgfx::TextureHandle> tex;
  };
  struct Preview { GameRefState camera; GameRefState viewport; std::vector<SceneInstance> instances; };
  // Projection depends on scene geometry, never on the viewing camera.
  // Keep parked cars out of the per-vertex floor queries on subsequent frames.
  struct ProjectedShadow {
    struct Stamp {
      void* key = nullptr;
      std::shared_ptr<const ScxGeometry> source;
      std::array<float, 16> transform{};
    };
    std::vector<Stamp> stamps;
    std::array<float, 3> light{};
    void* owner = nullptr;
    float ceiling = 0.f;
    uint64_t floor_revision = 0;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
  };
  std::unordered_map<void*, ProjectedShadow> projected_shadows_;
  std::vector<SceneInstance> instances_;
  std::vector<Preview> previews_;
  std::unordered_map<const ScxGeometry*,ScenePart> instance_meshes_;

  void sync_instances();

  // Diffuse * baked lightmap, the way the exe draws a map. No lightmap
  // (OSD, FMV, a mesh without the slot) keeps the existing shading.
  void bind_baked_light(const ScxGroup* group,
                        const std::vector<bgfx::TextureHandle>* tex);


  void draw_instance(uint16_t view_id,const SceneInstance& instance,const float* matrix,bool preview,int pass);

  void draw_dynamic_world(const float* view, uint16_t view_id);

  void draw_vehicle_shadows(uint16_t view_id);

  void draw_previews();


  void expand_scene_bbox(const float mn[3], const float mx[3]);


  void destroy_part(ScenePart& p);

  void build_camera(float* view, float* proj);


  void upload_part(ScenePart& part);


  static bool is_logo_texture_slot(const ScxGeometry& geo, uint16_t tex_i);


  static bool is_logo_group(const ScxGroup& g, const ScxGeometry& geo);


  // Horizontal mirror (u → 1-u) for banner/logo groups only.
  static void flip_logo_uv_horizontal(ScxGeometry& geo);


  void draw_scene(uint16_t view_id);


  void load_part_textures(ScenePart& part);


#ifdef _WIN32
  static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  bgfx::TextureHandle tex_for_osd_icon(const std::string& path);


  void draw_osd_quad(const OsdWidget& w, float cr = 1.f, float cg = 1.f,
                     float cb = 1.f, float ca = 1.f);


  bgfx::TextureHandle tex_for_font(void* font_key);


  void draw_osd_texts(int32_t only_layer, bool filter_layer, bool behind_ui);


  // One OSD line. y is the glyph top (y+ down). align 1 centers on x.
  void draw_osd_line(void* font_key, const std::string& text, float x, float y,
                     int align, uint32_t argb);


  // Osd.animate builds this after idleFor(1). The host never runs that
  // loop except for the credits scroller, so the plate is drawn here.
  // Row titles live in tip too; they are already painted beside the slider.
  void draw_tooltip();


  void draw_fmv(uint16_t view_id);


  // Draw a string in OSD text-space (same path as SlrrOsdText glyphs).
  void draw_osd_string(float x, float y, int32_t align, uint32_t argb,
                       const std::string& text, void* font_key);


  void draw_osd();
  void draw_vehicle_hud(const VehicleHudState& hud);
  std::string gps_hud_path_;
  std::unordered_map<std::string, ScxGeometry> hud_preview_meshes_;


  HWND hwnd_ = nullptr;
  bool shown_ = false;
  int mouse_x_ = 0;
  int mouse_y_ = 0;
  int32_t tip_hover_ = -2;
  float tip_since_ = 0.f;
  bool tip_font_tried_ = false;
  int lmb_x_ = 0;
  int lmb_y_ = 0;
  bool have_mouse_ = false;
  bool raw_mouse_ = false;
  bool camera_cursor_clipped_ = false;
  bool lmb_down_ = false;
  int32_t slider_drag_index_ = -1;
  Object* inventory_drag_source_ = nullptr;
  bool inventory_dragging_ = false;
  char inventory_drag_cursor_ = 'J';
#endif

  bool ready_ = false;
  bool quit_ = false;
  bool garage_ = false;
  bool osd_overlay_ = false;
  // Openingpic stays up after the boot movie. Once the menu or its movie
  // has covered it, a later empty frame must not bring that still back.
  bool opening_retired_ = false;
  bool scx_ok_ = false;
  bool mesh_ok_ = false;
  int32_t osd_buttons_ = 0;
  uint16_t osd_view_ = kOsdUnder;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t frames_ = 0;
  float mesh_min_[3] = {0, 0, 0};
  float mesh_max_[3] = {0, 0, 0};
  std::vector<ScenePart> parts_;
  bgfx::VertexLayout layout_;
  bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle white_tex_ = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle fmv_tex_ = BGFX_INVALID_HANDLE;
  int32_t fmv_tex_w_ = 0;
  uint64_t fmv_tex_serial_ = 0;
  int32_t fmv_tex_h_ = 0;
  bgfx::UniformHandle s_tex_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_lightmap_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_lightmap_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_lightmap_uv_ = BGFX_INVALID_HANDLE;
  std::unordered_map<std::string, bgfx::TextureHandle> osd_tex_;
  std::unordered_map<void*, bgfx::TextureHandle> font_tex_;
  bgfx::UniformHandle u_alpha_test_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_color_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle vehicle_program_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_mix_=BGFX_INVALID_HANDLE,s_env_=BGFX_INVALID_HANDLE,s_bake_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_layers_=BGFX_INVALID_HANDLE,u_channels_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_base_uv_=BGFX_INVALID_HANDLE,u_mix_uv_=BGFX_INVALID_HANDLE,u_env_uv_=BGFX_INVALID_HANDLE,u_diffuse_=BGFX_INVALID_HANDLE,u_spec_=BGFX_INVALID_HANDLE,u_sun_diff_=BGFX_INVALID_HANDLE,u_sun_amb_=BGFX_INVALID_HANDLE,u_sun_spec_=BGFX_INVALID_HANDLE,u_sun_dir_=BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle shadow_program_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle shadow_recv_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_shadow_mtx_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_shadow_ramp_ = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle shadow_tex_[4] = {
      BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE,
      BGFX_INVALID_HANDLE};
  bgfx::FrameBufferHandle shadow_fb_[4] = {
      BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE,
      BGFX_INVALID_HANDLE};

};

}  // namespace slrr
