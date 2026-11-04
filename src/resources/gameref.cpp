#include "slrr/gameref.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace slrr {
namespace {

std::unordered_map<void*, GameRefState> g_refs;
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
  auto own=g_refs.find(key);
  if(own!=g_refs.end()&&own->second.binding){g_refs.erase(own);return;}
  GameRefState* n = gameref_get(key);
  if (n) unlink_from_parent(n);
  // Orphan children stay keyed but parent cleared.
  for (auto& kv : g_refs) {
    if (kv.second.parent == key) kv.second.parent = nullptr;
  }
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

}  // namespace slrr
