#include "slrr/race_runtime.hpp"
#include "slrr/gc_roots.hpp"
#include "slrr/gameref.hpp"
#include "slrr/script_events.hpp"
#include "slrr/host_input.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/script_lights.hpp"
#include "slrr/skeleton.hpp"
#include "slrr/vehicle_dynamics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace slrr {
namespace {

std::unordered_map<void*, GameRefState> g_refs;
// unordered_map rehash preserves element addresses. Erase removes the index
// before freeing its state; aliases retain their own dormant ID if unbound.
std::unordered_map<int32_t, GameRefState*> g_live_refs;
uint64_t g_scene_revision = 1, g_next_incarnation = 1;
// Canonical keys of trigger volumes and of free PhysicsRef bodies, so their
// per-frame and per-tick passes do not scan every GameRef.
std::unordered_set<void*> g_trigger_refs, g_simulated_refs;
std::unordered_set<void*> g_lights;
// Keys that were given kind Camera (#268). gameref_active_cameras ran over
// every node several times per frame; it now reads these and drops the
// ones that are gone or no longer cameras.
std::unordered_set<void*> g_camera_refs;
uint32_t g_next_live = 0x70000000;
int32_t g_fog_color = 0;
float g_fog_near = 0.f;
float g_fog_far = 0.f;
bool g_fog = false;
int32_t g_camera_sequence = 0;

void unlink_from_parent(GameRefState* n) {
  if (!n || !n->parent) return;
  GameRefState* p = gameref_get(n->parent);
  if (!p) return;
  auto& ch = p->children;
  ch.erase(std::remove(ch.begin(), ch.end(), n->key), ch.end());
}

}  // namespace

float gameref_now() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration<float>(clock::now() - t0).count();
}

const float* gameref_render_matrix(const GameRefState& node) {
  return node.render_pose && node.matrix_override &&
                 std::equal(node.local_matrix, node.local_matrix + 16, node.render_basis)
             ? node.render_matrix
             : nullptr;
}

void gameref_clear() {
  race_reset();
  ++g_scene_revision;
  part_drop_actors();
  character_forget_all();
  g_live_refs.clear();
  g_refs.clear();
  g_trigger_refs.clear();
  g_simulated_refs.clear();
  g_lights.clear();
  g_camera_refs.clear();
  script_lights_clear();
  g_next_live = 0x70000000;
  g_fog = false;
}

GameRefState* gameref_upsert(void* key, GameRefKind kind) {
  if (!key) return nullptr;
  ++g_scene_revision;
  auto it = g_refs.find(key);
  if (kind == GameRefKind::Camera) g_camera_refs.insert(key);
  if (it != g_refs.end()) {
    if (kind != GameRefKind::Generic) it->second.kind = kind;
    it->second.empty = false;
    return &it->second;
  }
  GameRefState n;
  n.incarnation = g_next_incarnation++;
  n.key = key;
  n.kind = kind;
  n.empty = false;
  auto [ins, _] = g_refs.emplace(key, std::move(n));
  return &ins->second;
}

GameRefState* gameref_get(void* key) {
  if (!key) return nullptr;
  for(int depth=0;depth<64;depth++) {
    auto it=g_refs.find(key);if(it==g_refs.end())return nullptr;
    if(!it->second.binding)return &it->second;
    key=it->second.binding;
  }
  return nullptr;
}
GameRefState* gameref_find_live(int32_t id) {
  if(id<0x70000000)return nullptr;
  const auto it = g_live_refs.find(id);
  return it != g_live_refs.end() && !it->second->binding ? it->second : nullptr;
}
bool gameref_uses_resource(uint32_t id) {
  return std::any_of(g_refs.begin(), g_refs.end(), [id](const auto& entry) {
    return !entry.second.binding && static_cast<uint32_t>(entry.second.type_id) == id;
  });
}
int32_t gameref_make_live(void* key) {
  auto* state=gameref_get(key);if(!state)state=gameref_upsert(key);if(!state)return 0;
  if (!state->live_id) {
    // IDs never recycle before clear. Check exhaustion without signed overflow.
    if (g_next_live > static_cast<uint32_t>(INT32_MAX)) return 0;
    const auto id = static_cast<int32_t>(g_next_live);
    g_live_refs.emplace(id, state);
    state->live_id = id;
    ++g_next_live;
  }
  return state->live_id;
}
bool gameref_bind_alias(void* reference,void* target) {
  auto* canonical=gameref_get(target);if(!reference||!canonical)return false;
  if(reference==canonical->key)return true;
  auto* ref=gameref_upsert(reference);ref->binding=canonical->key;return true;
}

void gameref_remove(void* key) {
  if (!key) return;
  ++g_scene_revision;
  g_trigger_refs.erase(key);
  g_simulated_refs.erase(key);
  character_forget(key);
  race_forget(key);
  script_events_destroy(static_cast<Object*>(key));
  auto own=g_refs.find(key);
  if(own!=g_refs.end()&&own->second.binding){g_live_refs.erase(own->second.live_id);g_refs.erase(own);return;}
  GameRefState* n = gameref_get(key);
  if (n) unlink_from_parent(n);
  // Orphan children stay keyed but parent cleared.
  for (auto it = g_refs.begin(); it != g_refs.end();) {
    if (it->second.binding == key) {
      g_live_refs.erase(it->second.live_id);
      it = g_refs.erase(it);
    } else {
      if (it->second.parent == key) it->second.parent = nullptr;
      ++it;
    }
  }
  g_lights.erase(key);
  g_camera_refs.erase(key);
  // Script lights die with the GameRef they are attached to (#235).
  script_lights_forget_parent(key);
  if (n) g_live_refs.erase(n->live_id);
  g_refs.erase(key);
}

void gameref_set_type(GameRefState& node, int32_t type) {
  if (node.type_id == type) return;
  node.type_id = type;
  ++g_scene_revision;
}
void gameref_release_render_mask(GameRefRenderMask& mask) {
  for (const auto& [key, incarnation] : mask.hidden) {
    auto* node = gameref_get(key);
    if (node && node->key == key && node->incarnation == incarnation) node->empty = false;
  }
  mask = {};
}
bool gameref_mask_duplicate_renders(GameRefRenderMask& mask, void* root, void* keep, int32_t type) {
  if (mask.revision == g_scene_revision && mask.root == root && mask.keep == keep && mask.type == type)
    return false;
  gameref_release_render_mask(mask);
  std::vector<void*> pending{root};
  std::unordered_set<void*> visited;
  for (size_t i = 0; i < pending.size(); ++i) {
    auto* node = gameref_get(pending[i]);
    if (!node || !visited.insert(node->key).second) continue;
    const bool kept = node->key == keep || node->script == keep;
    if (!kept && node->kind == GameRefKind::RenderRef && node->type_id == type && !node->empty) {
      mask.hidden.emplace_back(node->key, node->incarnation);
      node->empty = true;
    }
    for (void* child : node->children) pending.push_back(child);
  }
  mask.revision = g_scene_revision;
  mask.root = root; mask.keep = keep; mask.type = type;
  return true;
}

void gameref_set_parent(void* key, void* parent) {
  GameRefState* n = gameref_get(key);
  if (!n) n = gameref_upsert(key);
  if (!n) return;
  key=n->key;
  if(auto* canonical_parent=gameref_get(parent))parent=canonical_parent->key;
  if (n->parent == parent) return;
  ++g_scene_revision;
  unlink_from_parent(n);
  n->parent = parent;
  if (parent) {
    GameRefState* p = gameref_upsert(parent);
    if (p) p->children.push_back(key);
  }
}

void gameref_set_flags(void* key, int32_t flags) {
  GameRefState* n = gameref_get(key);
  if(!n)n=gameref_upsert(key);
  if (n) n->flags |= flags;
}

void gameref_clear_flags(void* key, int32_t flags) {
  GameRefState* n = gameref_get(key);
  if (n) n->flags &= ~flags;
}

int32_t gameref_get_flags(void* key) {
  GameRefState* n = gameref_get(key);
  return n ? n->flags : 0;
}

void gameref_request_active_collision(void* key) {
  GameRefState* n = gameref_get(key);
  if (!n) n = gameref_upsert(key);
  if (n) gameref_queue_collision_state(*n);
}

void gameref_process_collision_requests() {
  for (auto& [key, n] : g_refs) gameref_activate_collision_state(n);
}

void gameref_set_matrix(void* key, float px, float py, float pz, float yaw,
                        float pitch, float roll) {
  if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz) ||
      !std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(roll)) return;
  GameRefState* n = gameref_get(key);
  if(!n)n=gameref_upsert(key);
  if (!n) return;
  n->matrix_override=false;
  n->pos[0] = px;
  n->pos[1] = py;
  n->pos[2] = pz;
  n->ypr[0] = yaw;
  n->ypr[1] = pitch;
  n->ypr[2] = roll;
  std::fill(std::begin(n->vel), std::end(n->vel), 0.f);
  std::fill(std::begin(n->angular_vel), std::end(n->angular_vel), 0.f);
  // ROCTrack.moveCamera and City.startRace stage the bound follow camera and
  // keep the binding until the race starts. Put the body there and hold it.
  if (n->kind == GameRefKind::Camera && n->follow_vehicle && n->cockpit_index < 0 &&
      !n->camera_free) {
    std::copy_n(n->pos, 3, n->eye_s);
    std::fill(std::begin(n->eye_vel), std::end(n->eye_vel), 0.f);
    n->cam_pose_init = n->follow_hold = true;
  }
}

void gameref_set_camera_roll_from_matrix(void* key, float roll) {
  GameRefState* n = gameref_get(key);
  if (!n || n->kind != GameRefKind::Camera || !std::isfinite(roll)) return;
  if (n->follow_vehicle && n->cockpit_index < 0 && !n->camera_free) return;
  n->camera_roll = roll;
}

bool gameref_mod_camera_set(void* key, ModCameraMode mode, const float eye[3],
                            const float at[3], const float up[3], float fov, void* car) {
  GameRefState* n = gameref_get(key);
  if (!n || n->kind != GameRefKind::Camera) return false;
  int32_t car_id = 0;
  if (mode == ModCameraMode::Relative || mode == ModCameraMode::Local) {
    float frame[16];
    if (!car || !part_is_car(car) || !part_camera_pose(car, -1, frame)) return false;
    car_id = gameref_make_live(car);
    if (!car_id) return false;
  }
  if (!mod_camera_set(n->mod_pose, mode, eye, at, up, fov)) return false;
  n->mod_pose_car = car_id;
  return true;
}

void gameref_mod_camera_clear(void* key) {
  if (GameRefState* n = gameref_get(key)) {
    n->mod_pose = {};
    n->mod_pose_car = 0;
  }
}

void gameref_set_event_mask(void* key, int32_t mask) {
  GameRefState* n = gameref_get(key);
  if (!n) n = gameref_upsert(key, GameRefKind::GameType);
  if (n) n->event_mask |= mask;
}

void gameref_clear_event_mask(void* key, int32_t mask) {
  GameRefState* n = gameref_get(key);
  if (n) n->event_mask &= ~mask;
}

void gameref_add_timer(void* key, float deadline_abs, int32_t timer_id) {
  GameRefState* n = gameref_get(key);
  if (!n) n = gameref_upsert(key, GameRefKind::GameType);
  if (!n) return;
  GameRefTimer t;
  t.deadline = deadline_abs;
  t.timer_id = timer_id;
  n->timers.push_back(t);
}

void gameref_remove_all_timers(void* key) {
  GameRefState* n = gameref_get(key);
  if (n) n->timers.clear();
}

std::vector<DueTimer> gameref_poll_timers(float now_sec) {
  // Soft PE Engine_TickTimers: due when fire_at <= clock; oneshot erase.
  // Caller still mask-gates EVENT_TIME before handleEvent.
  std::vector<DueTimer> due;
  for (auto& kv : g_refs) {
    GameRefState& n = kv.second;
    for (size_t i = 0; i < n.timers.size();) {
      if (n.timers[i].deadline <= now_sec) {
        DueTimer d;
        d.key = n.key;
        d.timer_id = n.timers[i].timer_id;
        due.push_back(d);
        n.timers.erase(n.timers.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
    }
  }
  return due;
}

void gameref_bind_scene_camera(void* key, const std::string& params) {
  GameRefState* st = gameref_get(key);
  if (!st || (st->type_id & 0xFFFF) != 0x33) return;
  if (st->kind != GameRefKind::Camera) ++g_scene_revision;
  st->kind = GameRefKind::Camera;
  g_camera_refs.insert(st->key);
  st->cam_active = false;
  st->half_aov = 60.f;
  st->dmin = 0.05f;
  st->dmax = 500.f;
  gameref_make_live(key);
  float p[6] = {};
  int flags = 0;
  const int fields = std::sscanf(params.c_str(), " %f , %f , %f , %f , %f , %f , %i",
                                 &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &flags);
  st->cam_collision = fields == 7 && (flags & 0x10) != 0;
  st->cam_pose_init = false;
  st->cam_az_set = false;
  if (fields >= 3)
    gameref_set_matrix(key, p[0], p[1], p[2], p[3], p[4], p[5]);
}

void gameref_bind_trigger(void* key, const std::string& params) {
  GameRefState* st = gameref_get(key);
  if (!st || (st->type_id & 0xFFFF) != 0x34) return;
  TriggerVolume volume;
  if (!trigger_volume_parse(params, volume)) return;
  st->trigger_shape = volume.shape;
  std::copy(volume.size, volume.size + 3, st->trigger_size);
  g_trigger_refs.insert(st->key);
  // Scripts compare trigger handles by id() (ROCTrack, Nevada_Track).
  gameref_make_live(key);
  const float* p = volume.pose;
  gameref_set_matrix(key, p[0], p[1], p[2], p[3], p[4], p[5]);
}

void gameref_set_physics_simulated(void* key, bool simulated) {
  GameRefState* st = gameref_get(key);
  if (!st) return;
  st->physics_simulated = simulated;
  if (simulated)
    g_simulated_refs.insert(st->key);
  else
    g_simulated_refs.erase(st->key);
}

std::vector<GameRefState*> gameref_simulated_bodies() {
  std::vector<GameRefState*> bodies;
  for (void* key : g_simulated_refs) {
    const auto found = g_refs.find(key);
    if (found != g_refs.end() && found->second.physics_simulated &&
        found->second.physics_shape != PhysicsShape::None)
      bodies.push_back(&found->second);
  }
  return bodies;
}

std::vector<void*> gameref_trigger_keys() {
  std::vector<void*> keys;
  for (void* key : g_trigger_refs) {
    const auto found = g_refs.find(key);
    if (found != g_refs.end() && found->second.trigger_shape != TriggerShape::None)
      keys.push_back(key);
  }
  return keys;
}

void aim_matrix(GameRefState* cam, const float eye[3], const float at[3]) {
  float fx = at[0] - eye[0], fy = at[1] - eye[1], fz = at[2] - eye[2];
  float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
  if (fl < 1e-6f) {
    fx = 0.f;
    fy = 0.f;
    fz = -1.f;
    fl = 1.f;
  }
  fx /= fl;
  fy /= fl;
  fz /= fl;
  float rx = fy * 0.f - fz * 1.f;
  float ry = fz * 0.f - fx * 0.f;
  float rz = fx * 1.f - fy * 0.f;
  float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
  if (rl < 1e-6f) {
    rx = 1.f;
    ry = 0.f;
    rz = 0.f;
    rl = 1.f;
  }
  rx /= rl;
  ry /= rl;
  rz /= rl;
  const float ux = ry * fz - rz * fy;
  const float uy = rz * fx - rx * fz;
  const float uz = rx * fy - ry * fx;
  float* m = cam->local_matrix;
  m[0] = rx;
  m[1] = ry;
  m[2] = rz;
  m[3] = 0.f;
  m[4] = ux;
  m[5] = uy;
  m[6] = uz;
  m[7] = 0.f;
  m[8] = -fx;
  m[9] = -fy;
  m[10] = -fz;
  m[11] = 0.f;
  m[12] = eye[0];
  m[13] = eye[1];
  m[14] = eye[2];
  m[15] = 1.f;
  cam->matrix_override = true;
}

void target_point(GameRefState* cam, int32_t id, const float off[3],
                  float out[3]) {
  GameRefState* n = gameref_find_live(id);
  if (!n) n = cam->parent ? gameref_get(cam->parent) : nullptr;
  float px = 0.f, py = 0.f, pz = 0.f, yaw = 0.f;
  if (const float* drawn = n ? gameref_render_matrix(*n) : nullptr) {
    // Aim at the drawn car, which is interpolated between physics ticks.
    px = drawn[12];
    py = drawn[13];
    pz = drawn[14];
    yaw = std::atan2(drawn[8], drawn[10]);
  } else if (n) {
    px = n->pos[0];
    py = n->pos[1];
    pz = n->pos[2];
    yaw = n->ypr[0];
  }
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  out[0] = px + cy * off[0] + sy * off[2];
  out[1] = py + off[1];
  out[2] = pz - sy * off[0] + cy * off[2];
}

// Heading of a camera target, as target_point turns its offsets.
static float target_heading(GameRefState* cam, int32_t id) {
  GameRefState* n = gameref_find_live(id);
  if (!n) n = cam->parent ? gameref_get(cam->parent) : nullptr;
  if (const float* m = n ? gameref_render_matrix(*n) : nullptr)
    return std::atan2(m[8], m[10]);
  return n ? n->ypr[0] : 0.f;
}

void refresh_aim(GameRefState* cam) {
  if (!cam->aim_look && !cam->aim_move) return;
  float eye[3] = {cam->pos[0], cam->pos[1], cam->pos[2]};
  float at[3] = {eye[0], eye[1], eye[2] - 1.f};
  if (cam->aim_look) target_point(cam, cam->look_id, cam->look_off, at);
  if (cam->aim_move) {
    float follow[3];
    target_point(cam, cam->move_id, cam->move_off, follow);
    // SLRR_rev javaSceneCamera (codex/physics-reverse-port @ c65675f): without
    // a dist range the eye keeps its creation offset from the move point, and
    // its height above that point. Catalog creation pose is (-2, 1, -4).
    float ox = cam->pos[0] - follow[0];
    float oz = cam->pos[2] - follow[2];
    float len = std::sqrt(ox * ox + oz * oz);
    if (len < 1e-4f) {
      ox = std::sin(cam->ypr[0]);
      oz = std::cos(cam->ypr[0]);
      len = 1.f;
    }
    eye[0] = follow[0] + ox / len * len;
    eye[2] = follow[2] + oz / len * len;
    const float lift = cam->pos[1] - follow[1];
    eye[1] = follow[1] + (lift > 0.1f ? lift : 0.1f);
  }
  std::copy(eye, eye + 3, cam->aim_eye);
  std::copy(at, at + 3, cam->aim_at);
  aim_matrix(cam, eye, at);
}

void gameref_activate_camera(GameRefState& camera, GameRefState& viewport, int priority) {
  camera.viewport = viewport.key;
  camera.cam_pri = priority;
  camera.cam_active = true;
  camera.cam_seq = ++g_camera_sequence;
  camera.chase_fov = 0.f;  // re-entered views restart from the current speed
}

bool gameref_scene_command(void* key, const std::string& line) {
  GameRefState* st = gameref_get(key);
  if (!st || (st->type_id & 0xFFFF) != 0x33) return false;
  const char* text = line.c_str();
  while (*text == ' ' || *text == '\t') ++text;
  char word[32] = {};
  std::sscanf(text, "%31s", word);
  if (std::strcmp(word, "render") == 0) {
    int vp_id = 0, b = 0, c = 0, pri = 0;
    std::sscanf(text, "%*s %d %d %d %d", &vp_id, &b, &c, &pri);
    if (GameRefState* vp = gameref_find_live(vp_id)) {
      gameref_activate_camera(*st, *vp, pri);
    }
    return false;
  }
  if (std::strcmp(word, "hide") == 0) {
    st->cam_active = false;
    return false;
  }
  if (std::strcmp(word, "look") == 0) {
    int id = 0;
    float off[3] = {};
    std::sscanf(text, "%*s %d %f , %f , %f", &id, &off[0], &off[1], &off[2]);
    st->look_id = id;
    std::copy(off, off + 3, st->look_off);
    st->aim_look = true;
    st->angle_gain = st->angle_rate = 0.f;  // native look clears angle
    st->follow_hold = false;
    refresh_aim(st);
    return false;
  }
  if (std::strcmp(word, "move") == 0) {
    int id = 0;
    float off[3] = {};
    float dist = 0.f;
    float attach[3]{};
    const int count = std::sscanf(text, "%*s %d %f , %f , %f %f %f , %f , %f",
        &id, &off[0], &off[1], &off[2], &dist, &attach[0], &attach[1], &attach[2]);
    st->camera_attached = count == 8 && dist <= .25f;
    if (st->camera_attached)
      for (int i = 0; i < 3; ++i) off[i] += attach[i];
    st->move_id = id;
    std::copy(off, off + 3, st->move_off);
    st->move_dist = dist;
    if (!st->camera_attached) st->cam_distance = dist;
    if (st->script_dist_set) {
      if (st->cam_distance < st->script_dist_min)
        st->cam_distance = st->script_dist_min;
      if (st->cam_distance > st->script_dist_max)
        st->cam_distance = st->script_dist_max;
    }
    st->aim_move = true;
    st->angle_gain = st->angle_rate = 0.f;  // native move clears angle
    st->follow_hold = false;
    refresh_aim(st);
    return false;
  }
  if (std::strcmp(word, "height") == 0) {
    if (std::sscanf(text, "%*s %f", &st->cam_height) == 1)
      st->cam_height_set = true;
    return false;
  }
  if (std::strcmp(word, "roll") == 0) {
    std::sscanf(text, "%*s %f", &st->camera_roll);
    return true;
  }
  if (std::strcmp(word, "autozoom") == 0) {
    st->camera_autozoom = true;
    return true;
  }
  if (std::strcmp(word, "dist") == 0) {
    float a = 0.f, b = 0.f;
    if (std::sscanf(text, "%*s %f %f", &a, &b) >= 2 && b >= a) {
      st->script_dist_min = a;
      st->script_dist_max = b;
      st->script_dist_set = true;
    }
    return true;
  }
  if (std::strcmp(word, "smooth") == 0) {
    float a = st->look_smooth, b = st->move_smooth;
    const int n = std::sscanf(text, "%*s %f %f", &a, &b);
    if (n >= 1) st->look_smooth = std::max(0.f, std::min(1.f, a));
    if (n >= 2) st->move_smooth = std::max(0.f, std::min(1.f, b));
    return true;
  }
  if (std::strcmp(word, "force") == 0) {
    std::sscanf(text, "%*s %f %f %f", &st->cam_spring, &st->cam_damping,
                &st->cam_perpdamp);
    return true;
  }
  if (std::strcmp(word, "zoom") == 0) {
    float aov = st->aov_goal > 0.f ? st->aov_goal : st->half_aov;
    std::sscanf(text, "%*s %f %f", &aov, &st->aov_rate);
    st->aov_goal = std::clamp(aov, 5.f, 120.f);
    GameRefState* vp = st->viewport ? gameref_get(st->viewport) : nullptr;
    const bool full = !vp || vp->vp_w >= 0.9f || vp->vp_h >= 0.9f;
    // Only the world camera is ticked; other views take the goal at once.
    if (st->aov_rate < 0.f || !full) st->half_aov = st->aov_goal;
    return full;
  }
  if (std::strcmp(word, "simulate") == 0) {
    int on = 0;
    std::sscanf(text, "%*s %d", &on);
    st->camera_simulate = on != 0;
    return true;
  }
  if (std::strcmp(word, "nearplane") == 0) {
    std::sscanf(text, "%*s %f", &st->dmin);
    return false;
  }
  if (std::strcmp(word, "farplane") == 0) {
    std::sscanf(text, "%*s %f", &st->dmax);
    return false;
  }
  if (std::strcmp(word, "angle") == 0) {
    float a[3] = {};
    std::sscanf(text, "%*s %f %f %f", &a[0], &a[1], &a[2]);
    st->angle_offset = a[0];
    st->angle_gain = a[1];
    st->angle_rate = a[2];
    // The optional phase time is unused by stock scripts; start from here.
    st->angle_seed = a[2] != 0.f;
    return true;
  }
  return std::strcmp(word, "torque") == 0;
}

GameRefState* fullscreen_script_camera() {
  GameRefState* found = nullptr;
  for (GameRefState* c : gameref_active_cameras()) {
    if (!c || !c->cam_active) continue;
    GameRefState* vp = c->viewport ? gameref_get(c->viewport) : nullptr;
    const GameRefState* car = c->cockpit_index >= 0 ? gameref_find_live(c->follow_vehicle)
                                                    : nullptr;
    if (c->cockpit_index >= 0 && (!vp || !vp->vp_active || !car)) continue;
    if (void* world = scene_instances_world()) {
      // A local Dummy world (GPS, inventory, selector) never supplies the
      // main scene camera, even if its viewport fills the screen. Conversely,
      // the scene camera remains the main camera when scripts resize its view.
      // The cockpit camera (car "render") has no parent of its own: it sits
      // in the world of the car it rides.
      float matrix[16];
      if (!scene_instance_transform(car ? car->key : c->key, world, matrix)) continue;
    } else if (vp && (vp->vp_w < 0.99f || vp->vp_h < 0.99f)) continue;
    // Osd.show() adds a second fullscreen camera (priority 2, parent = the
    // Osd) after the scene camera. It exists for 3D widgets; those are drawn
    // as 2D. Taking it as the world camera looks past the car.
    if (GameRefState* parent = c->parent ? gameref_get(c->parent) : nullptr)
      if (parent->class_name.find("Osd") != std::string::npos ||
          parent->class_name == "java.io.MouseCursor") continue;
    if (!found || c->cam_seq >= found->cam_seq) found = c;
  }
  return found;
}

// Pori step: current += (dest - current) * rate, once per tick at 60 Hz.
void pori_step(float* cur, const float* dest, float rate, float dt) {
  if (dt <= 0.f) return;
  float k = rate;
  if (dt > 0.f && rate > 0.f && rate < 1.f)
    k = 1.f - std::pow(1.f - rate, dt * 60.f);
  if (k < 0.f) k = 0.f;
  if (k > 1.f) k = 1.f;
  for (int i = 0; i < 3; ++i) cur[i] += (dest[i] - cur[i]) * k;
}

void move_orbit_body(GameRefState* cam, float delta[3], float dt) {
  float* p = cam->eye_s;
  float* v = cam->eye_vel;
  // The 606 camera uses a 0.15 m sphere. In the traced garage impact its
  // contact had no friction and a restitution of 0.20.
  for (int contact = 0; contact < 3; ++contact) {
    SceneSphereHit hit;
    if (!cam->cam_collision ||
        !scene_instances_sweep_sphere(p, delta, 0.15f, hit)) {
      for (int i = 0; i < 3; ++i) p[i] += delta[i];
      return;
    }
    float vn = 0.f;
    for (int i = 0; i < 3; ++i) {
      p[i] += delta[i] * hit.fraction + hit.normal[i] * 0.0001f;
      vn += v[i] * hit.normal[i];
    }
    const float bounce = 0.20f * std::max(0.f, -vn - 0.2f);
    dt *= 1.f - hit.fraction;
    for (int i = 0; i < 3; ++i) {
      v[i] += (bounce - std::min(vn, 0.f)) * hit.normal[i];
      delta[i] = v[i] * dt;
    }
  }
}

// PE camera body (FUN_004897b0 / FUN_0048a430). The mouse is a force on a
// 0.05 kg body. The spring only restores the distance to the hub, and the
// perpendicular term brakes sideways speed. Releasing the drag leaves the
// body where it is: nothing pulls the angle back, so there is no bounce.
// The two forces do not cancel cleanly, which is the tremble.
static float g_camera_warp = 1.f;

// Native angle goal (handler 0x43af6d, update 0x439d3c): a nonzero rate turns
// the goal at rate rad per second of game time from the azimuth the command
// found; `simulate` keeps it on real time unless the game is paused.
static float angle_goal(GameRefState& cam, float rel, float dt) {
  if (cam.angle_rate == 0.f) return cam.angle_offset;
  const float clock =
      cam.camera_simulate && g_camera_warp > 1e-5f ? 1.f : g_camera_warp;
  cam.angle_phase = cam.angle_seed ? rel
      : std::remainder(cam.angle_phase + cam.angle_rate * dt * clock, 6.2831853f);
  cam.angle_seed = false;
  return cam.angle_phase;
}

void drive_orbit(GameRefState* cam, const float* hub, float dist, float frame_dt,
                 float heading) {
  const float ax = cam->ctl_x;
  const float ay = cam->ctl_y;
  cam->ctl_x = 0.f;
  cam->ctl_y = 0.f;
  float rem = frame_dt;
  if (rem < 0.f) rem = 0.f;
  if (rem > 0.1f) rem = 0.1f;
  // `angle`: an uncapped tangential force err * gain (N) along Ypr(azimuth).x.
  float angle_to = 0.f;
  if (cam->angle_gain > 0.f && rem > 0.f)
    angle_to = heading + angle_goal(*cam, std::remainder(std::atan2(
        cam->eye_s[0] - hub[0], cam->eye_s[2] - hub[2]) - heading, 6.2831853f), rem);
  constexpr float kStep = 1.f / 120.f;
  while (rem > 1e-4f) {
    float dt = rem;
    if (rem >= kStep) {
      dt = kStep;
      if (rem - kStep <= kStep) dt = rem * 0.5f;
    }
    rem -= dt;
    float fx = ax * -400.f;
    float fy = ay * 400.f;
    const float fl2 = fx * fx + fy * fy;
    if (fl2 > 100.f) {
      const float s = 10.f / std::sqrt(fl2);
      fx *= s;
      fy *= s;
    }
    float* p = cam->eye_s;
    float* v = cam->eye_vel;
    float fwx = hub[0] - p[0];
    float fwy = hub[1] - p[1];
    float fwz = hub[2] - p[2];
    float fl = std::sqrt(fwx * fwx + fwy * fwy + fwz * fwz);
    if (fl < 1e-3f) {
      fwx = 0.f;
      fwy = 0.f;
      fwz = -1.f;
    } else {
      fwx /= fl;
      fwy /= fl;
      fwz /= fl;
    }
    float rx = -fwz;
    float rz = fwx;
    float rl = std::sqrt(rx * rx + rz * rz);
    if (rl < 1e-4f) {
      rx = 1.f;
      rz = 0.f;
      rl = 1.f;
    }
    rx /= rl;
    rz /= rl;
    const float ux = -rz * fwy;
    const float uy = rz * fwx - rx * fwz;
    const float uz = rx * fwy;
    float Fx = rx * fx + ux * fy - v[0] * 0.0025f;
    float Fy = uy * fy - v[1] * 0.0025f;
    float Fz = rz * fx + uz * fy - v[2] * 0.0025f;
    if (cam->angle_gain > 0.f) {
      const float az = std::atan2(p[0] - hub[0], p[2] - hub[2]);
      const float f = std::remainder(angle_to - az, 6.2831853f) * cam->angle_gain;
      Fx += std::cos(az) * f;
      Fz -= std::sin(az) * f;
    }
    if (cam->cam_height_set) {
      const float err = cam->cam_height + hub[1] - p[1];
      if (std::fabs(err) > 0.001f) Fy += err * 30.f - 2.f * v[1];
    }
    const float dx = hub[0] - p[0];
    const float dy = hub[1] - p[1];
    const float dz = hub[2] - p[2];
    const float len2 = dx * dx + dy * dy + dz * dz;
    if (dist > 0.f && len2 > 1e-4f) {
      const float len = std::sqrt(len2);
      const float nx = dx / len;
      const float ny = dy / len;
      const float nz = dz / len;
      const float along = v[0] * nx + v[1] * ny + v[2] * nz;
      const float spring = (len - dist) * cam->cam_spring;
      const float damp = -along * cam->cam_damping;
      float sfx = nx * (spring + damp);
      float sfy = ny * (spring + damp);
      float sfz = nz * (spring + damp);
      const float sf2 = sfx * sfx + sfy * sfy + sfz * sfz;
      if (sf2 > 400.f) {
        const float s = 20.f / std::sqrt(sf2);
        sfx *= s;
        sfy *= s;
        sfz *= s;
      }
      const float tx = v[0] - nx * along;
      const float ty = v[1] - ny * along;
      const float tz = v[2] - nz * along;
      Fx += sfx + tx * cam->cam_perpdamp;
      Fy += sfy + ty * cam->cam_perpdamp;
      Fz += sfz + tz * cam->cam_perpdamp;
    }
    constexpr float kInvMass = 20.f;
    const float axa = Fx * kInvMass;
    const float aya = Fy * kInvMass;
    const float aza = Fz * kInvMass;
    float delta[] = {v[0] * dt + axa * 0.5f * dt * dt,
                     v[1] * dt + aya * 0.5f * dt * dt,
                     v[2] * dt + aza * 0.5f * dt * dt};
    v[0] += axa * dt;
    v[1] += aya * dt;
    v[2] += aza * dt;
    move_orbit_body(cam, delta, dt);
  }
}

// Held follow body: it stays where the script put it, looks at the target,
// keeps inside the dist range and turns toward the `angle` goal. Relative
// azimuth 0 is behind the target. The native angle is a tangential force
// err * gain on the camera body, which perpendicular damping turns into an
// angular speed err * gain / (|perpdamp| * r); this steps at that rate.
static void held_follow_pose(GameRefState& cam, const float* center, float heading,
                             float dt) {
  float d[3] = {cam.eye_s[0] - center[0], cam.eye_s[1] - center[1],
                cam.eye_s[2] - center[2]};
  if (cam.angle_gain > 0.f && dt > 0.f) {
    const float rel = std::remainder(std::atan2(d[0], d[2]) - heading, 6.2831853f);
    const float goal = angle_goal(cam, rel, dt);
    const float r = std::max(std::hypot(d[0], d[2]), .5f);
    const float rate = cam.angle_gain / (std::max(std::abs(cam.cam_perpdamp), .05f) * r);
    const float step = std::remainder(goal - rel, 6.2831853f) * -std::expm1(-dt * rate);
    const float c = std::cos(step), s = std::sin(step), x = d[0], z = d[2];
    d[0] = x * c + z * s;
    d[2] = z * c - x * s;
  }
  const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  const float lo = cam.script_dist_min, hi = cam.script_dist_max;
  const float scale = len < 1e-4f ? 1.f : len > hi ? hi / len : len < lo ? lo / len : 1.f;
  // The target pulled away or the player took the view: chase again.
  if (dt > 0.f && cam.angle_gain <= 0.f && (len > hi + .5f || host_input().rmb_down))
    cam.follow_hold = false;
  for (int i = 0; i < 3; ++i) cam.eye_s[i] = center[i] + d[i] * scale;
}

// Chase camera speed feel. The native follow view keeps a fixed 42 degree
// angle, so 160 km/h in a bend looked like 60. The angle widens with speed
// (smoothstep from 30 to 250 km/h, up to +20 degrees) and the forward
// acceleration feeds the eye distance. Both are smoothed so physics ticks
// landing unevenly on render frames do not shake the view.
constexpr float kChaseFov = 42.f;
static void update_chase_feel(GameRefState& cam, void* car, const float* frame, float dt) {
  // Until the first timed step the pose reads kChaseFov.
  const bool fresh = cam.chase_fov <= 0.f;
  if (dt <= 0.f) return;
  float v[3];
  if (!part_car_velocity(car, v)) return;
  // A view switched to at speed starts from that speed's angle, not 42.
  if (fresh) cam.chase_speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  // Chassis -Z is forward; signed speed so reversing does not count as pulling.
  const float back = std::hypot(frame[8], frame[10]);
  const float forward = back > 1e-6f ? -(v[0] * frame[8] + v[2] * frame[10]) / back : 0.f;
  const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  const float prev = cam.chase_speed;
  cam.chase_speed += (speed - prev) * -std::expm1(-dt * 4.f);
  const float accel = forward >= 0.f ? (cam.chase_speed - prev) / dt : 0.f;
  cam.chase_accel += (std::clamp(accel, -15.f, 15.f) - cam.chase_accel) * -std::expm1(-dt * 2.5f);
  const float t = std::clamp((cam.chase_speed * 3.6f - 30.f) / 220.f, 0.f, 1.f);
  const float goal = kChaseFov + 20.f * t * t * (3.f - 2.f * t);
  cam.chase_fov = fresh ? goal : cam.chase_fov + (goal - cam.chase_fov) * -std::expm1(-dt * 2.f);
}

static bool driving_camera_pose(GameRefState& cam, float* eye, float* at,
                                float* up, float dt) {
  using physics::Vec3;
  if (cam.camera_free) {
    const float yaw = cam.ypr[0] + cam.orbit_yaw;
    const float pitch = std::clamp(cam.ypr[1] + cam.orbit_pitch, -1.5f, 1.5f);
    const Vec3 forward{-std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                       -std::cos(yaw) * std::cos(pitch)};
    const Vec3 right{std::cos(yaw), 0, -std::sin(yaw)};
    const auto& input = host_input();
    const Vec3 motion = (forward * input.free_camera_z + right * input.free_camera_x +
                         Vec3{0, input.free_camera_y, 0}) * (dt * 12);
    cam.pos[0] += float(motion.x);
    cam.pos[1] += float(motion.y);
    cam.pos[2] += float(motion.z);
    std::copy_n(cam.pos, 3, eye);
    at[0] = eye[0] + float(forward.x);
    at[1] = eye[1] + float(forward.y);
    at[2] = eye[2] + float(forward.z);
    return true;
  }

  const bool attached = cam.camera_attached && !cam.follow_variant;
  const int target_id = attached ? cam.move_id : cam.follow_vehicle;
  const auto* target = gameref_find_live(target_id);
  PartMatrix frame;
  if (!target || !part_camera_pose(target->key, cam.cockpit_index, frame.data()))
    return false;
  // The interior view widens with speed like the chase view (only its eye
  // stays fixed); a script-attached camera keeps its own zoom.
  if (!attached) update_chase_feel(cam, target->key, frame.data(), dt);
  if (cam.cockpit_index >= 0) {
    std::copy_n(frame.data() + 12, 3, eye);
    for (int i = 0; i < 3; ++i) at[i] = eye[i] - frame[8 + i];
    if (up) std::copy_n(frame.data() + 4, 3, up);
    return true;
  }
  if (attached) {
    target_point(&cam, cam.move_id, cam.move_off, eye);
    target_point(&cam, cam.look_id, cam.look_off, at);
    return true;
  }

  const double distance = cam.follow_variant == 4 ? 11. : cam.follow_variant == 3 ? 12. :
      std::clamp(cam.cam_distance > 0.f ? cam.cam_distance : 7.f,
                 cam.script_dist_min, cam.script_dist_max);
  // Speed feel: the eye drops back while the car pulls and closes in while
  // it brakes. Only the drawn radius moves; the rope tow keeps `distance`.
  const double reach = distance + std::clamp(cam.chase_accel * .12, -.8, 1.2);
  // Native follow camera (update 0x438b60; its angle gain stays 0): the body
  // feels only the dist spring along the eye-car line and perpendicular
  // damping, so time never undoes a look and a parked car keeps it. Driving
  // tows the eye back behind like a rope (tractrix): tan(a/2) shrinks by
  // exp(-s/r) over s metres of forward travel. This runs on every pose query,
  // including dt = 0 ones, because look_s is the last focus it saw.
  if (cam.cam_pose_init) {
    const double back = std::hypot(frame[8], frame[10]);
    const double s = back > 1e-6 ? -((frame[12] - cam.look_s[0]) * frame[8] +
                                     (frame[14] - cam.look_s[2]) * frame[10]) / back : 0.;
    if (s > 0.) {
      const double keep = std::exp(-s / distance);
      cam.orbit_yaw = float(2. * std::atan(std::tan(.5 * cam.orbit_yaw) * keep));
      cam.orbit_pitch = float(2. * std::atan(std::tan(.5 * cam.orbit_pitch) * keep));
    }
  }
  // SLRR_rev FollowCamera keeps the car's actual translation and damps
  // the offset. This avoids camera lag when a render frame has fewer ticks.
  const double azimuth = std::atan2(frame[8], frame[10]) + cam.orbit_yaw +
                         (cam.follow_variant == 4 ? 3.141592653589793 : 0);
  const double lift = cam.follow_variant == 4 ? 1.9 : cam.follow_variant == 3 ? 2.4 : 1.4;
  const double radius = std::hypot(reach, lift);
  const double elevation = std::clamp(std::atan2(lift, reach) + cam.orbit_pitch, -1.35, 1.35);
  const Vec3 offset{std::sin(azimuth) * std::cos(elevation) * radius,
                    std::sin(elevation) * radius,
                    std::cos(azimuth) * std::cos(elevation) * radius};
  const Vec3 focus{frame[12], frame[13] + 1.1, frame[14]};
  const Vec3 desired = focus + offset;
  const float position[3] = {float(desired.x), float(desired.y), float(desired.z)};
  const float center[3] = {float(focus.x), float(focus.y), float(focus.z)};
  if (!cam.cam_pose_init) {
    std::copy_n(position, 3, cam.eye_s);
    cam.cam_pose_init = true;
  } else if (cam.follow_hold || cam.angle_gain > 0.f) {
    cam.follow_hold = true;
    held_follow_pose(cam, center, float(std::atan2(frame[8], frame[10])), dt);
  } else {
    const float weight = -std::expm1(-dt * (cam.follow_variant ? 9.f : 5.f));
    for (int i = 0; i < 3; ++i)
      cam.eye_s[i] = std::lerp(cam.eye_s[i] + center[i] - cam.look_s[i], position[i], weight);
  }
  std::copy_n(center, 3, cam.look_s);
  std::copy_n(center, 3, at);
  std::copy_n(cam.eye_s, 3, eye);
  float delta[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
  SceneSphereHit hit;
  if (scene_instances_sweep_sphere(at, delta, .15f, hit)) {
    const float fraction = std::clamp(hit.fraction - .02f, .05f, 1.f);
    for (int i = 0; i < 3; ++i) eye[i] = at[i] + delta[i] * fraction;
  }
  if (dt > 0.f) {
    std::copy_n(eye, 3, cam.pos);
    cam.ypr[0] = std::atan2(eye[0] - at[0], eye[2] - at[2]);
    cam.ypr[1] = std::atan2(at[1] - eye[1], std::hypot(eye[0] - at[0], eye[2] - at[2]));
  }
  return true;
}

static bool world_camera_pose(float eye[3], float at[3], float* fov, float* znear,
                              float* zfar, float dt, float* up = nullptr) {
  GameRefState* cam = fullscreen_script_camera();
  if (!cam || !eye || !at) return false;
  if (up) { up[0] = 0; up[1] = 1; up[2] = 0; }
  if (dt > 0.f && cam->aov_goal > 0.f)
    cam->half_aov += (cam->aov_goal - cam->half_aov) * -std::expm1(-dt * cam->aov_rate);
  // A mod camera pose (#487) is the whole view. A Relative or Local pose
  // reads the car's drawn frame now, so the draw-time query matches the drawn
  // car even when the poses were published after the scripts ran
  // (free-running physics); Local also turns and banks with that frame.
  // Without its car it keeps the last pose written to pos/ypr.
  if (cam->mod_pose.mode != ModCameraMode::None) {
    float frame[16];
    const float* car = nullptr;
    if (cam->mod_pose.mode == ModCameraMode::Relative ||
        cam->mod_pose.mode == ModCameraMode::Local) {
      const GameRefState* target = gameref_find_live(cam->mod_pose_car);
      if (target && part_camera_pose(target->key, -1, frame)) car = frame;
    }
    float pose_up[3];
    if (mod_camera_resolve(cam->mod_pose, car, eye, at, pose_up)) {
      if (up) std::copy_n(pose_up, 3, up);
      if (fov) *fov = cam->mod_pose.fov > 0.f ? cam->mod_pose.fov
                                              : cam->half_aov > 1.f ? cam->half_aov : 60.f;
      // Closer than the chase view's .20 for bumper and hood eyes.
      if (znear) *znear = .10f;
      if (zfar) *zfar = 3000.f;
      // getPos / getOri on the camera (the GPS reads the renderer's) and
      // clearPose continue from here, roll included. Every resolution
      // writes back, the renderer's dt = 0 query too: in free-running play
      // the frame's car pose is published after the dt > 0 advance, so only
      // the draw-time resolution matches the drawn frame. The bound follow
      // camera's roll stays the script's `roll` command, as in setMatrix.
      std::copy_n(eye, 3, cam->pos);
      mod_look_ypr(eye, at, cam->ypr[0], cam->ypr[1]);
      cam->ypr[2] = mod_look_roll(eye, at, pose_up);
      if (!(cam->follow_vehicle && cam->cockpit_index < 0 && !cam->camera_free))
        cam->camera_roll = cam->ypr[2];
      return true;
    }
  }
  if (driving_camera_pose(*cam, eye, at, up, dt)) {
    const bool cockpit = cam->camera_attached && !cam->follow_variant;
    const bool chase = !cockpit && !cam->camera_free;
    if (fov) *fov = cockpit ? cam->half_aov : chase && cam->chase_fov > 0.f ? cam->chase_fov : kChaseFov;
    // Outside the cockpit, reserve more depth precision for close body seams.
    if (znear) *znear = cockpit ? .03f : .20f;
    if (zfar) *zfar = 3000.f;
    return true;
  }
  // SLRR_rev javaSceneCamera (codex/physics-reverse-port @ c65675f): the
  // eye keeps its horizontal offset from the move target, clamped to the
  // dist range. Height is the `height` command, or how far the body already
  // sits above the target. Look is the look target. No car: the garage
  // looks at map + defLookPos, the same focus point.
  eye[0] = cam->pos[0];
  eye[1] = cam->pos[1];
  eye[2] = cam->pos[2];
  float follow[3] = {eye[0], eye[1], eye[2]};
  const bool have_follow = cam->aim_move;
  if (have_follow) {
    target_point(cam, cam->move_id, cam->move_off, follow);
    // A car's `move` point is the ground under the chassis, not the origin
    // in the middle of the body. Height and distance hang on that point;
    // `look` still aims at the chassis.
    if (GameRefState *n = gameref_find_live(cam->move_id)) {
      float ground = follow[1];
      if (part_car_ground(n->key, &ground)) follow[1] = ground;
    }
  }
  if (cam->aim_look) {
    target_point(cam, cam->look_id, cam->look_off, at);
  } else {
    const float yaw = cam->ypr[0];
    const float pitch = cam->ypr[1];
    const float cp = std::cos(pitch);
    at[0] = eye[0] - std::sin(yaw) * cp;
    at[1] = eye[1] + std::sin(pitch);
    at[2] = eye[2] - std::cos(yaw) * cp;
  }
  // `smooth` is a Pori step on the two anchors, so changing car slides
  // instead of cutting. The drag then pulls the eye toward the pose on
  // that anchor; the spring is the resistance.
  if (!cam->cam_pose_init) {
    std::copy(at, at + 3, cam->look_s);
    std::copy(follow, follow + 3, cam->move_s);
  } else if (cam->aim_look && have_follow) {
    pori_step(cam->look_s, at, cam->look_smooth, dt);
    pori_step(cam->move_s, follow, cam->move_smooth, dt);
  }
  if (cam->aim_look && have_follow && cam->move_dist > 0.25f) {
    float dist = cam->cam_distance > 0.05f ? cam->cam_distance : cam->move_dist;
    if (cam->script_dist_set) {
      dist = std::max(cam->script_dist_min, std::min(cam->script_dist_max, dist));
    }
    if (!cam->cam_az_set) {
      const float dx = cam->pos[0] - follow[0];
      const float dz = cam->pos[2] - follow[2];
      cam->cam_base_az =
          (dx * dx + dz * dz) > 1e-6f ? std::atan2(dx, dz) : cam->ypr[0];
      const float oh = std::sqrt(dx * dx + dz * dz);
      cam->cam_base_elev = std::atan2(cam->pos[1] - at[1], oh > 1e-3f ? oh : 1.f);
      cam->cam_az_set = true;
    }
    const float azimuth = cam->cam_base_az - cam->orbit_yaw;
    const float pitch_lim = cam->cam_height_set ? 0.3f : 1.3f;
    const float pitch =
        std::max(-pitch_lim, std::min(pitch_lim, cam->orbit_pitch));
    const float* hub = cam->move_s;
    float desired[3];
    if (cam->cam_height_set) {
      const float lift = cam->cam_height;
      const float horiz =
          dist > lift + 0.05f ? std::sqrt(dist * dist - lift * lift) : dist;
      const float cp = std::cos(pitch);
      const float sp = std::sin(pitch);
      desired[0] = hub[0] + std::sin(azimuth) * horiz * cp;
      desired[1] = hub[1] + lift * cp + horiz * sp;
      desired[2] = hub[2] + std::cos(azimuth) * horiz * cp;
    } else {
      const float elev = cam->cam_base_elev + pitch;
      const float ce = std::cos(elev);
      const float se = std::sin(elev);
      desired[0] = hub[0] + std::sin(azimuth) * dist * ce;
      desired[1] = hub[1] + dist * se;
      desired[2] = hub[2] + std::cos(azimuth) * dist * ce;
    }
    if (!cam->cam_pose_init) {
      std::copy(desired, desired + 3, cam->eye_s);
      cam->eye_vel[0] = cam->eye_vel[1] = cam->eye_vel[2] = 0.f;
      cam->cam_pose_init = true;
    }
    if (dt > 0.f) drive_orbit(cam, hub, dist, dt, target_heading(cam, cam->move_id));
    std::copy(cam->eye_s, cam->eye_s + 3, eye);
    std::copy(cam->look_s, cam->look_s + 3, at);
  } else if (have_follow && !cam->aim_look) {
    // `move id x,y,z dist` with no look: the point is the eye. The trailing
    // number is how far short of it the body stops (car lot: 0.01). The
    // slide is a Pori step of 0.02 at 60 Hz: twice the first try, and the
    // step is scaled by frame time so the speed does not follow the fps.
    // Orientation stays the camera ypr.
    const float rate = 0.015f;
    if (!cam->cam_pose_init || rate <= 0.f || rate >= 1.f) {
      std::copy(follow, follow + 3, cam->eye_s);
      cam->cam_pose_init = true;
    } else {
      pori_step(cam->eye_s, follow, rate, dt);
    }
    std::copy(cam->eye_s, cam->eye_s + 3, eye);
    const float yaw = cam->ypr[0];
    const float pitch = cam->ypr[1];
    const float cp = std::cos(pitch);
    at[0] = eye[0] - std::sin(yaw) * cp;
    at[1] = eye[1] + std::sin(pitch);
    at[2] = eye[2] - std::cos(yaw) * cp;
  } else if (have_follow && cam->move_dist <= 0.25f) {
    eye[0] = follow[0];
    eye[1] = follow[1];
    eye[2] = follow[2];
  }
  if (up && cam->camera_roll != 0.f) {
    const auto forward = physics::Vec3{at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]}.normalized();
    const auto right = forward.cross({0, 1, 0}).normalized();
    if (right.dot(right) > 0) {
      const auto rolled = right.cross(forward) * std::cos(cam->camera_roll) +
                          right * std::sin(cam->camera_roll);
      up[0] = float(rolled.x); up[1] = float(rolled.y); up[2] = float(rolled.z);
    }
  }
  if (fov) {
    const auto delta = physics::Vec3{eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
    *fov = cam->camera_autozoom ? float(std::clamp(240.0 / std::max(1.0, delta.length()), 18.0, 65.0))
                               : cam->half_aov > 1.f ? cam->half_aov : 60.f;
  }
  if (znear) *znear = cam->dmin > 0.001f ? cam->dmin : 0.05f;
  if (zfar) *zfar = cam->dmax > 1.f ? cam->dmax : 500.f;
  return true;
}

bool gameref_world_camera(float eye[3], float at[3], float* fov, float* znear,
                          float* zfar, float* up) {
  return world_camera_pose(eye, at, fov, znear, zfar, 0.f, up);
}

void gameref_advance_world_camera(float dt, float time_warp) {
  g_camera_warp = time_warp;
  float eye[3], at[3];
  world_camera_pose(eye, at, nullptr, nullptr, nullptr, dt);
}

bool gameref_world_camera_can_drag() {
  const auto* cam = fullscreen_script_camera();
  return cam && (cam->camera_free || (cam->follow_vehicle && cam->cockpit_index < 0) ||
                 (cam->aim_move && cam->aim_look && cam->move_dist > .25f));
}

void gameref_orbit_world_camera(float yaw_delta, float pitch_delta, float key_yaw,
                                float key_pitch, float zoom_axis, float dt) {
  auto& in = host_input();
  float ax = 0.f;
  float ay = 0.f;
  const float dt_ms = dt > 1e-4f ? dt * 1000.f : 0.f;
  if (dt_ms > 0.f) {
    ax = in.mouse_dx / dt_ms * 0.005f;
    ay = in.mouse_dy / dt_ms * 0.005f;
    ax = std::max(-1.f, std::min(1.f, ax));
    ay = std::max(-1.f, std::min(1.f, ay));
  }
  ax += key_yaw;
  ay += key_pitch;
  GameRefState* cam = fullscreen_script_camera();
  if (cam && (cam->camera_free || (cam->follow_vehicle && cam->cockpit_index < 0))) {
    cam->orbit_yaw = std::remainder(cam->orbit_yaw + in.mouse_dx * .003f, 6.2831853f);
    cam->orbit_pitch = std::clamp(cam->orbit_pitch + in.mouse_dy * .003f, -1.1f, 1.1f);
  }
  in.mouse_dx = in.mouse_dy = 0.f;
  if (!cam || cam->cockpit_index >= 0) return;
  // Orbit cameras take the axes as a force. A free pose still tracks the
  // pointer directly.
  if (cam->aim_look && cam->move_dist > 0.25f) {
    cam->ctl_x = ax;
    cam->ctl_y = ay;
  } else if (!cam->follow_vehicle && !cam->camera_free) {
    cam->orbit_yaw += yaw_delta;
    const float pitch_lim = cam->cam_height_set ? 0.3f : 1.3f;
    cam->orbit_pitch = std::max(-pitch_lim, std::min(pitch_lim,
                                                     cam->orbit_pitch + pitch_delta));
  }
  if (dt < 0.f) dt = 0.f;
  if (dt > 0.1f) dt = 0.1f;
  if (cam->cam_distance < 0.05f) {
    cam->cam_distance = cam->move_dist > 0.05f ? cam->move_dist :
                         cam->follow_vehicle ? 7.f : 3.5f;
  }
  // Native camera: distance *= 1 + axis10 * 100 * dt, then the dist clamp.
  // A wheel detent arrives as ±1; the script maps that to a short pulse,
  // about a quarter of the length. PgUp / PgDn stay a small continuous axis.
  float axis = zoom_axis;
  if (std::fabs(axis) >= 0.5f) axis = std::copysign(0.15f, axis);
  if (std::fabs(axis) > 1e-5f && dt > 0.f) {
    float factor = 1.f - axis * 100.f * dt;
    if (factor < 0.25f) factor = 0.25f;
    cam->cam_distance *= factor;
  }
  if (cam->script_dist_set) {
    cam->cam_distance = std::max(cam->script_dist_min,
                                 std::min(cam->script_dist_max, cam->cam_distance));
  } else {
    cam->cam_distance = std::max(0.5f, std::min(80.f, cam->cam_distance));
  }
}

std::vector<GameRefState*> gameref_active_cameras() {
  std::vector<GameRefState*> out;
  std::vector<void*> stale;
  for (void* key : g_camera_refs) {
    const auto found = g_refs.find(key);
    if (found == g_refs.end() || found->second.kind != GameRefKind::Camera) {
      stale.push_back(key);
      continue;
    }
    auto& camera = found->second;
    if (!camera.cam_active || camera.binding) continue;
    // A camera cannot render through a hidden or destroyed viewport. Scene
    // ownership alone does not end its lifetime when a dialog/section exits.
    if (camera.viewport) {
      const auto* viewport = gameref_get(camera.viewport);
      if (!viewport || !viewport->vp_active) continue;
    }
    out.push_back(&camera);
  }
  for (void* key : stale) g_camera_refs.erase(key);
  // Equal priorities used to keep the node table's hash order; activation
  // order (cam_seq) is the same for every run (#268).
  std::sort(out.begin(), out.end(),
            [](const GameRefState* a, const GameRefState* b) {
              return a->cam_pri != b->cam_pri ? a->cam_pri < b->cam_pri
                                              : a->cam_seq < b->cam_seq;
            });
  return out;
}

std::vector<GameRefState*> gameref_nodes_with_mesh() {
  std::vector<GameRefState*> out;
  for (auto& kv : g_refs) {
    if (kv.second.has_mesh && kv.second.mesh.info.ok) out.push_back(&kv.second);
  }
  return out;
}
void gameref_register_light(void* key) {
  if (auto* node = gameref_get(key)) g_lights.insert(node->key);
}
std::vector<GameRefState*> gameref_directional_lights() {
  std::vector<GameRefState*> out;
  out.reserve(g_lights.size());
  for (void* key : g_lights) {
    auto* node = gameref_get(key);
    if (node && node->kind == GameRefKind::RenderRef && !node->empty &&
        node->type_id && node->directional_light.set) out.push_back(node);
  }
  return out;
}
std::vector<GameRefState*> gameref_render_nodes() {
  std::vector<GameRefState*> out;
  for (auto& kv : g_refs)
    if (kv.second.kind == GameRefKind::RenderRef && !kv.second.empty && kv.second.type_id)
      out.push_back(&kv.second);
  return out;
}

std::vector<void*> gameref_world_roots() {
  std::vector<void*> out;
  for (auto& kv : g_refs) {
    if (kv.second.flags & kWorldTreeRoot) out.push_back(kv.first);
  }
  return out;
}

void gameref_set_fog(int32_t color, float near_z, float far_z) {
  g_fog_color = color;
  g_fog_near = near_z;
  g_fog_far = far_z;
  g_fog = true;
}

bool gameref_get_fog(int32_t* color, float* near_z, float* far_z) {
  if (!g_fog) return false;
  if (color) *color = g_fog_color;
  if (near_z) *near_z = g_fog_near;
  if (far_z) *far_z = g_fog_far;
  return true;
}

void gameref_camera_set_fog(GameRefState& camera, int32_t color, float near_z,
                            float far_z) {
  camera.fog_set = true;
  camera.fog_color = color;
  camera.fog_near = near_z;
  camera.fog_far = far_z > near_z ? far_z : near_z + 1.f;
}


void gameref_visit_gc_roots(const GcRootVisitor& visit) {
  for (const auto& entry : g_refs) {
    const auto& n = entry.second;
    // A detached RenderRef type is owned by its script references.
    // ResourceRef.finalize releases its registry entry when the type dies.
    if (n.kind == GameRefKind::RenderRef && !n.live_id && !n.binding &&
        !n.parent && n.children.empty() && !n.flags &&
        n.class_name == "java.util.resource.RenderRef")
      continue;
    visit(entry.first); visit(n.binding); visit(n.parent); visit(n.script); visit(n.viewport);
    for (void* child : n.children) visit(child);
  }
}
}  // namespace slrr
