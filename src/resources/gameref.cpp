#include "slrr/gc_roots.hpp"
#include "slrr/gameref.hpp"
#include "slrr/script_events.hpp"
#include "slrr/host_input.hpp"
#include "slrr/part_runtime.hpp"

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
std::unordered_set<void*> g_lights;
int32_t g_next_live = 0x70000000;
int32_t g_fog_color = 0;
float g_fog_near = 0.f;
float g_fog_far = 0.f;
bool g_fog = false;

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

void gameref_clear() {
  g_refs.clear();
  g_lights.clear();
  g_next_live = 0x70000000;
  g_fog = false;
}

GameRefState* gameref_upsert(void* key, GameRefKind kind) {
  if (!key) return nullptr;
  auto it = g_refs.find(key);
  if (it != g_refs.end()) {
    if (kind != GameRefKind::Generic) it->second.kind = kind;
    it->second.empty = false;
    return &it->second;
  }
  GameRefState n;
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
  for(auto& entry:g_refs)if(!entry.second.binding&&entry.second.live_id==id)return &entry.second;
  return nullptr;
}
int32_t gameref_make_live(void* key) {
  auto* state=gameref_get(key);if(!state)state=gameref_upsert(key);if(!state)return 0;
  if(!state->live_id){while(gameref_find_live(g_next_live))++g_next_live;state->live_id=g_next_live++;}
  return state->live_id;
}
bool gameref_bind_alias(void* reference,void* target) {
  auto* canonical=gameref_get(target);if(!reference||!canonical)return false;
  if(reference==canonical->key)return true;
  auto* ref=gameref_upsert(reference);ref->binding=canonical->key;return true;
}

void gameref_remove(void* key) {
  if (!key) return;
  script_events_destroy(static_cast<Object*>(key));
  auto own=g_refs.find(key);
  if(own!=g_refs.end()&&own->second.binding){g_refs.erase(own);return;}
  GameRefState* n = gameref_get(key);
  if (n) unlink_from_parent(n);
  // Orphan children stay keyed but parent cleared.
  for (auto& kv : g_refs) {
    if (kv.second.parent == key) kv.second.parent = nullptr;
  }
  g_lights.erase(key);
  g_refs.erase(key);
}

void gameref_set_parent(void* key, void* parent) {
  GameRefState* n = gameref_get(key);
  if (!n) n = gameref_upsert(key);
  if (!n) return;
  key=n->key;
  if(auto* canonical_parent=gameref_get(parent))parent=canonical_parent->key;
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

void gameref_set_matrix(void* key, float px, float py, float pz, float yaw,
                        float pitch, float roll) {
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
}

void gameref_set_event_mask(void* key, int32_t mask) {
  GameRefState* n = gameref_upsert(key, GameRefKind::GameType);
  if (n) n->event_mask |= mask;
}

void gameref_clear_event_mask(void* key, int32_t mask) {
  GameRefState* n = gameref_get(key);
  if (n) n->event_mask &= ~mask;
}

void gameref_add_timer(void* key, float deadline_abs, int32_t timer_id) {
  GameRefState* n = gameref_upsert(key, GameRefKind::GameType);
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
  st->kind = GameRefKind::Camera;
  st->cam_active = false;
  st->half_aov = 60.f;
  st->dmin = 0.05f;
  st->dmax = 500.f;
  gameref_make_live(key);
  float p[6] = {};
  if (std::sscanf(params.c_str(), " %f , %f , %f , %f , %f , %f", &p[0], &p[1],
                  &p[2], &p[3], &p[4], &p[5]) >= 3)
    gameref_set_matrix(key, p[0], p[1], p[2], p[3], p[4], p[5]);
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
  if (n) {
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
      st->viewport = vp->key;
      st->cam_pri = pri;
      st->cam_active = true;
      static int32_t seq = 0;
      st->cam_seq = ++seq;
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
    refresh_aim(st);
    return false;
  }
  if (std::strcmp(word, "move") == 0) {
    int id = 0;
    float off[3] = {};
    float dist = 0.f;
    std::sscanf(text, "%*s %d %f , %f , %f %f", &id, &off[0], &off[1], &off[2],
                &dist);
    st->move_id = id;
    std::copy(off, off + 3, st->move_off);
    st->move_dist = dist;
    st->cam_distance = dist;
    if (st->script_dist_set) {
      if (st->cam_distance < st->script_dist_min)
        st->cam_distance = st->script_dist_min;
      if (st->cam_distance > st->script_dist_max)
        st->cam_distance = st->script_dist_max;
    }
    st->aim_move = true;
    refresh_aim(st);
    return false;
  }
  if (std::strcmp(word, "height") == 0) {
    if (std::sscanf(text, "%*s %f", &st->cam_height) == 1)
      st->cam_height_set = true;
    return false;
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
    float aov = st->half_aov;
    std::sscanf(text, "%*s %f", &aov);
    if (aov > 1.f && aov < 179.f) st->half_aov = aov;
    GameRefState* vp = st->viewport ? gameref_get(st->viewport) : nullptr;
    return !vp || vp->vp_w >= 0.9f || vp->vp_h >= 0.9f;
  }
  if (std::strcmp(word, "nearplane") == 0) {
    std::sscanf(text, "%*s %f", &st->dmin);
    return false;
  }
  if (std::strcmp(word, "farplane") == 0) {
    std::sscanf(text, "%*s %f", &st->dmax);
    return false;
  }
  return std::strcmp(word, "torque") == 0;
}

GameRefState* fullscreen_script_camera() {
  GameRefState* found = nullptr;
  for (GameRefState* c : gameref_active_cameras()) {
    if (!c || !c->cam_active) continue;
    GameRefState* vp = c->viewport ? gameref_get(c->viewport) : nullptr;
    if (vp && (vp->vp_w < 0.99f || vp->vp_h < 0.99f)) continue;
    // Osd.show() adds a second fullscreen camera (priority 2, parent = the
    // Osd) after the scene camera. It exists for 3D widgets; those are drawn
    // as 2D. Taking it as the world camera looks past the car.
    if (GameRefState* parent = c->parent ? gameref_get(c->parent) : nullptr)
      if (parent->class_name.find("Osd") != std::string::npos) continue;
    if (!found || c->cam_seq >= found->cam_seq) found = c;
  }
  return found;
}

// Pori step: current += (dest - current) * rate, once per tick at 60 Hz.
void pori_step(float* cur, const float* dest, float rate, float dt) {
  float k = rate;
  if (dt > 0.f && rate > 0.f && rate < 1.f)
    k = 1.f - std::pow(1.f - rate, dt * 60.f);
  if (k < 0.f) k = 0.f;
  if (k > 1.f) k = 1.f;
  for (int i = 0; i < 3; ++i) cur[i] += (dest[i] - cur[i]) * k;
}

// PE camera body (FUN_004897b0 / FUN_0048a430). The mouse is a force on a
// 0.05 kg body. The spring only restores the distance to the hub, and the
// perpendicular term brakes sideways speed. Releasing the drag leaves the
// body where it is: nothing pulls the angle back, so there is no bounce.
// The two forces do not cancel cleanly, which is the tremble.
void drive_orbit(GameRefState* cam, const float* hub, float dist, float frame_dt) {
  const float ax = cam->ctl_x;
  const float ay = cam->ctl_y;
  cam->ctl_x = 0.f;
  cam->ctl_y = 0.f;
  float rem = frame_dt;
  if (rem < 0.f) rem = 0.f;
  if (rem > 0.1f) rem = 0.1f;
  constexpr float kStep = 0.02f;
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
    p[0] += v[0] * dt + axa * 0.5f * dt * dt;
    p[1] += v[1] * dt + aya * 0.5f * dt * dt;
    p[2] += v[2] * dt + aza * 0.5f * dt * dt;
    v[0] += axa * dt;
    v[1] += aya * dt;
    v[2] += aza * dt;
  }
}

bool gameref_world_camera(float eye[3], float at[3], float* fov, float* znear,
                          float* zfar) {
  GameRefState* cam = fullscreen_script_camera();
  if (!cam || !eye || !at) return false;
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
  const float now = gameref_now();
  float dt = cam->cam_clock < 0.f ? 0.f : now - cam->cam_clock;
  cam->cam_clock = now;
  if (dt < 0.f) dt = 0.f;
  if (dt > 0.1f) dt = 0.1f;
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
    drive_orbit(cam, hub, dist, dt);
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
  if (fov) *fov = cam->half_aov > 1.f ? cam->half_aov : 60.f;
  if (znear) *znear = cam->dmin > 0.001f ? cam->dmin : 0.05f;
  if (zfar) *zfar = cam->dmax > 1.f ? cam->dmax : 500.f;
  return true;
}

void gameref_orbit_world_camera(float yaw_delta, float pitch_delta,
                                float zoom_axis, float dt) {
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
  if (in.keys_left) ax += 0.005f;
  if (in.keys_right) ax -= 0.005f;
  if (in.keys_up) ay += 0.003f;
  if (in.keys_down) ay -= 0.003f;
  in.mouse_dx = 0.f;
  in.mouse_dy = 0.f;
  GameRefState* cam = fullscreen_script_camera();
  if (!cam) return;
  // Orbit cameras take the axes as a force. A free pose still tracks the
  // pointer directly.
  if (cam->aim_look && cam->move_dist > 0.25f) {
    cam->ctl_x = ax;
    cam->ctl_y = ay;
  } else {
    cam->orbit_yaw += yaw_delta;
    const float pitch_lim = cam->cam_height_set ? 0.3f : 1.3f;
    cam->orbit_pitch = std::max(-pitch_lim, std::min(pitch_lim,
                                                     cam->orbit_pitch + pitch_delta));
  }
  if (dt < 0.f) dt = 0.f;
  if (dt > 0.1f) dt = 0.1f;
  if (cam->cam_distance < 0.05f) {
    cam->cam_distance = cam->move_dist > 0.05f ? cam->move_dist : 3.5f;
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
  for (auto& kv : g_refs) {
    if (kv.second.kind == GameRefKind::Camera && kv.second.cam_active)
      out.push_back(&kv.second);
  }
  std::sort(out.begin(), out.end(),
            [](const GameRefState* a, const GameRefState* b) {
              return a->cam_pri < b->cam_pri;
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


void gameref_visit_gc_roots(const GcRootVisitor& visit) {
  for (const auto& entry : g_refs) {
    const auto& n = entry.second;
    visit(entry.first); visit(n.binding); visit(n.parent); visit(n.script); visit(n.viewport);
    for (void* child : n.children) visit(child);
  }
}
}  // namespace slrr
