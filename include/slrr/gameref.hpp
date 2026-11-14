#pragma once

#include "slrr/trigger_volume.hpp"
#include "slrr/scx_mesh.hpp"
#include "slrr/scene_sun.hpp"

#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
// GameRef_queueActiveCollision: consumed by the next physics preparation.
constexpr int32_t kCollisionQueued = 0x10000000;

enum class GameRefKind : uint8_t {
  Generic = 0,
  Dummy = 1,
  RenderRef = 2,
  GroundRef = 3,
  Camera = 4,
  Viewport = 5,
  GameType = 6,
  PhysicsRef = 7,
};

enum class PhysicsShape : uint8_t {
  None = 0,
  Resource = 1,
  Box = 2,
  Sphere = 3,
};

struct GameRefTimer {
  float deadline = 0.f;  // absolute sim seconds
  int32_t timer_id = 0;
};

struct RenderLinePoint {
  std::array<float, 3> pos, normal;
  float width;
};

struct GameRefState {
  std::shared_ptr<const ScxGeometry> deformed_geometry;
  std::shared_ptr<ScxGeometry> line_geometry;
  std::optional<RenderLinePoint> line_end;
  float line_length = 0.f;
  float detail = 0.f;
  int32_t deformed_type = 0;
  SceneSun directional_light;
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
  float angular_vel[3] = {0, 0, 0};
  // Script-created PhysicsRef primitive. Box uses half extents; sphere uses
  // physics_size[0] as its radius. Resource uses type_id's CFG collision bodies.
  PhysicsShape physics_shape = PhysicsShape::None;
  float physics_size[3] = {0, 0, 0};
  bool physics_static = true;
  // setStatic(0): the body falls and is pushed. Created bodies stay fixed;
  // the stock garage jack, lifts and walls never call setStatic.
  bool physics_simulated = false;
  bool dynamic_native = false;
  // RID_TRIGGER (system:0x34) volume from its creation params; see
  // trigger_volume.hpp.
  TriggerShape trigger_shape = TriggerShape::None;
  float trigger_size[3] = {0, 0, 0};
  bool collision_pending = false;
  bool collision_active = false;
  std::string alias;
  std::string sound_file;
  std::string class_name;
  std::vector<GameRefTimer> timers;
  // Children (intrusive list stand-in).
  std::vector<void*> children;
  // Camera / Viewport extras (kind-specific).
  void* viewport = nullptr;
  float half_aov = 45.f;
  float dmin = 0.1f;
  float dmax = 100.f;
  // Camera.setFog. The driving camera fades into the sky from 1.5 km to 3 km.
  // GroundRef weather fog stays on the separate globals.
  bool fog_set = false;
  int32_t fog_color = 0x0091badb;
  float fog_near = 1500.f;
  float fog_far = 3000.f;
  int32_t cam_pri = 0;
  int32_t cam_seq = 0;
  bool cam_active = false;
  bool cam_collision = false;
  int32_t follow_vehicle = 0;
  int cockpit_index = -1;
  int follow_variant = 0;
  bool camera_free = false, camera_autozoom = false, camera_attached = false;
  float camera_roll = 0.f;
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
  // Parent-local eye and look point. The preview rebuilds its view from
  // these: the Z reflection that matches the mesh also reverses a look-at basis.
  float aim_eye[3] = {};
  float aim_at[3] = {};
  // Optional bound mesh (RenderRef / GroundRef).
  bool has_mesh = false;
  bool sky_render = false;
  uint32_t color = 0xffffff;
  uint32_t paint_source = 0, paint_texture = 0;
  std::vector<std::pair<uint32_t, uint32_t>> texture_replacements;
  bool vehicle_material = false;
  // Cfg light bits (1 brake, 2 reverse, 4 city, 16 indicator). Drawn only
  // while that light is on.
  uint32_t light_flags = 0;
  bool matrix_override = false;
  float local_matrix[16]{};
  // Drawn pose of a simulated body between two fixed physics ticks. It holds
  // only while local_matrix still equals render_basis, so a script move wins.
  bool render_pose = false;
  float render_matrix[16]{};
  float render_basis[16]{};
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

// The interpolated parent-local matrix to draw, or null to draw local_matrix.
const float* gameref_render_matrix(const GameRefState& node);

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
inline void gameref_queue_collision_state(GameRefState& state) {
  if (state.collision_pending || state.collision_active) return;
  state.collision_pending = true;
  state.flags |= kCollisionQueued;
}
inline void gameref_activate_collision_state(GameRefState& state) {
  if (!state.collision_pending) return;
  state.collision_pending = false;
  state.collision_active = true;
  state.flags &= ~kCollisionQueued;
}
void gameref_request_active_collision(void* key);
// Promote queued collision activation at the physics boundary, never midway
// through a scene-graph mutation or script callback.
void gameref_process_collision_requests();

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
// system:0x34 (GameRef.RID_TRIGGER). Parses the trigger volume from the
// constructor params, poses the node and gives it a live id; other types are
// ignored. Trigger keys are listed for the per-tick trigger events.
void gameref_bind_trigger(void* key, const std::string& params);
std::vector<void*> gameref_trigger_keys();
// PhysicsRef.setStatic: a simulated body belongs to the host free-body
// solver, which visits gameref_simulated_bodies() every physics tick.
void gameref_set_physics_simulated(void* key, bool simulated);
std::vector<GameRefState*> gameref_simulated_bodies();
// render/look/move/zoom/hide on the script camera. A move with no look
// target slides the pose with Pori; a move plus look orbits.
bool gameref_scene_command(void* key, const std::string& line);
// Fullscreen script camera (dealer, car lot). Eye and look are script
// metres; the renderer still flips Z to match the uploaded meshes.
bool gameref_world_camera(float eye[3], float at[3], float* fov, float* znear,
                          float* zfar, float* up = nullptr);
void gameref_activate_camera(GameRefState& camera, GameRefState& viewport, int priority);
void gameref_advance_world_camera(float dt);
bool gameref_world_camera_can_drag();
void gameref_orbit_world_camera(float yaw_delta, float pitch_delta,
                                float zoom_axis, float dt);
std::vector<GameRefState*> gameref_nodes_with_mesh();
std::vector<GameRefState*> gameref_render_nodes();
void gameref_register_light(void* key);
std::vector<GameRefState*> gameref_directional_lights();
std::vector<void*> gameref_world_roots();  // flags & WORLDTREEROOT

void gameref_set_fog(int32_t color, float near_z, float far_z);
bool gameref_get_fog(int32_t* color, float* near_z, float* far_z);
void gameref_camera_set_fog(GameRefState& camera, int32_t color, float near_z,
                            float far_z);

}  // namespace slrr
