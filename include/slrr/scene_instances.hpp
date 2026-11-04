#pragma once

#include "slrr/scx_mesh.hpp"
#include <array>
#include <cstdint>
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
  int paint_slot = -1;
  std::string paint_texture;
  bool vehicle_material = false;
};

void scene_instances_set_resources(const RpkIndex* resources);
void scene_instances_set_envmap(uint32_t resource);
std::string scene_instances_envmap();
void scene_instances_select(void* world, void* skydome);
std::vector<SceneInstance> scene_instances_collect();
std::vector<SceneInstance> scene_instances_collect_root(void* root);
// Rejects missing parents, hidden subtrees and cycles.
bool scene_instance_transform(void* key, void* root, float* matrix);
// Ground maps (dealer, car lot, Valo City) keep their triangles in the
// typeof-1 instance tree. `params` names the material table. On success
// *render_id is that render; *poly is the mesh in metres, or null when the
// tree has no POLY sections.
bool scene_instances_ground_map(const RpkIndex& rpk, uint32_t ground_id,
                                uint32_t* render_id,
                                std::shared_ptr<const ScxGeometry>* poly);
// Drawn instead of the render object's .scx (a ground template is materials).
void scene_instances_override_mesh(uint32_t id,
                                   std::shared_ptr<const ScxGeometry> geo);
}  // namespace slrr
