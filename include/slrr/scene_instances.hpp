#pragma once

#include "slrr/scx_mesh.hpp"
#include "slrr/scene_sun.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace slrr {
class RpkIndex;
struct GameRefState;

struct SceneInstance {
  SceneSun lighting;
  void *key = nullptr;
  std::shared_ptr<const ScxGeometry> geometry;
  // Column-major renderer matrix; script meters, with the SCX Z reflection.
  std::array<float, 16> transform{};
  bool sky = false;
  uint32_t color = 0xffffff;
  int paint_slot = -1;
  std::string paint_texture;
  bool vehicle_material = false;
  uint32_t light_flags = 0;
  // Render-object id. Carries `flags` (dynamic shadow caster) and `shd_*`.
  int32_t type_id = 0;
};

// Flat blob under a traffic car (kocsiarnyek). Vectors are metres in the
// render object's local space, before the mesh Z flip.
struct SceneShadowDecal {
  bool present = false;
  float center[3] = {};
  float axis_u[3] = {};
  float axis_v[3] = {};
  float uv[4] = {0.f, 0.f, 1.f, 1.f};
  std::string texture;
};

void scene_instances_set_resources(const RpkIndex *resources);
void scene_instances_set_envmap(uint32_t resource);
std::string scene_instances_envmap();
void scene_instances_select(void *world, void *skydome);
std::vector<SceneInstance> scene_instances_collect();
std::vector<SceneInstance> scene_instances_collect_root(void *root);
SceneSun scene_instances_light(void* root=nullptr);
// Rejects missing parents, hidden subtrees and cycles.
bool scene_instance_transform(void *key, void *root, float *matrix);
// Active world, unreflected script metres. False for inventory/other worlds.
bool scene_instances_script_matrix(void *key, float *matrix);
// Nearest mesh the ray enters, in unreflected script metres. Sky and
// instances outside the active world are ignored. Null when the ray only
// starts inside a volume (the garage shell around the camera).
void *scene_instances_pick(const float origin[3], const float direction[3], float* point = nullptr);
bool scene_instances_pointer_ray(int px, int py, int width, int height,
                                 float origin[3], float direction[3]);
// Ground maps (dealer, car lot, Valo City) keep their triangles in the
// typeof-1 instance tree. `params` names the material table. On success
// *render_id is that render; *poly is the mesh in metres, or null when the
// tree has no POLY sections.
bool scene_instances_ground_map(const RpkIndex &rpk, uint32_t ground_id,
                                uint32_t *render_id,
                                std::shared_ptr<const ScxGeometry> *poly);
// Drawn instead of the render object's .scx (a ground template is materials).
void scene_instances_override_mesh(uint32_t id,
                                   std::shared_ptr<const ScxGeometry> geo);
// Boot garage shells are drawn from the renderer preload, not as RenderRefs.
// Their triangles stay here so a parked car can find the concrete.
void scene_instances_add_static_mesh(const ScxGeometry &geo);
// The boot preload belongs to the garage, not to dealer/track worlds.
void scene_instances_enable_static_floor(bool enabled);
// Highest surface at script (x, z) that is not above `below`. Skips the sky,
// vehicle materials, and anything parented under `ignore` (the car).
bool scene_instances_floor(float x, float z, float below, void *ignore,
                           float *height);
// Refresh once per frame; unchanged terrain keeps its spatial index.
void scene_instances_prepare_physics();
uint64_t scene_instances_floor_builds();
// Changes whenever the world-space floor used by projected shadows changes.
uint64_t scene_instances_floor_revision();
struct SceneFloorHit {
  float height = 0;
  float normal[3] = {0, 1, 0};
};
// Renderer-space projection onto the sampled receiver, along the scene light.
bool scene_shadow_projection(const float *script_frame,
                             const SceneFloorHit &hit, float *matrix);
// `flags` is the render-object flags word (8 casts a dynamic shadow).
// `decal` is set when shd_texture and both axes are real.
bool scene_shadow_info(uint32_t type_id, uint32_t *flags,
                       SceneShadowDecal *decal);
bool scene_instances_sample_floor(float x, float z, float below, void *ignore,
                                  SceneFloorHit &hit);
// One entry per mesh under `root`: x, z and the lowest vertex Y, in script
// space (the same Y the chassis pose uses).
bool scene_instances_supports(void *root,
                              std::vector<std::array<float, 3>> &points);
} // namespace slrr
