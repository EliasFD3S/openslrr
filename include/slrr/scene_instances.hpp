#pragma once

#include "slrr/scx_mesh.hpp"
#include "slrr/scene_sun.hpp"
#include "slrr/vehicle_dynamics.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace slrr {
class RpkIndex;
struct GameRefState;

// Resolve an authored SCX group material for an original mesh triangle. Missing
// triangle/group coverage is unknown, never a synthesized material identity.
inline std::optional<std::uint32_t> scene_mesh_material(
    const ScxGeometry& geometry, std::optional<std::uint32_t> triangle) {
  if (!triangle || *triangle > (UINT32_MAX - 2) / 3) return std::nullopt;
  const std::uint32_t first = *triangle * 3;
  for (const auto& group : geometry.groups) {
    if (first < group.index_start) continue;
    const auto offset = first - group.index_start;
    if (offset < group.index_count) return group.material_type;
  }
  return std::nullopt;
}

struct SceneInstance {
  SceneSun lighting;
  bool cull_backfaces = false;
  void *key = nullptr;
  std::shared_ptr<const ScxGeometry> geometry;
  // Column-major renderer matrix; script meters, with the SCX Z reflection.
  std::array<float, 16> transform{};
  bool sky = false;
  uint32_t color = 0xffffff;
  int paint_slot = -1;
  std::string paint_texture;
  std::vector<std::pair<size_t, std::string>> texture_replacements;
  bool vehicle_material = false;
  uint32_t light_flags = 0;
  // Render-object id. Carries `flags` (dynamic shadow caster) and `shd_*`.
  int32_t type_id = 0;
  // Upload-space bone palette (scaled, Z flipped). Empty keeps the CPU pose.
  // Inside a renderer frame, a walker outside the collecting view has
  // gpu_skin set and an empty palette; it is kept for residency, not drawn.
  bool gpu_skin = false;
  std::vector<float> skin;
};

// Flat blob under a traffic car (kocsiarnyek). Vectors are metres in the
// render object's local space, before the mesh Z flip.
struct SceneBatchInstance {
  std::array<float, 16> transform{};
  uint32_t color = 0xffffff;
};
struct SceneBatch {
  std::shared_ptr<const ScxGeometry> geometry;
  SceneSun lighting{};
  std::vector<SceneBatchInstance> instances;
};

struct SceneShadowDecal {
  bool present = false;
  float center[3] = {};
  float axis_u[3] = {};
  float axis_v[3] = {};
  float uv[4] = {0.f, 0.f, 1.f, 1.f};
  std::string texture;
};

struct SceneInstanceCacheStats {
  std::size_t meshes = 0;
  std::size_t mesh_overrides = 0;
  std::size_t collision_overrides = 0;
  std::size_t texture_paths = 0;
  std::size_t ground_prop_maps = 0;
  std::size_t static_meshes = 0;
};
SceneInstanceCacheStats scene_instances_cache_stats();

void scene_instances_set_resources(const RpkIndex *resources);
void scene_instances_set_envmap(uint32_t resource);
std::string scene_instances_envmap();
void scene_instances_select(void *world, void *skydome, bool preview_lamps);
using SceneFrustum = std::array<std::array<float, 4>, 6>;
bool scene_instances_has_ground_props();
void* scene_instances_world();
// Previous-frame occlusion. A true result keeps the sphere. Null skips the
// test, which is what the shadow map and the reflection faces do.
using SceneHizTest = bool (*)(const float center[3], float radius, void* user);
void scene_instances_set_hiz(SceneHizTest test, void* user);
std::vector<SceneInstance> scene_instances_collect(
    const SceneFrustum* frustum = nullptr,
    std::vector<std::shared_ptr<const ScxGeometry>>* resident_props = nullptr);
// Repeated opaque props already grouped per cell. reflection_origin, in
// render metres, drops placements that cover fewer than about four pixels
// of a 256² reflection face.
void scene_instances_collect_batches(const SceneFrustum* frustum,
                                     std::vector<SceneBatch>& out,
                                     const float* reflection_origin = nullptr);
std::vector<SceneInstance> scene_instances_collect_root(void *root);
// The renderer encloses one flush in this scope. Inside it, collections of
// the active world reuse a single snapshot of its render objects instead of
// walking and posing them again for every view (camera, shadow, reflection).
// Nothing in a flush changes the scene graph. Pedestrian bone palettes are
// built only for walkers inside the frustum passed to scene_instances_collect.
void scene_instances_frame_cache(bool enabled);
SceneSun scene_instances_light(void* root=nullptr);
// Rejects missing parents, hidden subtrees and cycles.
bool scene_instance_transform(void *key, void *root, float *matrix);
// Active world, unreflected script metres. False for inventory/other worlds.
bool scene_instances_script_matrix(void *key, float *matrix);
// Render ground-shell matrix for placements owned by a GroundRef. Unlike the
// GroundRef's own pose, this is the frame used by static map placements.
bool scene_instances_ground_frame(void *ground, float *matrix);
// A node's own pose (pos and ypr, or its local matrix override) relative to
// its parent, column-major, script metres.
void scene_instances_local_matrix(const GameRefState& node, float matrix[16]);
// Collision mesh (object frame, metres), positive body mass and mass centre
// of a native object type's CFG. False when the type has no collision body.
bool scene_instances_native_body(uint32_t type, std::shared_ptr<const ScxGeometry>& mesh,
                                 float& mass, float center[3]);
// Positive-mass map placements are drawn as ordinary props until a vehicle
// comes close. Queue every dormant placement overlapping a world-space box,
// then create the loose bodies once the caller is outside its own iteration.
void scene_instances_wake_dynamics(const float lower[3], const float upper[3]);
void scene_instances_promote_dynamics();
// A promoted placement goes back to being an ordinary prop. With a world frame
// (script metres, in the world it was promoted in) its render instances and
// wake bounds move to that pose, so a knocked-over bin stays where it settled
// for the rest of the session; without one it reappears where it was before
// promotion. Returns false when node is not a promoted placement.
bool scene_instances_settle_dynamic(void* node, const float* world_frame);
std::shared_ptr<const ScxGeometry> scene_instances_geometry(uint32_t resource);
// Parse render meshes off the calling thread and publish them before return.
void scene_instances_preload(const std::vector<uint32_t>& resources);
std::string scene_instances_texture(uint32_t resource);
// Nearest mesh the ray enters, in unreflected script metres. Sky and
// instances outside the active world are ignored. Null when the ray only
// starts inside a volume (the garage shell around the camera).
void *scene_instances_pick(const float origin[3], const float direction[3], float* point = nullptr);
bool scene_instances_pointer_ray(int px, int py, int width, int height,
                                 float origin[3], float direction[3]);
// A typeof-1 ground tree can include render triangles, collision triangles
// and links to exterior packages. The meshes use script metres.
struct GroundMapData {
  struct Placement {
    uint32_t type = 0;
    int32_t seed = -1;
    std::array<float, 3> position{}, rotation{};
  };
  uint32_t render_id = 0;
  std::shared_ptr<const ScxGeometry> poly, collision;
  std::vector<uint32_t> exterior_maps;
  std::vector<Placement> placements;
};
GroundMapData scene_instances_ground_map(const RpkIndex &rpk, uint32_t ground_id);
void scene_instances_add_ground_props(const GroundMapData& map);
// Drawn instead of the render object's .scx (a ground template is materials).
void scene_instances_override_mesh(uint32_t id,
                                   std::shared_ptr<const ScxGeometry> geo);
void scene_instances_override_collision(uint32_t id,
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
struct SceneSphereHit {
  float fraction = 1.f;
  float normal[3] = {};
  float point[3] = {};
  float penetration = 0.f;
  physics::ContactIdentity identity;
};
// Sweep through the indexed static scene in script metres.
bool scene_instances_sweep_sphere(const float start[3], const float delta[3],
                                  float radius, SceneSphereHit &hit,
                                  float max_vertical_normal = 1.f);
struct SceneBodyContact {
  float normal[3] = {};
  float point[3] = {};
  float penetration = 0.f;
  void* owner = nullptr;
};
// Full oriented chassis bounds against indexed native boxes and spheres.
// The frame transforms local bounds to script metres; half is XYZ half size.
bool scene_instances_contact_box(const float frame[16], const float half[3],
                                 SceneBodyContact& contact,
                                 const void* ignore = nullptr);
// The same oriented box against the wall triangles (|normal.y| at most
// max_vertical_normal) of every static triangle set the sphere sweep uses, so
// a house corner or a fence end between the chassis corner probes still stops
// the car. Deepest overlap wins.
bool scene_instances_contact_walls(const float frame[16], const float half[3],
                                   SceneBodyContact& contact,
                                   float max_vertical_normal = .65f);
// Part-local triangle queries. The sweep retains an earlier hit if supplied.
bool scene_mesh_sweep_sphere(const ScxGeometry& mesh, physics::Vec3 start,
                             physics::Vec3 movement, double radius, SceneSphereHit& hit);
double scene_mesh_nearest(const ScxGeometry& mesh, physics::Vec3 point,
                          physics::Vec3& closest);
uint64_t scene_instances_floor_builds();
// Changes whenever the world-space floor used by projected shadows changes.
uint64_t scene_instances_floor_revision();
struct SceneFloorHit {
  float height = 0;
  float normal[3] = {0, 1, 0};
  physics::ContactIdentity identity;
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
