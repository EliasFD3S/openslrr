#include "slrr/script_lights.hpp"
#include "slrr/gameref.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace slrr {
namespace {
struct Entry {
  ScriptLight light;
  uint64_t order = 0;
};
std::unordered_map<void *, Entry> g_script_lights;
uint64_t g_script_light_order = 0;
std::unordered_map<void *, SceneryLightList> g_scenery_lights;
} // namespace

bool script_lights_create(void *key, void *parent, ScriptLightKind kind) {
  auto *node = gameref_get(parent);
  if (!key || !node || node->key == key)
    return false;
  // A second create on the same object restarts it with default values.
  Entry entry;
  entry.light.key = key;
  entry.light.parent = node->key;
  entry.light.kind = kind;
  entry.order = ++g_script_light_order;
  g_script_lights[key] = entry;
  return true;
}

ScriptLight *script_lights_get(void *key) {
  const auto it = g_script_lights.find(key);
  return it == g_script_lights.end() ? nullptr : &it->second.light;
}

void script_lights_destroy(void *key) { g_script_lights.erase(key); }

void script_lights_forget_parent(void *parent) {
  if (!parent)
    return;
  for (auto it = g_script_lights.begin(); it != g_script_lights.end();)
    it = it->second.light.parent == parent ? g_script_lights.erase(it)
                                           : std::next(it);
  g_scenery_lights.erase(parent);
}

void script_lights_clear() {
  g_script_lights.clear();
  g_scenery_lights.clear();
}

size_t script_lights_count() { return g_script_lights.size(); }

std::vector<const ScriptLight *> script_lights_all() {
  std::vector<const Entry *> entries;
  entries.reserve(g_script_lights.size());
  for (const auto &[key, entry] : g_script_lights)
    entries.push_back(&entry);
  std::sort(entries.begin(), entries.end(),
            [](const Entry *a, const Entry *b) { return a->order < b->order; });
  std::vector<const ScriptLight *> out;
  out.reserve(entries.size());
  for (const auto *entry : entries)
    out.push_back(&entry->light);
  return out;
}

void script_lights_visit_gc_roots(const GcRootVisitor &visit) {
  // Scenery lights hold no Java object; their parent shell lives in the
  // world tree and is not rooted here.
  for (const auto &[key, entry] : g_script_lights) {
    visit(key);
    visit(entry.light.parent);
  }
}

bool scenery_lights_attach(void *parent, SceneryLightList lights) {
  auto *node = gameref_get(parent);
  if (!node || !lights || lights->empty())
    return false;
  g_scenery_lights[node->key] = std::move(lights);
  return true;
}

size_t scenery_lights_count() {
  size_t count = 0;
  for (const auto &[parent, list] : g_scenery_lights)
    count += list->size();
  return count;
}

std::vector<std::pair<void *, SceneryLightList>> scenery_lights_all() {
  return {g_scenery_lights.begin(), g_scenery_lights.end()};
}

std::vector<SceneryLightPick>
scenery_lights_near(const std::array<float, 3> &focus, float reach,
                    uint32_t groups_on,
                    const std::function<bool(void *, float *)> &matrix) {
  std::vector<SceneryLightPick> out;
  if (!groups_on || !matrix || !(reach > 0.f))
    return out;
  for (const auto &[parent, list] : g_scenery_lights) {
    float m[16];
    if (!matrix(parent, m))
      continue;
    for (const auto &light : *list) {
      if (!light.lights_scene || !(light.group & groups_on))
        continue;
      const auto &l = light.position;
      const std::array<float, 3> p{
          m[0] * l[0] + m[4] * l[1] + m[8] * l[2] + m[12],
          m[1] * l[0] + m[5] * l[1] + m[9] * l[2] + m[13],
          -(m[2] * l[0] + m[6] * l[1] + m[10] * l[2] + m[14])};
      const float d = std::sqrt((p[0] - focus[0]) * (p[0] - focus[0]) +
                                (p[1] - focus[1]) * (p[1] - focus[1]) +
                                (p[2] - focus[2]) * (p[2] - focus[2]));
      const float range = std::min(light.range, kSceneryLightMaxRange);
      if (!(d < reach) || !(range > .1f))
        continue;
      SceneryLightPick pick;
      pick.position = p;
      pick.color = light.color;
      pick.attenuation = light.attenuation;
      pick.range = range;
      pick.intensity = kSceneryLightIntensity;
      pick.distance = d;
      pick.source = &light;
      out.push_back(pick);
    }
  }
  std::sort(out.begin(), out.end(), [](const SceneryLightPick &a, const SceneryLightPick &b) {
    if (a.distance != b.distance)
      return a.distance < b.distance;
    if (a.position != b.position)
      return a.position < b.position;
    return a.source < b.source;
  });
  return out;
}

std::vector<SceneryLightPick>
scenery_lights_select(const std::array<float, 3> &eye, size_t budget,
                      uint32_t groups_on,
                      const std::function<bool(void *, float *)> &matrix,
                      const SceneryLightFrustum *frustum, float max_distance) {
  std::vector<SceneryLightPick> candidates;
  if (!budget || !groups_on || !matrix || !(max_distance > 0.f))
    return candidates;
  // Normalised planes, so the sphere test uses metres.
  SceneryLightFrustum planes{};
  if (frustum)
    for (size_t i = 0; i < planes.size(); ++i) {
      const auto &q = (*frustum)[i];
      const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
      if (length > 0.f && std::isfinite(length))
        planes[i] = {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
      else
        planes[i] = {0.f, 0.f, 0.f, 1.f}; // degenerate: accept everything
    }
  const float limit2 = max_distance * max_distance;
  for (const auto &[parent, list] : g_scenery_lights) {
    float m[16];
    if (!matrix(parent, m))
      continue;
    for (const auto &light : *list) {
      if (!light.lights_scene || !(light.group & groups_on))
        continue;
      const auto &l = light.position;
      // S*M*l with S = diag(1,1,-1): render space.
      const std::array<float, 3> p{
          m[0] * l[0] + m[4] * l[1] + m[8] * l[2] + m[12],
          m[1] * l[0] + m[5] * l[1] + m[9] * l[2] + m[13],
          -(m[2] * l[0] + m[6] * l[1] + m[10] * l[2] + m[14])};
      const float dx = p[0] - eye[0], dy = p[1] - eye[1], dz = p[2] - eye[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (!std::isfinite(d2) || d2 >= limit2)
        continue;
      SceneryLightPick pick;
      pick.position = p;
      pick.color = light.color;
      pick.attenuation = light.attenuation;
      pick.range = std::min(light.range, kSceneryLightMaxRange);
      pick.distance = std::sqrt(d2);
      pick.source = &light;
      if (!(pick.range > .1f))
        continue;
      bool visible = true;
      if (frustum)
        for (const auto &q : planes)
          if (q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3] < -pick.range) {
            visible = false;
            break;
          }
      if (visible)
        candidates.push_back(pick);
    }
  }
  // Ties break on position so the set does not depend on map iteration order.
  const auto nearer = [](const SceneryLightPick &a, const SceneryLightPick &b) {
    if (a.distance != b.distance)
      return a.distance < b.distance;
    if (a.position != b.position)
      return a.position < b.position;
    return a.source < b.source;
  };
  float cutoff = max_distance;
  if (candidates.size() > budget) {
    std::nth_element(candidates.begin(), candidates.begin() + budget,
                     candidates.end(), nearer);
    cutoff = candidates[budget].distance;
    candidates.resize(budget);
  }
  std::sort(candidates.begin(), candidates.end(), nearer);
  const float band = std::clamp(cutoff * .4f, 20.f, kSceneryLightFadeBand);
  for (auto &pick : candidates) {
    const float fade = std::clamp((cutoff - pick.distance) / band, 0.f, 1.f);
    pick.intensity = kSceneryLightIntensity * fade * fade * (3.f - 2.f * fade);
  }
  std::erase_if(candidates, [](const SceneryLightPick &pick) {
    return !(pick.intensity > 0.f);
  });
  return candidates;
}
} // namespace slrr
