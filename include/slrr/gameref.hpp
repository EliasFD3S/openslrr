#pragma once

#include "slrr/scx_mesh.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

// Soft PE Native.ptr stand-in for the Java hierarchy (system Scripts):
//   java.lang.Native
//     └── java.util.resource.ResourceRef   ← newNative/set/load/id/parent TREE
//           ├── GameRef                   ← create / flags / pose / script
//           │     └── java.lang.GameType  ← timers / eventMask / createNative…
//           │           └── Scene → Garage / …
//           ├── RenderRef                 ← create / setMatrix / setType
//           │     └── Camera
//           ├── Dummy / GroundRef / …
// Java bodies (ctors, wrappers) stay TUFA. Host natives only on declaring class.

constexpr int32_t kWorldTreeRoot = 0x00000010;
constexpr int32_t kWorldTreeLeaf = 0x00000040;

constexpr int32_t kEventTime = 0x00000080;
constexpr int32_t kEventAny = 0x0FFFFFFF;

enum class GameRefKind : uint8_t {
  Generic = 0,
  Dummy = 1,
  RenderRef = 2,
  GroundRef = 3,
  Camera = 4,
  Viewport = 5,
  GameType = 6,
};

struct GameRefTimer {
  float deadline = 0.f;  // absolute sim seconds
  int32_t timer_id = 0;
};

struct GameRefState {
  void* key = nullptr;  // java.util.resource.GameRef* / GameType*
  void* binding = nullptr; // reference handle alias; never another scene node
  int32_t live_id = 0; // independent from the RPK type ID
  void* parent = nullptr;
  void* script = nullptr;  // usually same as key for GameType
  GameRefKind kind = GameRefKind::Generic;
  int32_t flags = 0;
  int32_t type_id = 0;
  int32_t event_mask = 0;
  bool empty = true;
  float pos[3] = {0, 0, 0};
  float ypr[3] = {0, 0, 0};
  float vel[3] = {0, 0, 0};
  std::string alias;
  std::string class_name;
  std::vector<GameRefTimer> timers;
  // Children (intrusive list stand-in).
  std::vector<void*> children;
  // Camera / Viewport extras (kind-specific).
  void* viewport = nullptr;
  float half_aov = 45.f;
  float dmin = 0.1f;
  float dmax = 100.f;
  int32_t cam_pri = 0;
  int32_t cam_seq = 0;
  bool cam_active = false;
  float vp_x = 0.f, vp_y = 0.f, vp_w = 1.f, vp_h = 1.f;
  int32_t vp_pri = 0;
  int32_t vp_flags = 0;
  bool vp_active = false;
  // RID_CAMERA preview (catalog, dealers). look/move offsets are script metres.
  int32_t look_id = 0;
  int32_t move_id = 0;
  float look_off[3] = {};
  float move_off[3] = {};
  bool aim_look = false;
  bool aim_move = false;
  float move_dist = 0.f;
  // `dist a b` and `height` from the script camera (javaSceneCamera).
  float script_dist_min = 1.f;
  float script_dist_max = 10.f;
  bool script_dist_set = false;
  float cam_height = 0.f;
  bool cam_height_set = false;
  // Mouse orbit accumulated until the view is built.
  float orbit_yaw = 0.f;
  float orbit_pitch = 0.f;
  float orbit_zoom = 0.f;
  // Spring length of the native camera (`move` distance). The wheel scales it
  // inside the `dist` range: garage 1–10, dealer 2.5–4.6.
  float cam_distance = 0.f;
  // `smooth` lerps the look point and the move point the way Pori does
  // (current += (dest - current) * rate). `force` is the drag spring.
  float look_smooth = 0.5f;
  float move_smooth = 0.5f;
  float cam_spring = 0.3f;
  float cam_damping = 0.5f;
  float cam_perpdamp = -0.7f;
  bool cam_pose_init = false;
  bool cam_az_set = false;
  float cam_base_az = 0.f;
  float cam_base_elev = 0.2f;
  float look_s[3] = {};
  float move_s[3] = {};
  float eye_s[3] = {};
  float eye_vel[3] = {};
  // This frame's look axes (PE mouse rate). The orbit body consumes them.
  float ctl_x = 0.f;
  float ctl_y = 0.f;
  float cam_clock = -1.f;
  // Parent-local eye and look point. The preview rebuilds its view from
  // these: the Z reflection that matches the mesh also reverses a look-at basis.
  float aim_eye[3] = {};
  float aim_at[3] = {};
  // Optional bound mesh (RenderRef / GroundRef).
  bool has_mesh = false;
  bool sky_render = false;
  uint32_t color = 0xffffff;
  uint32_t paint_source = 0, paint_texture = 0;
  bool vehicle_material = false;
  // Cfg light bits (1 brake, 2 reverse, 4 city, 16 indicator). Drawn only
  // while that light is on.
  uint32_t light_flags = 0;
  bool matrix_override = false;
  float local_matrix[16]{};
  ScxGeometry mesh;
  // Native object script (.cfg): render mesh, collision sphere, click volume.
  bool script_parsed = false;
  int32_t script_render = 0;
  int32_t script_body_res = 0;
  float script_body_r = 0.f;
  float script_trigger_r = 0.f;
  bool script_noclick = false;
  std::string script_name;
};

float gameref_now();  // steady seconds since first call (timers / animate)

void gameref_clear();
GameRefState* gameref_upsert(void* key, GameRefKind kind = GameRefKind::Generic);
GameRefState* gameref_get(void* key);
GameRefState* gameref_find_live(int32_t id);
int32_t gameref_make_live(void* key);
bool gameref_bind_alias(void* reference, void* target);
void gameref_remove(void* key);

void gameref_set_parent(void* key, void* parent);
void gameref_set_flags(void* key, int32_t flags);
void gameref_clear_flags(void* key, int32_t flags);
int32_t gameref_get_flags(void* key);

void gameref_set_matrix(void* key, float px, float py, float pz, float yaw,
                        float pitch, float roll);
void gameref_set_event_mask(void* key, int32_t mask);
void gameref_clear_event_mask(void* key, int32_t mask);
void gameref_add_timer(void* key, float deadline_abs, int32_t timer_id);
void gameref_remove_all_timers(void* key);

// Due timers (deadline <= now). Caller dispatches handleEvent(EVENT_TIME, id).
struct DueTimer {
  void* key = nullptr;
  int32_t timer_id = 0;
};
std::vector<DueTimer> gameref_poll_timers(float now_sec);

std::vector<GameRefState*> gameref_active_cameras();
// Newest fullscreen camera. Catalog slots (viewport under 1×1) are skipped.
GameRefState* fullscreen_script_camera();
// system:0x33. Constructor params are "x,y,z,yaw,pitch,roll,...".
void gameref_bind_scene_camera(void* key, const std::string& params);
// render/look/move/zoom/hide on the script camera. A move with no look
// target slides the pose with Pori; a move plus look orbits.
bool gameref_scene_command(void* key, const std::string& line);
// Fullscreen script camera (dealer, car lot). Eye and look are script
// metres; the renderer still flips Z to match the uploaded meshes.
bool gameref_world_camera(float eye[3], float at[3], float* fov, float* znear,
                          float* zfar);
void gameref_orbit_world_camera(float yaw_delta, float pitch_delta,
                                float zoom_axis, float dt);
std::vector<GameRefState*> gameref_nodes_with_mesh();
std::vector<GameRefState*> gameref_render_nodes();
std::vector<void*> gameref_world_roots();  // flags & WORLDTREEROOT

void gameref_set_fog(int32_t color, float near_z, float far_z);
bool gameref_get_fog(int32_t* color, float* near_z, float* far_z);

}  // namespace slrr
