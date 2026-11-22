#pragma once

// Internal to the libbgfx renderer. The public surface stays
// slrr/render_backend.hpp. Method bodies live in the sibling cpp files.

#include "debug_overlay.hpp"
#include "osd_text_batch.hpp"
#include "scene_mesh_data.hpp"
#include "slrr/dds_image.hpp"
#include "slrr/gameref.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/scene_light.hpp"
#include "render_debug.hpp"
#include "slrr/scene_sun.hpp"
#include <optional>

#include <bgfx/bgfx.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
  uint32_t rgba = 0xffffffffu;
};

class LibBgfxBackend final : public IRenderBackend {

  // Map cascades use views 0/2/3/4; vehicle depth uses view 1.
  // The world is view 5 or 6, depending on the FMV backdrop. Previews start at 8. The
  // interface stays on kOsdView, above those rects. Point shadows alternatively
  // use 0/1/2/3/4/64; reserved view 64 is reordered before the world.
  static constexpr uint16_t kShadowResolution = 2048;
  static constexpr uint16_t kStaticShadowView = 0;
  static constexpr uint16_t kVehicleShadowFirst = 1;
  static constexpr uint16_t kWorldView = 5;
  static constexpr uint16_t kFmvLayer = 6;
  static constexpr uint16_t kOsdUnder = 7;
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
  void set_debug_context(Jvm& jvm, const SectionMachine& sections) {
    debug_overlay_.set_context(jvm, sections);
  }
  void set_debug_startup(bool vsync, bool overlay) {
    debug_overlay_.render_options().vsync = vsync;
    debug_overlay_.set_visible(overlay);
  }


  void add_scx_geometry(ScxGeometry geo);


 private:
  friend struct SceneMeshCacheRegression;
  friend struct SceneShadowRegression;
  DebugOverlay debug_overlay_;
  bool debug_was_visible_ = false;
  struct ScenePart {
    SceneMeshData geo;
    std::vector<ScxGroup> shadow_groups;
    bool merge_groups = false;
    std::shared_ptr<const ScxGeometry> source;
    bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle skin_vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle ibh = BGFX_INVALID_HANDLE;
    std::vector<bgfx::TextureHandle> tex;
    std::vector<std::string> shared_texture_paths;
    uint32_t revision = 0;
  };
  struct SharedMapTexture {
    bgfx::TextureHandle handle;
    size_t users;
  };
  std::unordered_map<std::string, SharedMapTexture> map_textures_;
  struct Preview { GameRefState camera; GameRefState viewport; std::vector<SceneInstance> instances; };
  std::vector<SceneInstance> instances_;
  std::vector<SceneBatch> prop_batches_;
  std::vector<SceneBatch> night_batches_;  // night overlays per mesh (#264)
  std::vector<Preview> previews_;
  std::unordered_map<const ScxGeometry*,ScenePart> instance_meshes_;
  std::unordered_map<void*, const ScxGeometry*> deformed_meshes_;
  // What sync_instances marked used last time (#262). When the list, the
  // revisions and the number of resident meshes are unchanged, nothing would
  // be uploaded or released, so the used map and the sweep are skipped.
  struct ResidencyUse {
    const void* key = nullptr;
    const void* owner = nullptr;
    uint32_t revision = 0;
    bool deformed = false;
    bool operator==(const ResidencyUse&) const = default;
  };
  std::vector<ResidencyUse> residency_uses_, residency_scratch_;
  size_t residency_count_ = SIZE_MAX;  // instance_meshes_.size() after the sweep
  uint64_t residency_skips_ = 0;       // for tests
  bool residency_cache_ = true;        // tests: false rebuilds every frame

  void cache_scene_mesh(const std::shared_ptr<const ScxGeometry>& source,
                        void* owner, bool map_prop);
  void sync_instances(const float* view, const float* projection);
  static SceneFrustum view_frustum(const float* view, const float* projection);
  void apply_video_mode(const VideoMode& mode);
  void update_static_shadow(const float* eye, const VideoOptions& video);
  void update_static_shadow_inner(const float* eye, const VideoOptions& video);

  // Diffuse * baked lightmap, the way the exe draws a map. No lightmap
  // (OSD, FMV, a mesh without the slot) keeps the existing shading.
  uint32_t world_sampler() const {
    return host_enhanced_graphics().anisotropic_filtering && render_debug().anisotropic
        ? BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC : BGFX_SAMPLER_NONE;
  }
  void publish_debug_stats();
  // Per-view light uniforms (#246). bgfx applies the uniforms set before a
  // submit to that draw only, into a cache that persists across draws, in
  // render order. In a Sequential view render order is submit order, so the
  // view-invariant light arrays (fixtures, lamps, lamp/car shadow tiles, sun
  // cascades, point faces) are set on the first lit draw of each view and
  // frame and skipped after. Unknown or sorted views keep uploading per draw.
  std::bitset<256> sequential_views_;
  uint64_t light_uniform_revision_ = 1;
  bool light_array_cache_ = true;     // tests: false uploads on every draw
  void set_view_sequential(uint16_t view) {
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    sequential_views_.set(view);
  }
  // Per-view uniform values (#262), the same rule for the single-value sun,
  // fog, sky, shadow, interior sun, point/local light, car depth, AO and
  // count uniforms. Inside a BindingScope of a Sequential view a value equal
  // to the one already set in that view this frame is skipped. A set outside
  // such a scope forgets that handle; a scope that ends with values set after
  // its last submit, or a submit bgfx drops (no indices), forgets them all,
  // because those values reach whichever submit comes next, or none.
  struct UniformSlot {
    uint32_t generation = 0;
    std::array<float, 16> value{};
  };
  // Texture stages kept across submits (#262). In the same scope, submits use
  // BGFX_DISCARD_ALL without BGFX_DISCARD_BINDINGS, so a stage keeps its
  // texture and only stages that change are set again. A draw that leaves a
  // kept stage unset is submitted with a full discard, as before, so no draw
  // samples a texture it did not ask for. The scope ends with
  // discard(BGFX_DISCARD_BINDINGS): later code starts with no bindings.
  struct TextureSlot {
    bool valid = false;
    uint16_t sampler = 0, texture = 0;
    uint32_t flags = 0;
  };
  static constexpr size_t kTextureStages = 16;
  // Ground POLY vertex colours are baked light (#275): grey, mostly 0 or 1,
  // darker on slopes away from the original's sun. Outdoors without dynamic
  // lighting they are the scene's light (fs_mesh multiplies them in). Under
  // the dynamic sun they would light the terrain twice and leave the baked
  // dark areas black whatever the sun does, so the sun and its shadows
  // replace them.
  bool baked_vertex_light(bool ground_polygons) const {
    return ground_polygons && outdoor_lighting_ && !host_enhanced_graphics().dynamic_lighting;
  }
  // Below this night factor street lamps do not light (#271), and below the
  // second they cast no shadow tile.
  static constexpr float kLampNightMin = .01f;
  static constexpr float kLampShadowNightMin = .3f;
  static constexpr size_t kPropInstanceChunk = 1024;  // was 128 before #262
  // Everything the binding caches (#246, #262) know about the encoder they
  // feed (#270). The host thread uses main_bind_ with encoder 0; a worker
  // that submits a view on its own encoder binds its own state, so the
  // caches always describe the encoder that receives the values.
  struct BindState {
    uint16_t binding_view = UINT16_MAX;  // view of the draw being bound
    uint16_t light_arrays_view = UINT16_MAX;
    int32_t light_arrays_frame = -1;
    uint64_t light_arrays_revision = 0;
    uint64_t light_array_uploads = 0;  // for tests and the debug menu
    std::vector<UniformSlot> uniform_values;
    uint32_t uniform_generation = 1;
    uint16_t uniform_view = UINT16_MAX;
    int32_t uniform_frame = -1;
    bool uniform_pending = false;   // cached values set since the last submit
    bool uniform_scope = false;     // inside a Sequential BindingScope
    uint64_t uniform_sets = 0, uniform_skips = 0;  // for tests
    std::array<TextureSlot, kTextureStages> texture_bindings{};
    uint32_t texture_set_mask = 0;  // stages the current draw set
    bool texture_run = false;
    uint64_t texture_sets = 0, texture_skips = 0;  // for tests
  };
  BindState main_bind_;
  static thread_local BindState* t_bind_;
  static thread_local bgfx::Encoder* t_encoder_;
  BindState& bind() { return t_bind_ ? *t_bind_ : main_bind_; }
  const BindState& bind() const { return t_bind_ ? *t_bind_ : main_bind_; }
  // The encoder of the current thread: a worker's own (bgfx::begin(true)),
  // else encoder 0, which bgfx::begin() returns on the API thread (#270).
  // Encoder 0 is a fixed member of the bgfx context: cached once instead of
  // calling bgfx::begin() for every draw call.
  static bgfx::Encoder* host_encoder_;
  static bgfx::Encoder* enc() {
    if (t_encoder_) return t_encoder_;
    if (!host_encoder_) host_encoder_ = bgfx::begin();
    return host_encoder_;
  }
  bool light_arrays_current() const {
    const auto& state = bind();
    return light_array_cache_ && state.binding_view < sequential_views_.size() &&
           sequential_views_.test(state.binding_view) &&
           state.light_arrays_view == state.binding_view &&
           state.light_arrays_frame == frames_ &&
           state.light_arrays_revision == light_uniform_revision_;
  }
  bool uniform_cache_ = true;      // tests: false sets every value
  void set_uniform(bgfx::UniformHandle handle, const float* value, uint16_t floats = 4);
  void forget_uniforms() {
    ++bind().uniform_generation;
    bind().light_arrays_view = UINT16_MAX;
  }
  bool texture_cache_ = true;      // tests: false sets every stage
  void set_texture(uint8_t stage, bgfx::UniformHandle sampler, bgfx::TextureHandle texture,
                   uint32_t flags = UINT32_MAX);
  void forget_textures() {
    for (auto& slot : bind().texture_bindings) slot.valid = false;
  }
  // Draws submitted per view this frame (#264), for the debug snapshot.
  // Every bgfx::submit of the renderer goes through submit_counted.
  std::array<uint32_t, 256> view_draws_{};
  void submit_counted(uint16_t view, bgfx::ProgramHandle program,
                      uint8_t flags = BGFX_DISCARD_ALL) {
    if (view < view_draws_.size()) ++view_draws_[view];
    enc()->submit(view, program, 0, flags);
  }
  // Worker encoders (#270). During a frame whose passes may run on workers,
  // the AO depth prepass, the car lamp tiles and each reflection face queue
  // one job per view instead of submitting; run_worker_submits runs them on
  // the job pool, each on its own encoder (bgfx::begin(true)) with its own
  // BindState, while the host thread submits the world on encoder 0. Outside
  // such a frame (tests, menus) queue_or_run submits at once, as before.
  bool parallel_submission_ = true;    // tests: false keeps one thread
  bool defer_submits_ = false;         // queue the worker passes
  bool parallel_window_ = false;       // workers running: no shared cache writes
  std::vector<std::function<void()>> worker_jobs_;
  std::vector<std::unique_ptr<BindState>> worker_binds_;
  uint64_t worker_submit_runs_ = 0;    // frames whose passes ran on workers, for tests
  void queue_or_run(std::function<void()> job) {
    if (defer_submits_) worker_jobs_.push_back(std::move(job));
    else job();
  }
  bool parallel_submission_ready() const;
  void run_worker_submits(const std::function<void()>& host_work);
  // Texture lookups by path (replacements, paint) create textures on a miss.
  // During the parallel window they read a snapshot taken on the host thread.
  std::unordered_map<std::string, bgfx::TextureHandle> resolved_textures_;
  bgfx::TextureHandle resolve_texture(const std::string& path);
  void snapshot_textures(const std::vector<SceneInstance>& list);
  // Every submit inside a BindingScope goes through here.
  void submit_bound(uint16_t view, bgfx::ProgramHandle program, uint32_t index_count);
  // The view of the draw being bound (#246, #262): draw_instance and
  // draw_prop_batches open one around their submits.
  struct BindingScope {
    LibBgfxBackend& backend;
    BindingScope(LibBgfxBackend& b, uint16_t view);
    ~BindingScope();
    BindingScope(const BindingScope&) = delete;
    BindingScope& operator=(const BindingScope&) = delete;
  };
  void bind_scene_shadow(bool receive, uint8_t stage);
  void bind_fixture_lights(bool enabled, bool upload_arrays);
  // Per-frame light data texture (#254), see shaders/scene_shadow.sh.
  static constexpr uint16_t kLightDataWidth = 256, kLightDataRows = 3;
  bgfx::TextureHandle light_data_tex_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_light_data_ = BGFX_INVALID_HANDLE;
  std::vector<float> light_data_;      // last uploaded contents
  uint64_t light_data_updates_ = 0;    // texture updates, for tests
  void upload_light_data();
  void bind_baked_light(const ScxGroup* group,
                        const std::vector<bgfx::TextureHandle>* tex,
                        bgfx::TextureHandle mix = BGFX_INVALID_HANDLE,
                        bool receive_static_shadow = false, const SceneSun* sun = nullptr, const float* sky = nullptr);


  void draw_instance(uint16_t view_id,const SceneInstance& instance,const float* matrix,bool preview,int pass,
                     const bgfx::InstanceDataBuffer* instance_data = nullptr);
  size_t draw_vehicle_instances(uint16_t view_id, const std::vector<const SceneInstance*>& instances);
  struct VehicleGroup {
    size_t span = 0;  // never reorder opaque cars across a pass-0 blend
    size_t next = 0, instanced = 0;
    std::vector<const SceneInstance*> instances;
  };
  std::vector<VehicleGroup> vehicle_groups_;
  std::vector<size_t> vehicle_group_indices_;
  // `shadow_local` < 0 reads static_shadow_local_; 0 / 1 override it, so a
  // worker pass never toggles a member the host thread reads (#270).
  void draw_prop_batches(uint16_t view_id, const std::vector<SceneBatch>& batches,
                         bool shadow, bool conservative = false, bool reversed_depth = false,
                         int shadow_local = -1);
  // Map props that draw_prop_batches cannot take, instanced per mesh in the
  // world view (#264): night overlays (`emissive`, from night_batches_) and
  // props with a blended section (grouped from instances_). Same material
  // rules as draw_instance for a map placement; `pass` as in draw_instance.
  struct PropPlacement {
    const float* matrix;
    uint32_t color;
  };
  void draw_props_instanced(uint16_t view_id, const ScenePart& part, const SceneSun& lighting,
                            bool emissive, int pass, const PropPlacement* placements,
                            size_t count);
  // A placement draw_dynamic_world draws through draw_props_instanced.
  static bool blended_prop_instance(const SceneInstance& instance);
  struct BlendedPropGroup {
    const ScxGeometry* geometry = nullptr;
    const SceneSun* lighting = nullptr;
    std::vector<PropPlacement> placements;
  };
  std::vector<BlendedPropGroup> blended_groups_;  // rebuilt per world draw, capacity kept
  std::vector<PropPlacement> placement_scratch_;
  void refresh_distance_fog(const float render_eye[3]);

  void draw_sky(uint16_t view_id, const float* view, const float* projection);
  void draw_dynamic_world(uint16_t view_id);
  void draw_sun_flare(const float* view, const float* projection, uint16_t view_id);
  void draw_nitro_flares(const float* view, const float* projection, uint16_t view_id);

  void draw_previews();


  void expand_scene_bbox(const float mn[3], const float mx[3]);


  void destroy_part(ScenePart& p);

  void build_camera(float* view, float* proj);


  void upload_part(ScenePart& part);
  void upload_skin(ScenePart& part);
  static bool is_logo_texture_slot(const ScxGeometry& geo, uint16_t tex_i);


  static bool is_logo_group(const ScxGroup& g, const ScxGeometry& geo);


  // Horizontal mirror (u → 1-u) for banner/logo groups only.
  static void flip_logo_uv_horizontal(ScxGeometry& geo);


  void draw_scene(uint16_t view_id);


  void load_part_textures(ScenePart& part, bool share_map_textures = false);
  // A mesh's texture files, one per slot ("" for a slot without a file).
  static std::vector<std::string> part_texture_paths(const ScxGeometry& geo);
  // Decodes, on the job pool, the files that load_part_textures will read for
  // these new meshes (geometry, shares map textures). It then takes each
  // image from here instead of reading the file on the host thread.
  void prefetch_part_textures(const std::vector<std::pair<const ScxGeometry*, bool>>& meshes);
  struct PrefetchedTexture {
    DdsImage image;
    size_t uses = 0;  // meshes still to load it; the image is freed at zero
  };
  std::unordered_map<std::string, PrefetchedTexture> prefetched_textures_;


#ifdef _WIN32
  static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  bgfx::TextureHandle tex_for_osd_icon(const std::string& path);
  // Painter textures (paint_texture.hpp), uploaded again when their pixels
  // change. tex_for_osd_icon routes their "paint:" paths here.
  bgfx::TextureHandle paint_texture_handle(uint32_t id);


  void draw_osd_quad(const OsdWidget& w, float cr = 1.f, float cg = 1.f,
                     float cb = 1.f, float ca = 1.f);


  bgfx::TextureHandle tex_for_font(void* font_key);


  void draw_osd_texts(int32_t only_layer, bool filter_layer, bool behind_ui);


  // One OSD line. y is the glyph top (y+ down). align 1 centers on x.
  void draw_osd_line(void* font_key, const std::string& text, float x, float y,
                     int align, uint32_t argb, const SlrrFontEffects* effects = nullptr);


  // Osd.animate builds this after idleFor(1). The host never runs that
  // loop except for the credits scroller, so the plate is drawn here.
  // Row titles live in tip too; they are already painted beside the slider.
  void draw_tooltip();


  void draw_fmv(uint16_t view_id);


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
  // HostInput::pointer_epoch when the left/right button went down.
  uint32_t lmb_epoch_ = 0;
  uint32_t rmb_epoch_ = 0;
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
  bgfx::VertexLayout text_layout_;
  bgfx::ProgramHandle text_program_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_font_params_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_font_outline_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_font_shadow_ = BGFX_INVALID_HANDLE;
  std::vector<OsdTextVertex> text_vertices_;
  std::vector<uint16_t> text_indices_;
  bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle inst_program_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle inst_shadow_program_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle skin_program_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_skin_bones_ = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle white_tex_ = BGFX_INVALID_HANDLE;
  // 1x1 far depth. The mesh shader's shadow sampler is a comparison
  // sampler, including the draws that do not receive a shadow.
  bgfx::TextureHandle depth_white_ = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle fmv_tex_ = BGFX_INVALID_HANDLE;
  int32_t fmv_tex_w_ = 0;
  uint64_t fmv_tex_serial_ = 0;
  int32_t fmv_tex_h_ = 0;
  bgfx::UniformHandle s_tex_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_lightmap_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_mesh_mix_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_lightmap_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_lightmap_uv_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_mesh_base_uv_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_mesh_mix_uv_ = BGFX_INVALID_HANDLE;
  std::unordered_map<std::string, bgfx::TextureHandle> osd_tex_;
  struct PaintTextureGpu {
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    uint32_t revision = 0, width = 0, height = 0;
    bool mips = false;
  };
  std::unordered_map<uint32_t, PaintTextureGpu> paint_tex_;
  void release_paint_textures(bool all);
  std::unordered_map<std::string, bgfx::TextureHandle> font_tex_;
  bgfx::UniformHandle u_alpha_test_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_color_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle vehicle_program_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle vehicle_inst_program_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_mix_=BGFX_INVALID_HANDLE,s_env_=BGFX_INVALID_HANDLE,s_bake_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_layers_=BGFX_INVALID_HANDLE,u_channels_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_base_uv_=BGFX_INVALID_HANDLE,u_mix_uv_=BGFX_INVALID_HANDLE,u_env_uv_=BGFX_INVALID_HANDLE,u_diffuse_=BGFX_INVALID_HANDLE,u_spec_=BGFX_INVALID_HANDLE,u_sun_diff_=BGFX_INVALID_HANDLE,u_sun_amb_=BGFX_INVALID_HANDLE,u_sun_spec_=BGFX_INVALID_HANDLE,u_sun_dir_=BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle static_shadow_program_ = BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle static_shadow_fb_ = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle static_shadow_tex_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_static_shadow_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_static_shadow_mtx_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_static_shadow_params_ = BGFX_INVALID_HANDLE;
  std::array<float, 16> static_shadow_matrix_{};
  bgfx::UniformHandle u_cloud_wind_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_cloud_sky_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_fog_color_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_fog_range_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_fog_eye_ = BGFX_INVALID_HANDLE;
  bool fog_enabled_ = false;
  float fog_near_ = 1500.f;
  float fog_far_ = 3000.f;
  float fog_color_[3] = {0.57f, 0.73f, 0.86f};
  float fog_eye_[3] = {};
  bool outdoor_lighting_ = false;
  bgfx::UniformHandle u_local_light_ = BGFX_INVALID_HANDLE;
  std::array<float, 4> local_light_{};
  void update_local_light();
  void update_scenery_lights();
  void update_ambient_occlusion(const float* view,const float* projection,uint16_t width,uint16_t height,uint16_t world_view);
  void bind_ambient_occlusion(bool enabled);
  void destroy_ao_targets();
  bgfx::ProgramHandle ao_program_=BGFX_INVALID_HANDLE,ao_skin_depth_program_=BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle ao_depth_fb_=BGFX_INVALID_HANDLE,ao_raw_fb_=BGFX_INVALID_HANDLE,ao_filter_fb_=BGFX_INVALID_HANDLE;
  bgfx::TextureHandle ao_depth_tex_=BGFX_INVALID_HANDLE,ao_raw_tex_=BGFX_INVALID_HANDLE,ao_filter_tex_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_ao_depth_=BGFX_INVALID_HANDLE,s_ao_input_=BGFX_INVALID_HANDLE,s_ao_field_=BGFX_INVALID_HANDLE;
  // AO geometry pass (#256): AO-resolution linear depth (R32F) and normal
  // (RGBA32F), written by view 141 and read by AO pass 1 and the filters.
  bgfx::UniformHandle s_ao_linear_=BGFX_INVALID_HANDLE,s_ao_normal_=BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle ao_geometry_fb_=BGFX_INVALID_HANDLE;
  bgfx::TextureHandle ao_linear_tex_=BGFX_INVALID_HANDLE,ao_normal_tex_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_ao_screen_=BGFX_INVALID_HANDLE,u_ao_projection_=BGFX_INVALID_HANDLE,u_ao_config_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_ao_params_=BGFX_INVALID_HANDLE,u_ao_sampling_=BGFX_INVALID_HANDLE,u_ao_field_size_=BGFX_INVALID_HANDLE;
  std::array<float,4> ao_projection_{};
  uint16_t ao_width_=0,ao_height_=0,ao_screen_width_=0,ao_screen_height_=0,ao_world_view_=UINT16_MAX;
  bool ao_active_=false;

  static constexpr size_t kFixtureLights=12;
  std::vector<SpotLight> fixture_lights_;
  void update_fixture_shadows(const VideoOptions& video);
  void update_lamp_shadows(const VideoOptions& video);
  bgfx::FrameBufferHandle fixture_shadow_fb_=BGFX_INVALID_HANDLE;
  bgfx::TextureHandle fixture_shadow_tex_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_fixture_shadow_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_fixture_params_=BGFX_INVALID_HANDLE;
  // Scenery lamps (#237), outside garages. kSceneryLamps matches the arrays
  // in shaders/scene_shadow.sh (3 x 64 vec4 of the fragment constants).
  static constexpr size_t kSceneryLamps=64;
  std::vector<LampLight> scenery_lamps_;
  // Car lights (#279) lead the unshadowed part of scenery_lamps_: at most
  // kCarLights, the followed car's first, then by distance to the eye.
  static constexpr size_t kCarLights=16;
  // Tuned in scene_shadow_gpu case car_lights; see KNOWLEDGE_BASE #279.
  static constexpr float kCarLightMaxDistance=150.f;
  static constexpr float kHeadlightIntensity=3.f, kHeadlightRange=60.f;
  static constexpr float kHeadlightInnerDegrees=14.f, kHeadlightOuterDegrees=30.f;
  static constexpr float kBrakeLightIntensity=1.5f, kBrakeLightRange=6.f, kBrakeLightQuadratic=.3f;
  static constexpr float kBrakeLightInnerDegrees=55.f, kBrakeLightOuterDegrees=85.f;
  size_t car_lamp_count_=0;
  void update_car_lights();
  // Harness hook: when set, used instead of part_car_lights().
  std::optional<std::vector<CarLight>> car_lights_override_;
  bgfx::UniformHandle u_lamp_params_=BGFX_INVALID_HANDLE;
  std::array<std::array<float,16>,kFixtureLights> fixture_shadow_matrices_{},fixture_cached_matrices_{};
  bool fixture_shadow_active_=false;
  uint16_t fixture_shadow_tile_=512;
  uint64_t fixture_shadow_revision_=0;

  bool bind_point_light(bool enabled);
  std::optional<PointLight> point_light_, cached_point_light_;
  bgfx::UniformHandle u_point_light_color_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_point_shadow_faces_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_point_shadow_params_ = BGFX_INVALID_HANDLE;
  std::array<std::array<float, 16>, 6> point_shadow_matrices_{};
  bool point_shadow_active_ = false;
  void update_interior_sun(const VideoOptions& video);
  void bind_interior_sun(bool enabled, uint8_t stage);
  bgfx::FrameBufferHandle interior_sun_fb_ = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle interior_sun_tex_ = BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle interior_aperture_fb_ = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle interior_aperture_tex_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_sun_aperture_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_interior_sun_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_interior_sun_mtx_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_interior_sun_dir_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_interior_sun_color_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_aperture_params_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_aperture_matrices_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_aperture_uv_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_aperture_edge_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_aperture_ = BGFX_INVALID_HANDLE;
  std::array<float,16> interior_sun_matrix_{};
  SceneSun interior_sun_;
  bool interior_sun_active_ = false;
  uint16_t interior_sun_size_=1024;
  uint16_t interior_sun_mask_ = UINT16_MAX;
  std::array<float,16> interior_sun_cached_matrix_{};

  void update_reflections(const float* eye);
  void destroy_reflections();
  std::array<bgfx::TextureHandle, 2> reflection_cubes_{{
      BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE}};
  size_t reflection_write_ = 0;
  bgfx::TextureHandle reflection_depth_ = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle reflection_color_ = BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle reflection_capture_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_reflection_cube_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_dynamic_reflection_ = BGFX_INVALID_HANDLE;
  std::array<float, 3> reflection_origin_{}, reflection_sample_origin_{};
  std::array<float, 3> reflection_box_min_{}, reflection_box_max_{};
  bool reflection_box_valid_ = false;
  bgfx::UniformHandle u_reflection_probe_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_reflection_box_min_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_reflection_box_max_ = BGFX_INVALID_HANDLE;
  void* reflection_world_ = nullptr;
  size_t reflection_face_ = 0;
  bool reflection_ready_ = false;
  bool reflection_scene_ = false;
  bgfx::UniformHandle u_shadow_cascades_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_shadow_origin_ = BGFX_INVALID_HANDLE;
  std::array<float,3> shadow_origin_{};
  void* shadow_origin_world_ = nullptr;
  bgfx::UniformHandle u_shadow_filter_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_shadow_ranges_ = BGFX_INVALID_HANDLE;
  std::array<std::array<float,16>,4> shadow_cascade_matrices_{};
  struct ShadowCasterStamp {
    const ScxGeometry* source = nullptr;
    uint32_t revision = 0;
    uint16_t vertex = UINT16_MAX, index = UINT16_MAX;
    std::array<float,16> transform{};
    bool operator==(const ShadowCasterStamp&) const = default;
  };
  std::vector<ShadowCasterStamp> fixture_shadow_casters_;
  std::vector<ShadowCasterStamp> interior_sun_casters_;
  // Street lamp shadows (#237): the nearest kLampShadows lamps each get one
  // downward 150-degree perspective tile in the fixture atlas (free outside
  // garages). Static scenery is collected once per assignment; the tile is
  // redrawn only when its lamp or a dynamic caster (vehicle, character)
  // changes. Weights fade in/out so reassignment does not pop.
  // kLampShadows lamps are wanted at once; the extra slots let a new lamp
  // fade in while the one it replaces fades out.
  static constexpr size_t kLampShadows=6, kLampShadowSlots=8;
  static constexpr float kLampShadowFov=150.f, kLampShadowNear=.3f, kLampShadowFade=.8f;
  // Selection around the focus (car, else camera): lamps within the light
  // range plus 10 m; a held lamp is replaced only by one at most 75 % as far
  // and at least 4 m nearer, after holding its slot for 1 s.
  static constexpr float kLampShadowReach=55.f, kLampShadowSwitchRatio=.75f,
                         kLampShadowSwitchMargin=4.f, kLampShadowHold=1.f;
  struct LampShadowSlot {
    const void* source=nullptr;
    std::array<float,3> position{};
    float range=0.f, weight=0.f, distance=0.f, hold=0.f;
    LampLight lamp{};  // lit even when outside the frustum-selected set
    bool wanted=false, valid=false;
    std::vector<SceneInstance> statics;
    std::vector<SceneBatch> static_batches;
    std::vector<ShadowCasterStamp> stamps;
    std::array<float,16> sample{};
  };
  std::array<LampShadowSlot,kLampShadowSlots> lamp_shadow_slots_{};
  // Car shadows (#237): every lamp whose light reaches the followed car gets
  // a car-only tile (up to kCarShadowTiles), a narrow perspective from the
  // lamp onto a kCarShadowCover sphere around the car, holding only the
  // vehicles inside it. The 16 tiles are the half-size quarters of atlas
  // tiles 8-11, drawn in one view with per-draw clip remapping and scissor.
  // Scenery tiles then skip vehicles inside that sphere.
  static constexpr size_t kCarShadowTiles=16, kLampSpecial=kLampShadowSlots+kCarShadowTiles;
  static constexpr float kCarShadowCover=6.f, kCarShadowFade=10.f;
  static constexpr uint16_t kCarShadowView=136;
  // Headlight shadows (#283): their own atlas of kHeadlightShadows tiles (3 x 2),
  // one view per tile from kHeadlightShadowView, drawn before the world. Tile
  // size is half the local shadow resolution. Matrices and tile rects live in
  // row 2 of the light data texture (96+4t, 120+t); a lamp entry names its
  // tile + 1 in its colour's w.
  static constexpr size_t kHeadlightShadows=6;
  static constexpr uint16_t kHeadlightShadowView=145;
  static constexpr float kHeadlightShadowNear=.15f, kHeadlightShadowMargin=2.f;  // degrees
  void update_headlight_shadows(const VideoOptions& video);
  void set_headlight_focus(const void* car) { headlight_focus_car_=car; }
  const void* headlight_focus_car_=nullptr;
  bgfx::FrameBufferHandle headlight_shadow_fb_=BGFX_INVALID_HANDLE;
  bgfx::TextureHandle headlight_shadow_tex_=BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_headlight_shadow_=BGFX_INVALID_HANDLE,u_headlight_shadow_=BGFX_INVALID_HANDLE;
  uint16_t headlight_shadow_tile_=0;
  size_t headlight_shadow_count_=0;
  std::array<std::array<float,16>,kHeadlightShadows> headlight_shadow_matrices_{};
  std::array<std::array<float,4>,kHeadlightShadows> headlight_shadow_rects_{};
  uint64_t headlight_shadow_redraws_=0;  // tiles drawn, for tests
  struct CarShadowTile {
    const void* source=nullptr;
    std::array<float,16> view{}, proj{}, sample{};
    float strength=0.f;
    LampLight lamp{};
  };
  std::vector<CarShadowTile> car_shadow_tiles_;
  std::vector<ShadowCasterStamp> car_shadow_stamps_;
  bool car_shadow_valid_=false;
  uint64_t car_shadow_redraws_=0;
  std::array<std::array<float,16>,kLampSpecial> lamp_car_matrices_{};
  std::array<std::array<float,4>,kLampSpecial> lamp_car_tiles_{};
  size_t lamp_shadow_count_=0;  // shadowed lamps at the front of scenery_lamps_
  bool lamp_shadow_atlas_bound_=false;
  uint64_t lamp_shadow_redraws_=0;  // tiles drawn, for tests
  uint64_t lamp_shadow_assignments_=0;  // lamps given a slot, for tests
  // Script-space point the shadowed lamps are ranked around: the followed
  // car, else the camera. Set each frame before update_lamp_shadows.
  bool lamp_focus_valid_=false;
  std::array<float,3> lamp_focus_{};
  void set_lamp_shadow_focus(const float* script) {
    lamp_focus_valid_=script!=nullptr;
    if(script) lamp_focus_={script[0],script[1],script[2]};
  }
  float lamp_shadow_dt_override_=-1.f;  // tests: fixed fade step in seconds
  double lamp_shadow_clock_=-1.0;
  std::array<std::array<float,16>,kLampSpecial> lamp_shadow_matrices_{};
  std::array<std::array<float,4>,kLampSpecial> lamp_shadow_tiles_{};
  bgfx::UniformHandle u_lamp_shadow_params_=BGFX_INVALID_HANDLE;
  std::vector<ShadowCasterStamp> shadow_cached_casters_;
  std::array<float,3> shadow_cached_origin_{};
  bool shadow_cache_valid_ = false;
  uint64_t shadow_atlas_revision_ = 0;
  bool shadow_cascades_active_ = false;
  std::unordered_set<const ScxGeometry*> shadow_meshes_;
  bool static_shadow_local_ = false; // Positional-light field, rather than sun visibility.
  bool static_shadow_active_ = false;
  bgfx::FrameBufferHandle car_depth_fb_ = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle car_depth_tex_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_car_depth_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_car_depth_mtx_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_car_depth_params_ = BGFX_INVALID_HANDLE;
  std::array<float, 16> car_depth_matrix_{};
  bool car_depth_active_ = false;
  uint16_t car_depth_size_=2048;
  void update_car_depth(const float* position, const VideoOptions& video);
  uint16_t static_shadow_size_ = 0;
  float static_shadow_distance_ = 0.f;
  bgfx::UniformHandle u_static_shadow_eye_ = BGFX_INVALID_HANDLE;
  std::array<float, 3> static_shadow_eye_{};
  VideoMode applied_video_mode_{};
  uint32_t reset_flags_ = BGFX_RESET_VSYNC;
  float cull_eye_[3]{};
  float cull_view_[16]{};
  float cull_proj_[16]{};
  bool screen_size_visible(const float center[3], float radius) const;
  static bool screen_size_keep(const float center[3], float radius, void* user);

};

}  // namespace slrr
