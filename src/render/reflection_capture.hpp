#pragma once
#include "slrr/gameref.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/scene_instances.hpp"
namespace slrr {
// Environment probes omit the vehicle subtree, including seated occupants.
// Independent walkers/people remain part of the reflected environment.
inline bool reflection_capture_visible(const SceneInstance &object) {
  if (object.sky || object.vehicle_material || part_render_owner(object.key))
    return false;
  void *key = object.key;
  for (int depth = 0; key && depth < 32; ++depth) {
    const auto *node = gameref_get(key);
    if (!node)
      break;
    if (node->vehicle_material)
      return false;
    key = node->parent;
  }
  return true;
}
} // namespace slrr
