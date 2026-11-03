#pragma once

#include "slrr/scx_mesh.hpp"
#include <array>
#include <memory>
#include <vector>

namespace slrr {
class RpkIndex;
struct GameRefState;

struct SceneInstance {
  void* key = nullptr;
  std::shared_ptr<const ScxGeometry> geometry;
  // Column-major renderer matrix; script meters, with the SCX Z reflection.
  std::array<float, 16> transform{};
  bool sky = false;
  uint32_t color = 0xffffff;
};

void scene_instances_set_resources(const RpkIndex* resources);
void scene_instances_select(void* world, void* skydome);
std::vector<SceneInstance> scene_instances_collect();
std::vector<SceneInstance> scene_instances_collect_root(void* root);
// Rejects missing parents, hidden subtrees and cycles.
bool scene_instance_transform(void* key, void* root, float* matrix);
}  // namespace slrr
