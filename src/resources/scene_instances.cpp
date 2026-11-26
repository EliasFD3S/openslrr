#include "slrr/gc_roots.hpp"
#include "slrr/frame_profiler.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/legacy_neon.hpp"
#include "slrr/light_resource.hpp"

#include "slrr/paint_texture.hpp"
#include "slrr/ptx_image.hpp"
#include "slrr/scene_collision.hpp"
#include <limits>
#include "slrr/gameref.hpp"
#include "slrr/job_system.hpp"
#include "slrr/object.hpp"
#include "slrr/part_config.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/race_runtime.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/scene_ambience.hpp"
#include "slrr/script_lights.hpp"
#include "slrr/skeleton.hpp"
#include "slrr/vehicle_dynamics.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace slrr {
namespace {
const RpkIndex *g_resources = nullptr;
void *g_world = nullptr;
SceneSizeFilter g_size_filter = nullptr;
void* g_size_filter_user = nullptr;
void *g_sky = nullptr;
bool g_preview_lamps = false;
SceneEnvironment g_environment_kind = SceneEnvironment::Automatic;
uint32_t g_environment = 0;
struct ShadowInfo {
  uint32_t flags = 0;
  SceneShadowDecal decal;
};
std::unordered_map<uint32_t, ShadowInfo> g_shadow_info;
std::unordered_map<uint32_t, std::shared_ptr<const ScxGeometry>> g_meshes;
std::unordered_map<uint32_t, std::shared_ptr<const ScxGeometry>> g_overrides;
std::unordered_map<uint32_t, std::shared_ptr<const ScxGeometry>> g_collision_overrides;
std::unordered_map<uint32_t, std::string> g_texture_paths;
struct StaticMesh {
  std::vector<float> positions;
  std::vector<uint32_t> indices;
  float scale = 0.01f;
  float cell_size = 2.f;
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
  std::vector<uint32_t> large_triangles;
  // Ground surfaces (#452), as in ScxGeometry: per triangle, into `surfaces`.
  std::vector<uint16_t> triangle_surfaces;
  std::vector<GroundSurface> surfaces;
  // With surfaces, each cell list's triangles' surface ids, one per triangle
  // in the list's order (cells hold vertex triples, not triangle ids).
  std::unordered_map<uint64_t, std::vector<uint16_t>> surface_cells;
  std::vector<uint16_t> large_surfaces;
  GroundSurface surface(uint16_t s) const {
    return s < surfaces.size() ? surfaces[s] : GroundSurface{};
  }
  // The surface ids parallel to `list` (a cell list or large_triangles of
  // this mesh), or null when the mesh has none.
  const std::vector<uint16_t>* surfaces_for(const std::vector<uint32_t>& list,
                                            uint64_t cell) const {
    if (triangle_surfaces.empty()) return nullptr;
    if (&list == &large_triangles) return &large_surfaces;
    const auto found = surface_cells.find(cell);
    return found == surface_cells.end() ? nullptr : &found->second;
  }
};
std::vector<StaticMesh> g_static;
bool g_static_enabled = true;
uint64_t floor_cell(int32_t x, int32_t z) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
         static_cast<uint32_t>(z);
}
struct GroundPropBatch {
  uint32_t render_id = 0;
  std::vector<uint32_t> indices;
  uint32_t slot = 0;  // index in GroundPropMap::batch_ids (#260)
};
struct GroundProp {
  uint32_t render_id;
  PartMatrix pose;
  uint32_t color;
  std::array<float, 3> center{};
  float radius = 0.f;
  bool batched = false;
  bool hidden = false;   // replaced by a live loose body
  int32_t dormant = -1;  // index into GroundPropMap::dormant, or -1
  bool night_only = false;
  int32_t night_slot = -1;  // GroundPropMap::night_ids index of a night-only prop (#264)
  bool signal = false;
  RoadPoint signal_position{};
  int8_t lod_min = -1, lod_max = -1;  // stock `lod first last`; -1 always drawn
  int32_t lod_index = -1;             // GroundPropMap::lods entry of its placement
  // World cache (#260): script-space centre and render-space instance matrix
  // (S*shell*pose*S) for GroundPropMap::cached_shell.
  std::array<float, 3> world_center{};
  PartMatrix render_matrix{};
};
// One per placement with a LOD chain (#252): shell-local centre and radius
// covering all its LOD renders, the type's thresholds and the level chosen
// by the main view (-1 until first chosen).
struct PlacementLod {
  std::array<float, 3> center{};
  float radius = 0.f;
  std::vector<float> sizes;
  int level = -1;
  uint64_t epoch = 0;  // g_lod_epoch of the update that chose `level` (#257)
};
struct GroundPropCell {
  std::vector<GroundProp> props;
  std::vector<GroundPropBatch> batches;
  std::array<float, 3> center{};
  float radius = 0.f;
  std::array<float, 3> world_center{};  // for GroundPropMap::cached_shell (#260)
};
// A positive-mass placement stays an instanced render prop until a car comes
// close. Only then does it become a live loose body, so a map with thousands of
// small props costs nothing per tick while they lie undisturbed.
struct DormantProp {
  std::shared_ptr<const PartConfig> config;
  PartMatrix pose{};
  int32_t seed = 0;
  std::array<float, 3> local_lower{}, local_upper{};  // collision bounds, object frame
  std::array<float, 3> lower{}, upper{};  // shell-local bounds, padded
  bool promoted = false;
  void* live = nullptr;     // the loose body's node while promoted
  PartMatrix live_shell{};  // its ground shell's world frame, captured on promotion
  std::vector<std::pair<GroundPropCell*, uint32_t>> props;  // render instances to hide
};
struct GroundPropMap {
  std::shared_ptr<const ScxGeometry> signal_geometry;
  uint64_t signal_stamp=0;
  std::vector<DormantProp> dormant;
  std::unordered_map<uint64_t, std::vector<uint32_t>> dormant_grid;
  std::unordered_map<uint64_t, GroundPropCell> cells;
  std::vector<uint32_t> resources;
  std::vector<StaticMesh> collision_meshes;
  collision::StaticIndex collision_shapes;
  // Type-13 `render` children of the placements, shell-local (#237). Each
  // new shell of this map attaches the same list to the light registry.
  SceneryLightList lights;
  std::vector<PlacementLod> lods;
  // Static prop world cache (#260): props keep their world centre and
  // instance matrix for this shell matrix; recomputed only when it changes.
  PartMatrix cached_shell{};
  bool world_cache_valid = false;
  // Persistent batch layout (#260): one slot per batchable mesh of the map.
  std::vector<uint32_t> batch_ids;
  std::vector<std::shared_ptr<const ScxGeometry>> batch_geometry;
  // The same for the night-only meshes, instanced as night overlays (#264).
  std::vector<uint32_t> night_ids;
  std::vector<std::shared_ptr<const ScxGeometry>> night_geometry;
};
ScenePropLodView g_lod_view;
// Bumped by every updating LOD view (one per main-view collection), so each
// placement's level is computed once per frame and reused by both of its
// chain members and by both collection paths (#257).
uint64_t g_lod_epoch = 1;
std::unordered_map<uint32_t, GroundPropMap> g_ground_props;
// The map is cached per render id and outlives its GroundRef: each visit to a
// city builds a new GroundRef, so the one to parent a promoted prop to is
// taken from the shell that woke it, never stored with the placement.
struct PendingDormant {
  GroundPropMap* map = nullptr;
  uint32_t index = 0;
  void* ground = nullptr;
  PartMatrix shell{};
};
std::vector<PendingDormant> g_pending_dormant;
std::unordered_map<void*, std::pair<GroundPropMap*, uint32_t>> g_live_dormant;

struct GroundCollisionWorld {
  PartMatrix matrix{};
  PartMatrix inverse{};
  const GroundPropMap* source = nullptr;
  std::vector<StaticMesh> meshes;
  std::array<float, 3> bounds_min{};
  std::array<float, 3> bounds_max{};
  bool has_bounds = false;
};
std::unordered_map<void*, GroundCollisionWorld> g_ground_collision;
collision::StaticIndex g_script_collision_shapes;

struct PrimitiveSource {
  PhysicsShape shape = PhysicsShape::None;
  std::array<float, 3> size{};
  std::shared_ptr<const ScxGeometry> geometry;
};
std::unordered_map<void*, PrimitiveSource> g_primitive_sources;

struct NativeCollisionData {
  bool dynamic = false;
  // Sum and centre of the positive-mass collision bodies, object frame.
  float mass = 0.f;
  std::array<float, 3> center{};
  // SCX bodies baked into the native object's local frame, in metres.
  std::shared_ptr<const ScxGeometry> mesh;
  // Primitive bodies keep their local poses until each placement is known.
  std::vector<PartBody> primitives;
};
std::unordered_map<uint32_t, NativeCollisionData> g_native_collision;
bool ground_prop_config(const RpkIndex& rpk, uint32_t type, PartConfig& out);
const NativeCollisionData& native_collision_data(
    const RpkIndex& rpk, uint32_t type, const PartConfig* parsed = nullptr);
constexpr float kPropCellSize = 64.f;
constexpr float kDormantCell = 16.f;

int32_t prop_cell(float value) {
  return static_cast<int32_t>(std::floor(value / kPropCellSize));
}
int32_t cell_coordinate(double position, const StaticMesh& mesh) {
  return static_cast<int32_t>(std::floor(position / mesh.cell_size));
}

struct FloorMesh {
  std::shared_ptr<const ScxGeometry> source;
  std::array<float, 16> matrix{};
  StaticMesh mesh;
  std::vector<void *> ancestors;
};
std::unordered_map<void *, FloorMesh> g_floor_meshes;
bool g_floor_prepared = false;
uint64_t g_floor_builds = 0;
uint64_t g_floor_revision = 1;
using FloorCells = std::unordered_map<uint64_t, std::vector<uint32_t>>;
using SurfaceCells = std::unordered_map<uint64_t, std::vector<uint16_t>>;
// Indexes triangles [first, last) of the mesh into cells and large.
// With surfaces, surface_cells and large_surfaces get each listed
// triangle's surface id in the same order.
void index_triangles(const StaticMesh& mesh, size_t first, size_t last, FloorCells& cells,
                     std::vector<uint32_t>& large, SurfaceCells& surface_cells,
                     std::vector<uint16_t>& large_surfaces) {
  const bool surfaces = !mesh.triangle_surfaces.empty();
  for (size_t i = first * 3; i + 2 < mesh.indices.size() && i < last * 3; i += 3) {
    float min_x = 0, max_x = 0, min_z = 0, max_z = 0;
    bool valid = true;
    for (int k = 0; k < 3; ++k) {
      const auto vertex = static_cast<size_t>(mesh.indices[i + k]) * 3;
      if (vertex + 2 >= mesh.positions.size()) {
        valid = false;
        break;
      }
      float x = mesh.positions[vertex] * mesh.scale,
            z = mesh.positions[vertex + 2] * mesh.scale;
      if (!std::isfinite(x) || !std::isfinite(z)) {
        valid = false;
        break;
      }
      if (!k) {
        min_x = max_x = x;
        min_z = max_z = z;
      } else {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_z = std::min(min_z, z);
        max_z = std::max(max_z, z);
      }
    }
    if (!valid)
      continue;
    const uint16_t surface = surfaces && i / 3 < mesh.triangle_surfaces.size()
                                 ? mesh.triangle_surfaces[i / 3] : uint16_t(0xffff);
    const int32_t x0 = cell_coordinate(min_x, mesh), x1 = cell_coordinate(max_x, mesh);
    const int32_t z0 = cell_coordinate(min_z, mesh), z1 = cell_coordinate(max_z, mesh);
    if ((int64_t(x1) - x0 + 1) * (int64_t(z1) - z0 + 1) > 4096) {
      large.insert(large.end(), mesh.indices.begin() + i, mesh.indices.begin() + i + 3);
      if (surfaces) large_surfaces.push_back(surface);
      continue;
    }
    for (int32_t x = x0; x <= x1; ++x)
      for (int32_t z = z0; z <= z1; ++z) {
        const uint64_t key = floor_cell(x, z);
        auto &cell = cells[key];
        cell.insert(cell.end(), mesh.indices.begin() + i,
                    mesh.indices.begin() + i + 3);
        if (surfaces) surface_cells[key].push_back(surface);
      }
  }
}

// From this size a mesh is indexed on the job pool (a map's merged
// collision has hundreds of thousands of triangles).
constexpr size_t kPoolFloorTriangles = 32768;

bool pool_floor_index(const StaticMesh& mesh) {
  return mesh.indices.size() / 3 >= kPoolFloorTriangles;
}

void index_floor(StaticMesh &mesh) {
  // Static garage triangles are indexed once in script space. Physics asks
  // for several support samples per 120 Hz substep; rescanning the whole shell
  // for every wheel/corner is unnecessary.
  const size_t triangles = mesh.indices.size() / 3;
  const size_t workers = pool_floor_index(mesh) && !jobs_inside() ? jobs_concurrency() : 1;
  if (workers < 2) {
    index_triangles(mesh, 0, triangles, mesh.cells, mesh.large_triangles, mesh.surface_cells,
                    mesh.large_surfaces);
    return;
  }
  // Each job indexes a contiguous run of triangles. Appending the runs in
  // order gives every cell the triangle order of a serial pass.
  struct Run {
    FloorCells cells;
    std::vector<uint32_t> large;
    SurfaceCells surface_cells;
    std::vector<uint16_t> large_surfaces;
  };
  std::vector<Run> runs(std::min(workers * 2, triangles / 4096 + 1));
  jobs_parallel_for(runs.size(), [&](size_t r) {
    index_triangles(mesh, triangles * r / runs.size(), triangles * (r + 1) / runs.size(),
                    runs[r].cells, runs[r].large, runs[r].surface_cells, runs[r].large_surfaces);
  });
  for (auto& run : runs) {
    for (auto& [key, list] : run.surface_cells) {
      auto& cell = mesh.surface_cells[key];
      cell.insert(cell.end(), list.begin(), list.end());
    }
    mesh.large_surfaces.insert(mesh.large_surfaces.end(), run.large_surfaces.begin(),
                               run.large_surfaces.end());
    for (auto& [key, list] : run.cells) {
      auto& cell = mesh.cells[key];
      if (cell.empty())
        cell = std::move(list);
      else
        cell.insert(cell.end(), list.begin(), list.end());
    }
    mesh.large_triangles.insert(mesh.large_triangles.end(), run.large.begin(), run.large.end());
  }
}

// index = false leaves the cells empty, for a caller that indexes later.
StaticMesh bake_static_mesh(const ScxGeometry& geo, const PartMatrix& matrix,
                            bool index = true) {
  StaticMesh mesh;
  mesh.scale = 1.f;
  if (geo.indices.size() > 100000) mesh.cell_size = 8.f;
  mesh.indices = geo.indices;
  mesh.triangle_surfaces = geo.triangle_surfaces;
  mesh.surfaces = geo.surfaces;
  mesh.positions.resize(geo.positions.size());
  for (size_t i = 0; i + 2 < geo.positions.size(); i += 3) {
    const float x = geo.positions[i] * geo.position_scale;
    const float y = geo.positions[i + 1] * geo.position_scale;
    const float z = geo.positions[i + 2] * geo.position_scale;
    mesh.positions[i] = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
    mesh.positions[i + 1] = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
    mesh.positions[i + 2] = matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14];
  }
  if (index) index_floor(mesh);
  return mesh;
}

StaticMesh transform_static_mesh(const StaticMesh& source, const PartMatrix& matrix,
                                 bool index = true) {
  StaticMesh mesh;
  mesh.scale = 1.f;
  mesh.cell_size = source.cell_size;
  mesh.indices = source.indices;
  mesh.triangle_surfaces = source.triangle_surfaces;
  mesh.surfaces = source.surfaces;
  mesh.positions.resize(source.positions.size());
  for (size_t i = 0; i + 2 < source.positions.size(); i += 3) {
    const float x = source.positions[i] * source.scale;
    const float y = source.positions[i + 1] * source.scale;
    const float z = source.positions[i + 2] * source.scale;
    mesh.positions[i] = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
    mesh.positions[i + 1] = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
    mesh.positions[i + 2] = matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14];
  }
  if (index) index_floor(mesh);
  return mesh;
}

void merge_static_meshes(std::vector<StaticMesh>& meshes) {
  if (meshes.size() < 2) return;
  StaticMesh combined;
  combined.scale = 1.f;
  // Size the buffers once (growing them by one mesh at a time copied the
  // whole buffer per placement: 12.5 s of a Valo City load), then copy each
  // mesh to its own range on the job pool.
  std::vector<size_t> position_at(meshes.size() + 1), index_at(meshes.size() + 1);
  for (size_t m = 0; m < meshes.size(); ++m) {
    position_at[m + 1] = position_at[m] + meshes[m].positions.size();
    index_at[m + 1] = index_at[m] + meshes[m].indices.size();
  }
  combined.positions.resize(position_at.back());
  combined.indices.resize(index_at.back());
  jobs_parallel_for(meshes.size(), [&](size_t m) {
    const auto& source = meshes[m];
    const auto first = static_cast<uint32_t>(position_at[m] / 3);
    std::transform(source.positions.begin(), source.positions.end(),
                   combined.positions.begin() + static_cast<std::ptrdiff_t>(position_at[m]),
                   [scale = source.scale](float value) { return value * scale; });
    std::transform(source.indices.begin(), source.indices.end(),
                   combined.indices.begin() + static_cast<std::ptrdiff_t>(index_at[m]),
                   [first](uint32_t index) { return first + index; });
  });
  // Surfaces follow their triangles, each table appended after the last.
  if (std::any_of(meshes.begin(), meshes.end(),
                  [](const StaticMesh& m) { return !m.triangle_surfaces.empty(); })) {
    combined.triangle_surfaces.assign(combined.indices.size() / 3, 0xffff);
    for (size_t m = 0; m < meshes.size(); ++m) {
      const auto& source = meshes[m];
      const size_t base = combined.surfaces.size();
      combined.surfaces.insert(combined.surfaces.end(), source.surfaces.begin(),
                               source.surfaces.end());
      for (size_t t = 0; t < source.triangle_surfaces.size() && t < source.indices.size() / 3; ++t)
        if (source.triangle_surfaces[t] < source.surfaces.size() && base + source.triangle_surfaces[t] < 0xffff)
          combined.triangle_surfaces[index_at[m] / 3 + t] =
              static_cast<uint16_t>(base + source.triangle_surfaces[t]);
    }
  }
  if (combined.indices.size() > 100000) combined.cell_size = 8.f;
  index_floor(combined);
  meshes.clear();
  meshes.push_back(std::move(combined));
}

std::shared_ptr<const ScxGeometry> primitive_geometry(GameRefState& node) {
  const std::array<float, 3> size = {node.physics_size[0], node.physics_size[1],
                                     node.physics_size[2]};
  auto& cached = g_primitive_sources[node.key];
  if (cached.geometry && cached.shape == node.physics_shape && cached.size == size)
    return cached.geometry;
  PartMatrix frame{};
  frame[0] = frame[5] = frame[10] = frame[15] = 1.f;
  std::shared_ptr<ScxGeometry> geometry;
  if (node.physics_shape == PhysicsShape::Box)
    geometry = std::make_shared<ScxGeometry>(
        collision::make_box_mesh(frame, {size[0], size[1], size[2]}));
  else if (node.physics_shape == PhysicsShape::Sphere)
    geometry = std::make_shared<ScxGeometry>(
        collision::make_sphere_mesh(frame, size[0], 16, 8));
  if (geometry && !geometry->info.ok) geometry.reset();
  cached.shape = node.physics_shape;
  cached.size = size;
  cached.geometry = geometry;
  return cached.geometry;
}

void identity(float *m) {
  std::fill(m, m + 16, 0.f);
  m[0] = m[5] = m[10] = m[15] = 1.f;
}
void multiply(float *out, const float *a, const float *b) {
  float result[16]{};
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row)
      for (int k = 0; k < 4; ++k)
        result[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
  std::copy(result, result + 16, out);
}
void local_matrix(const GameRefState &n, float *m) {
  float yaw[16], pitch[16], roll[16];
  identity(yaw);
  identity(pitch);
  identity(roll);
  const float cy = std::cos(n.ypr[0]), sy = std::sin(n.ypr[0]);
  const float cp = std::cos(n.ypr[1]), sp = std::sin(n.ypr[1]);
  const float cr = std::cos(n.ypr[2]), sr = std::sin(n.ypr[2]);
  yaw[0] = cy;
  yaw[2] = -sy;
  yaw[8] = sy;
  yaw[10] = cy;
  pitch[5] = cp;
  pitch[6] = sp;
  pitch[9] = -sp;
  pitch[10] = cp;
  roll[0] = cr;
  roll[1] = sr;
  roll[4] = -sr;
  roll[5] = cr;
  multiply(m, yaw, pitch);
  multiply(m, m, roll);
  m[12] = n.pos[0];
  m[13] = n.pos[1];
  m[14] = n.pos[2];
}

std::shared_ptr<const ScxGeometry> load_geometry(const RpkIndex& resources, uint32_t id) {
  std::shared_ptr<const ScxGeometry> result;
  const int pack = RpkIndex::pack_from_full(id);
  auto model = resources.model_for_resource(pack, id & 0xffffu);
  if (model.ok) {
    std::string source = model.source;
    std::replace(source.begin(), source.end(), '\\', '/');
    std::filesystem::path path =
        std::filesystem::path(resources.game_root()) / source;
    std::string ext = path.extension().string();
    for (char &c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".scy") {
      auto scx = path;
      scx.replace_extension(".scx");
      std::error_code error;
      if (std::filesystem::exists(scx, error))
        path = scx;
    }
    ext = path.extension().string();
    for (char &c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".scx" || ext == ".scy") {
      auto mesh =
          std::make_shared<ScxGeometry>(load_scx_geometry(path.string()));
      if (mesh->info.ok && !mesh->indices.empty()) {
        for (auto &texture : model.textures) {
          std::replace(texture.begin(), texture.end(), '\\', '/');
          if (!texture.empty() && std::filesystem::path(texture).is_relative())
            texture =
                (std::filesystem::path(resources.game_root()) / texture)
                    .string();
        }
        mesh->textures = std::move(model.textures);
        scx_resolve_baked_lightmaps(*mesh);
        adapt_legacy_neon_fixtures(*mesh);
        result = std::move(mesh);
        jobs_printf("[scene] resource=0x%x source=%s vertices=%d\n", id,
                    path.string().c_str(), result->info.vertices);
      }
    }
  }
  return result;
}

std::shared_ptr<const ScxGeometry> geometry(uint32_t id) {
  if (auto over = g_overrides.find(id); over != g_overrides.end()) return over->second;
  if (auto found = g_meshes.find(id); found != g_meshes.end()) return found->second;
  auto mesh = load_geometry(*g_resources, id);
  g_meshes.emplace(id, mesh);
  return mesh;
}

void preload_meshes(const std::vector<uint32_t>& resources) {
  std::vector<uint32_t> missing;
  for (const auto id : resources)
    if (!g_meshes.contains(id) && !g_overrides.contains(id)) missing.push_back(id);
  if (missing.empty()) return;
  std::vector<std::shared_ptr<const ScxGeometry>> decoded(missing.size());
  const auto& index = *g_resources;
  // Workers only read assets. Publish the completed cache on the host thread,
  // while the loading screen is still up.
  jobs_parallel_for(missing.size(),
                    [&](size_t job) { decoded[job] = load_geometry(index, missing[job]); });
  for (size_t i = 0; i < missing.size(); ++i)
    g_meshes.emplace(missing[i], std::move(decoded[i]));
}

std::array<float, 3> transform_point(const PartMatrix& matrix, const std::array<float, 3>& p) {
  return {matrix[0] * p[0] + matrix[4] * p[1] + matrix[8] * p[2] + matrix[12],
          matrix[1] * p[0] + matrix[5] * p[1] + matrix[9] * p[2] + matrix[13],
          matrix[2] * p[0] + matrix[6] * p[1] + matrix[10] * p[2] + matrix[14]};
}

uint32_t rd32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
float rdf(const uint8_t *p) {
  float f;
  std::memcpy(&f, p, 4);
  return f;
}

struct PolySection {
  uint32_t material = 0;
  std::vector<float> pos, nrm, uv0, uv1, uv2;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> colors;
};

struct GroundWalk {
  std::unordered_set<uint32_t> walked_offsets;
  const std::vector<uint8_t> *file = nullptr;
  std::string params;
  // The whole value: render id, six numbers, the SCM path, the TRC path.
  std::string params_line;
  std::vector<PolySection> poly;
  std::vector<uint32_t> exterior_slots;
  ScxGeometry collision;
  std::vector<GroundMapData::Placement> placements;
};

void append_ground_placement(GroundWalk& walk, std::string_view text) {
  constexpr std::string_view type_prefix = "gametype 0x";
  if (!text.starts_with(type_prefix)) return;
  text.remove_prefix(type_prefix.size());
  GroundMapData::Placement placement;
  const auto end = text.data() + text.size();
  const auto type = std::from_chars(text.data(), end, placement.type, 16);
  if (type.ec != std::errc{}) return;
  text.remove_prefix(type.ptr - text.data());
  const auto params = text.find_first_not_of(std::string_view("\r\n\0", 3));
  if (params == std::string_view::npos) {
    // No params line: the native sits at the map origin. Track Wizard places
    // a track's render-only meshes this way (`gametype 0x...` alone).
    walk.placements.push_back(placement);
    return;
  }
  text.remove_prefix(params);
  constexpr std::string_view params_prefix = "params ";
  if (!text.starts_with(params_prefix)) return;
  text.remove_prefix(params_prefix.size());
  for (size_t i = 0; i < 6; ++i) {
    const auto start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos) return;
    text.remove_prefix(start);
    float& value = i < 3 ? placement.position[i] : placement.rotation[i - 3];
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || !std::isfinite(value)) return;
    text.remove_prefix(parsed.ptr - text.data());
    if (i < 5) {
      const auto comma = text.find_first_not_of(" \t");
      if (comma == std::string_view::npos || text[comma] != ',') return;
      text.remove_prefix(comma + 1);
    }
  }
  const auto tail = text.find_first_not_of(" \t");
  text.remove_prefix(tail == std::string_view::npos ? text.size() : tail);
  if (text.starts_with(',')) {
    text.remove_prefix(1);
    const auto start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos) return;
    text.remove_prefix(start);
    const auto parsed = std::from_chars(text.data(), end, placement.seed);
    if (parsed.ec != std::errc{}) return;
    text.remove_prefix(parsed.ptr - text.data());
  }
  if (text.empty() || text.front() == '\r' || text.front() == '\n' || text.front() == '\0')
    walk.placements.push_back(placement);
}

void append_ground_collision(ScxGeometry& mesh, const uint8_t* data, size_t size) {
  if (size < 8) return;
  const uint32_t faces = rd32(data);
  if (faces > (size - 8) / 28) return;
  const size_t vertices_offset = 4 + static_cast<size_t>(faces) * 28;
  const uint32_t vertices = rd32(data + vertices_offset);
  if (vertices > (size - vertices_offset - 4) / 12) return;
  const uint32_t base = static_cast<uint32_t>(mesh.positions.size() / 3);
  const uint8_t* positions = data + vertices_offset + 4;
  for (size_t i = 0; i < static_cast<size_t>(vertices) * 3; ++i)
    mesh.positions.push_back(rdf(positions + i * 4));
  // A face: its SCM material index, three vertex indices and its normal
  // (#452). The material index is resolved once the SCM is read.
  for (uint32_t i = 0; i < faces; ++i) {
    const uint8_t* face = data + 4 + static_cast<size_t>(i) * 28;
    const uint32_t a = rd32(face + 4), b = rd32(face + 8), c = rd32(face + 12);
    if (a < vertices && b < vertices && c < vertices) {
      mesh.indices.insert(mesh.indices.end(), {base + a, base + b, base + c});
      const uint32_t material = rd32(face);
      mesh.triangle_surfaces.push_back(material < 0xffff ? static_cast<uint16_t>(material)
                                                         : uint16_t(0xffff));
    }
  }
}

void take_params(GroundWalk &w, const uint8_t *data, const uint8_t *end) {
  if (!w.params.empty() || !data)
    return;
  const uint8_t *s = data;
  while (s < end && *s != 0) {
    const uint8_t *line = s;
    while (s < end && *s != '\n' && *s != '\r' && *s != 0)
      ++s;
    std::string text(reinterpret_cast<const char *>(line),
                     static_cast<size_t>(s - line));
    while (s < end && (*s == '\n' || *s == '\r'))
      ++s;
    size_t a = 0;
    while (a < text.size() && (text[a] == ' ' || text[a] == '\t'))
      ++a;
    if (text.size() < a + 6)
      continue;
    std::string key = text.substr(a, 6);
    for (char &c : key)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key != "params")
      continue;
    if (a + 6 < text.size() && text[a + 6] != ' ' && text[a + 6] != '\t')
      continue;
    size_t v = a + 6;
    while (v < text.size() && (text[v] == ' ' || text[v] == '\t'))
      ++v;
    size_t e = v;
    while (e < text.size() && text[e] != ' ' && text[e] != '\t' &&
           text[e] != ',')
      ++e;
    if (e > v) {
      w.params = text.substr(v, e - v);
      w.params_line = text.substr(v);
    }
  }
}

void walk_ground_node(GroundWalk &w, uint32_t offset, uint32_t size,
                      int depth, bool placement = false) {
  const std::vector<uint8_t> &file = *w.file;
  if (size < 8 || offset > file.size() || size > file.size() - offset)
    return;
  if (!w.walked_offsets.insert(offset).second) return;
  const uint8_t *base = file.data() + offset;
  const uint8_t *end = base + size;
  const uint32_t n0 = rd32(base);
  if (n0 > (size - 8) / 4)
    return;
  const uint8_t *p = base + 4 + 4 * static_cast<size_t>(n0);
  if (end - p < 4)
    return;
  const uint32_t child_count = rd32(p);
  p += 4;
  struct Kid {
    uint8_t restype;
    uint32_t off, sz;
    bool placement;
  };
  std::vector<Kid> kids;
  bool ok = true;
  for (uint32_t i = 0; i < child_count; ++i) {
    if (end - p < 0x17) {
      ok = false;
      break;
    }
    const uint8_t restype = p[8];
    const uint8_t flags = p[9];
    const uint32_t foff = rd32(p + 0xE);
    const uint32_t sz = rd32(p + 0x12);
    const size_t name_len = p[0x16];
    size_t rec = 0x17 + name_len;
    if (static_cast<size_t>(end - p) < rec) {
      ok = false;
      break;
    }
    if (flags & 1u) {
      if (static_cast<size_t>(end - p) < rec + 48) {
        ok = false;
        break;
      }
      rec += 48;
    }
    p += rec;
    kids.push_back(Kid{restype, foff, sz, (flags & 0x20u) != 0});
  }
  while (ok && end - p >= 8) {
    const uint32_t tag = rd32(p);
    const uint32_t csize = rd32(p + 4);
    const uint8_t *data = p + 8;
    if (tag == 0 && csize == 0) {
      if (end - p < 12)
        break;
      take_params(w, p + 12, end);
      break;
    }
    if (csize > static_cast<size_t>(end - data))
      break;
    if (tag == 0x00445352u) {
      if (!placement)
        take_params(w, data, data + csize);
      else
        append_ground_placement(w, {reinterpret_cast<const char*>(data), csize});
    } else if (tag == 0x50545845u && depth == 0) { // EXTP
      for (size_t i = 0; i + 4 <= csize; i += 4)
        w.exterior_slots.push_back(rd32(data + i));
    } else if (tag == 0x53594850u) { // PHYS
      append_ground_collision(w.collision, data, csize);
    } else if (tag == 0x594C4F50u && csize >= 8) {
      const uint32_t nsec = rd32(data + 4);
      const uint8_t *q = data + 8;
      const uint8_t *cend = data + csize;
      for (uint32_t s = 0; s < nsec; ++s) {
        if (cend - q < 12)
          break;
        const uint32_t mat = rd32(q);
        const uint32_t nv = rd32(q + 4);
        const uint32_t nt = rd32(q + 8);
        q += 12;
        if (nv > 200000u || nt > 200000u)
          break;
        const uint64_t need =
            static_cast<uint64_t>(nv) * 56u + static_cast<uint64_t>(nt) * 12u;
        if (need > static_cast<uint64_t>(cend - q))
          break;
        PolySection sec;
        sec.material = mat;
        sec.pos.resize(static_cast<size_t>(nv) * 3u);
        sec.nrm.resize(sec.pos.size());
        sec.colors.resize(nv);
        sec.uv0.resize(static_cast<size_t>(nv) * 2u);
        sec.uv1.resize(sec.uv0.size());
        sec.uv2.resize(sec.uv0.size());
        for (uint32_t vi = 0; vi < nv; ++vi) {
          const uint8_t *v = q + static_cast<size_t>(vi) * 56u;
          sec.pos[vi * 3] = rdf(v);
          sec.pos[vi * 3 + 1] = rdf(v + 4);
          sec.pos[vi * 3 + 2] = rdf(v + 8);
          sec.nrm[vi * 3] = rdf(v + 12);
          sec.nrm[vi * 3 + 1] = rdf(v + 16);
          sec.nrm[vi * 3 + 2] = rdf(v + 20);
          sec.colors[vi] = rd32(v + 24);
          sec.uv0[vi * 2] = rdf(v + 32);
          sec.uv0[vi * 2 + 1] = rdf(v + 36);
          sec.uv1[vi * 2] = rdf(v + 40);
          sec.uv1[vi * 2 + 1] = rdf(v + 44);
          sec.uv2[vi * 2] = rdf(v + 48);
          sec.uv2[vi * 2 + 1] = rdf(v + 52);
        }
        q += static_cast<size_t>(nv) * 56u;
        bool oob = false;
        sec.indices.resize(static_cast<size_t>(nt) * 3u);
        for (uint32_t ii = 0; ii < nt * 3u; ++ii) {
          sec.indices[ii] = rd32(q + static_cast<size_t>(ii) * 4u);
          if (sec.indices[ii] >= nv)
            oob = true;
        }
        q += static_cast<size_t>(nt) * 12u;
        if (!oob && nv && nt)
          w.poly.push_back(std::move(sec));
      }
    }
    p = data + csize;
  }
  if (depth >= 64)
    return;
  for (const Kid &k : kids)
    if (k.restype == 1)
      walk_ground_node(w, k.off, k.sz, depth + 1, k.placement);
}

struct GroundMat {
  uint16_t tex = 0xffff;
  uint16_t mix = 0xffff;
  int16_t lm = -1;
  uint16_t tex_channel = 0, mix_channel = 0;
  uint32_t flags = 0;
};

std::vector<GroundMat>
read_ground_materials(const std::filesystem::path &path) {
  std::vector<GroundMat> mats;
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    return mats;
  const auto sz = static_cast<size_t>(in.tellg());
  if (sz < 16)
    return mats;
  in.seekg(0);
  std::vector<uint8_t> buf(sz);
  in.read(reinterpret_cast<char *>(buf.data()),
          static_cast<std::streamsize>(sz));
  if (!in || std::memcmp(buf.data(), "INVO", 4) != 0 ||
      rd32(buf.data() + 4) != 3)
    return {};
  size_t p = 8;
  while (p + 4 <= buf.size() && mats.size() < 4096) {
    const uint32_t hsz = rd32(buf.data() + p);
    if (hsz == 0)
      break;
    if (hsz < 46 || p + hsz + 8 > buf.size())
      break;
    // A material table steps over a zero vertex count and a zero face count.
    // A real mesh has geometry there; don't treat it as the table.
    if (rd32(buf.data() + p + hsz) != 0 || rd32(buf.data() + p + hsz + 4) != 0)
      return {};
    GroundMat m;
    m.tex = static_cast<uint16_t>(buf[p + 44] | (buf[p + 45] << 8));
    if (hsz > 0x39) {
      m.lm = static_cast<int16_t>(buf[p + 0x38] | (buf[p + 0x39] << 8));
    }
    m.flags = rd32(buf.data() + p + 40);
    if ((m.flags & 0x40u) && hsz >= 54)
      m.mix = static_cast<uint16_t>(buf[p + 52] | (buf[p + 53] << 8));
    if (hsz >= 74) {
      m.tex_channel = static_cast<uint16_t>(buf[p + 70] | (buf[p + 71] << 8));
      m.mix_channel = static_cast<uint16_t>(buf[p + 72] | (buf[p + 73] << 8));
    }
    mats.push_back(m);
    p += static_cast<size_t>(hsz) + 8;
  }
  return mats;
}
} // namespace

void scene_instances_preload(const std::vector<uint32_t>& resources) {
  if (!g_resources || resources.empty()) return;
  preload_meshes(resources);
}

void scene_instances_set_resources(const RpkIndex *resources) {
  part_physics_join();  // the collision caches below are read by a running batch
  character_forget_all();
  g_resources = resources;
  g_world = g_sky = nullptr;
  g_preview_lamps = false;
  g_environment_kind = SceneEnvironment::Automatic;
  g_meshes.clear();
  g_shadow_info.clear();
  g_overrides.clear();
  g_collision_overrides.clear();
  g_ground_props.clear();
  scene_ambience_reset();
  g_pending_dormant.clear();
  g_live_dormant.clear();
  g_ground_collision.clear();
  g_native_collision.clear();
  g_script_collision_shapes.clear();
  g_primitive_sources.clear();
  g_texture_paths.clear();
  g_static.clear();
  g_floor_meshes.clear();
  g_floor_prepared = false;
  g_floor_builds = 0;
  ++g_floor_revision;
  g_static_enabled = true;
  g_environment = 0;
}
void scene_instances_set_envmap(uint32_t resource) { g_environment = resource; }
std::shared_ptr<const ScxGeometry> scene_instances_geometry(uint32_t resource) {
  return geometry(resource);
}
std::string scene_instances_daylight_flare_texture() {
  if (!g_resources) return {};
  const auto* pack = g_resources->pack_by_path("maps/skydome.rpk");
  return pack ? scene_instances_texture(RpkIndex::make_full_id(pack->pack_id, 0x100))
              : std::string{};
}

std::string scene_instances_texture(uint32_t resource) {
  if (!resource) return {};
  // Painter textures live in host memory; the renderer uploads them by id.
  if (paint_texture_exists(resource)) return paint_texture_path(resource);
  // ResourceRef.makeTexture/Decal names a file by a live native handle.
  if (const auto* live = gameref_find_live(static_cast<int32_t>(resource))) {
    const auto* owner = static_cast<Object*>(live->key);
    const auto path = owner->fields.find("_texpath");
    const auto type = owner->fields.find("typeof");
    if (type != owner->fields.end() && type->second.tag == ValueTag::Int && type->second.v.i == 7 &&
        path != owner->fields.end() && path->second.tag == ValueTag::Obj && path->second.v.o)
      return path->second.v.o->str;
  }
  if (!g_resources) return {};
  if (const auto it = g_texture_paths.find(resource); it != g_texture_paths.end())
    return it->second;
  auto path = g_resources->texture_path_for_id(resource);
  if (!path.empty()) g_texture_paths.emplace(resource, path);
  return path;
}
std::string scene_instances_envmap() {
  return g_resources ? g_resources->texture_path_for_id(g_environment)
                     : std::string{};
}
void scene_instances_select(void *world, void *skydome, bool preview_lamps,
                            SceneEnvironment environment) {
  if (auto *sky = gameref_get(skydome))
    sky->sky_render = true;
  // Called every frame. The free-running physics thread reads the collision
  // caches, so join it only for a real change (#380): an unconditional join
  // paused the thread for the rest of every frame. The physics sync prepares
  // the caches each frame anyway.
  if (g_world == world && g_sky == skydome && g_preview_lamps == preview_lamps &&
      g_environment_kind == environment)
    return;
  part_physics_join();
  if (g_world != world) ++g_floor_revision;
  g_world = world;
  g_sky = skydome;
  g_preview_lamps = preview_lamps;
  g_environment_kind = environment;
  g_floor_prepared = false;
}

bool scene_instances_outdoor() {
  if (!g_world || g_preview_lamps) return false;
  if (g_environment_kind == SceneEnvironment::Interior) return false;
  if (g_environment_kind == SceneEnvironment::Exterior) return true;
  return scene_instances_has_ground_props();
}

bool chain_matrix(void *key, void *root, float *matrix, bool reflect,
                  bool drawn = false) {
  if (!key || !root || !matrix)
    return false;
  identity(matrix);
  // A parent cycle also ends here, after 128 links. This runs for every
  // render node several times per frame, so it does not allocate.
  size_t depth = 0;
  for (void *current = key; current;) {
    if (++depth > 128)
      return false;
    auto *n = gameref_get(current);
    if (!n || n->empty || (n->flags & kWorldTreeLeaf))
      return false;
    // Inventory items are independent worlds even though their roots have
    // an owning inventory parent. Never leak their meshes into the garage.
    if (current != root && (n->flags & kWorldTreeRoot))
      return false;
    float local[16];
    if (const float *pose = drawn ? gameref_render_matrix(*n) : nullptr)
      std::copy(pose, pose + 16, local);
    else if (n->matrix_override)
      std::copy(n->local_matrix, n->local_matrix + 16, local);
    else
      local_matrix(*n, local);
    void* bone_owner = nullptr;
    int bone_id = 0;
    if (character_linked_bone(current, &bone_owner, &bone_id) && bone_owner == n->parent) {
      float bone[16];
      if (character_bone_metres(bone_owner, bone_id, bone)) {
        float placed[16];
        multiply(placed, bone, local);
        std::copy(placed, placed + 16, local);
      }
    }
    multiply(matrix, local, matrix);
    if (current == root) {
      // Vertices are uploaded as S*v (S reflects Z). Instance matrix is S*M*S.
      if (reflect)
        for (int col = 0; col < 4; ++col)
          for (int row = 0; row < 4; ++row)
            if ((col == 2) != (row == 2))
              matrix[col * 4 + row] *= -1.f;
      return true;
    }
    current = n->parent;
  }
  return false;
}
bool scene_instance_transform(void *key, void *root, float *matrix) {
  return chain_matrix(key, root, matrix, true, true);
}
bool scene_instances_script_matrix(void *key, float *matrix) {
  return g_world && chain_matrix(key, g_world, matrix, false);
}
bool scene_instances_render_matrix(void *key, float *matrix) {
  return g_world && chain_matrix(key, g_world, matrix, false, true);
}

namespace {
// Shell-local bounds of a dormant placement at its current pose, padded so a
// car is found a little before its box touches the collision mesh.
void dormant_bounds(DormantProp& entry) {
  entry.lower.fill(std::numeric_limits<float>::infinity());
  entry.upper.fill(-std::numeric_limits<float>::infinity());
  for (int corner = 0; corner < 8; ++corner) {
    const auto world = transform_point(
        entry.pose, {corner & 1 ? entry.local_upper[0] : entry.local_lower[0],
                     corner & 2 ? entry.local_upper[1] : entry.local_lower[1],
                     corner & 4 ? entry.local_upper[2] : entry.local_lower[2]});
    for (int axis = 0; axis < 3; ++axis) {
      entry.lower[axis] = std::min(entry.lower[axis], world[axis] - .5f);
      entry.upper[axis] = std::max(entry.upper[axis], world[axis] + .5f);
    }
  }
}

template <typename Visit>
void dormant_cells(const DormantProp& entry, Visit&& visit) {
  const auto x0 = static_cast<int32_t>(std::floor(entry.lower[0] / kDormantCell));
  const auto x1 = static_cast<int32_t>(std::floor(entry.upper[0] / kDormantCell));
  const auto z0 = static_cast<int32_t>(std::floor(entry.lower[2] / kDormantCell));
  const auto z1 = static_cast<int32_t>(std::floor(entry.upper[2] / kDormantCell));
  for (int32_t x = x0; x <= x1; ++x)
    for (int32_t z = z0; z <= z1; ++z) visit(floor_cell(x, z));
}

void show_dormant(DormantProp& entry, bool shown) {
  for (const auto& [cell, prop] : entry.props)
    if (prop < cell->props.size()) cell->props[prop].hidden = !shown;
}
}  // namespace

void scene_instances_wake_dynamics(const float lower[3], const float upper[3]) {
  if (!lower || !upper) return;
  for (auto& [key, ground] : g_ground_collision) {
    auto* map = const_cast<GroundPropMap*>(ground.source);
    if (!map || map->dormant.empty()) continue;
    std::array<float, 3> lo, hi;
    lo.fill(std::numeric_limits<float>::infinity());
    hi.fill(-std::numeric_limits<float>::infinity());
    for (int corner = 0; corner < 8; ++corner) {
      const auto local = transform_point(
          ground.inverse, {corner & 1 ? upper[0] : lower[0], corner & 2 ? upper[1] : lower[1],
                           corner & 4 ? upper[2] : lower[2]});
      for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = std::min(lo[axis], local[axis]);
        hi[axis] = std::max(hi[axis], local[axis]);
      }
    }
    const auto x0 = static_cast<int32_t>(std::floor(lo[0] / kDormantCell));
    const auto x1 = static_cast<int32_t>(std::floor(hi[0] / kDormantCell));
    const auto z0 = static_cast<int32_t>(std::floor(lo[2] / kDormantCell));
    const auto z1 = static_cast<int32_t>(std::floor(hi[2] / kDormantCell));
    if (static_cast<int64_t>(x1 - x0 + 1) * (z1 - z0 + 1) > 256) continue;
    for (int32_t x = x0; x <= x1; ++x)
      for (int32_t z = z0; z <= z1; ++z) {
        const auto cell = map->dormant_grid.find(floor_cell(x, z));
        if (cell == map->dormant_grid.end()) continue;
        for (const auto index : cell->second) {
          auto& entry = map->dormant[index];
          if (entry.promoted || entry.lower[0] > hi[0] || entry.upper[0] < lo[0] ||
              entry.lower[1] > hi[1] || entry.upper[1] < lo[1] || entry.lower[2] > hi[2] ||
              entry.upper[2] < lo[2])
            continue;
          const auto* shell = gameref_get(key);
          if (!shell || !shell->parent) continue;
          entry.promoted = true;
          g_pending_dormant.push_back({map, index, shell->parent, ground.matrix});
        }
      }
  }
}

void scene_instances_promote_dynamics() {
  if (g_pending_dormant.empty()) return;
  auto pending = std::move(g_pending_dormant);
  g_pending_dormant.clear();
  for (const auto& [map, index, ground, shell] : pending) {
    auto& entry = map->dormant[index];
    void* node = part_add_dynamic_native(*entry.config, entry.config->renders, entry.pose,
                                         ground, entry.seed);
    if (!node) {
      entry.promoted = false;
      continue;
    }
    // The loose body draws itself; hide the static instances it replaces.
    entry.live = node;
    // The frame the loose solver places the body in (see advance_loose_panels).
    if (!scene_instances_ground_frame(ground, entry.live_shell.data())) entry.live_shell = shell;
    g_live_dormant[node] = {map, index};
    show_dormant(entry, false);
  }
}

bool scene_instances_settle_dynamic(void* node, const float* world_frame) {
  const auto found = g_live_dormant.find(node);
  if (found == g_live_dormant.end()) return false;
  auto [map, index] = found->second;
  g_live_dormant.erase(found);
  auto& entry = map->dormant[index];
  PartMatrix pose{}, delta{};
  bool moved = world_frame != nullptr;
  if (moved) {
    PartMatrix world;
    std::copy(world_frame, world_frame + 16, world.begin());
    pose = part_matrix_multiply(part_matrix_inverse_rigid(entry.live_shell), world);
    delta = part_matrix_multiply(pose, part_matrix_inverse_rigid(entry.pose));
    // Its instances stay in their cells, whose culling spheres grow to keep
    // them. A prop pushed further than a cell's width returns to its old pose
    // instead of making its cell visible from everywhere.
    for (const auto& [cell, prop_index] : entry.props) {
      if (prop_index >= cell->props.size()) continue;
      const auto center = transform_point(delta, cell->props[prop_index].center);
      moved = moved && std::hypot(center[0] - cell->center[0], center[1] - cell->center[1],
                                  center[2] - cell->center[2]) <= kPropCellSize;
    }
  }
  if (moved) {
    for (const auto& [cell, prop_index] : entry.props) {
      if (prop_index >= cell->props.size()) continue;
      auto& prop = cell->props[prop_index];
      prop.pose = part_matrix_multiply(delta, prop.pose);
      prop.center = transform_point(delta, prop.center);
      const float reach = std::hypot(prop.center[0] - cell->center[0],
                                     prop.center[1] - cell->center[1],
                                     prop.center[2] - cell->center[2]) + prop.radius;
      cell->radius = std::max(cell->radius, reach);
    }
    dormant_cells(entry, [&](uint64_t cell) {
      auto grid = map->dormant_grid.find(cell);
      if (grid != map->dormant_grid.end()) std::erase(grid->second, index);
    });
    entry.pose = pose;
    dormant_bounds(entry);
    dormant_cells(entry, [&](uint64_t cell) { map->dormant_grid[cell].push_back(index); });
  }
  entry.live = nullptr;
  entry.promoted = false;
  show_dormant(entry, true);
  return true;
}

void scene_instances_local_matrix(const GameRefState& node, float matrix[16]) {
  if (node.matrix_override)
    std::copy(node.local_matrix, node.local_matrix + 16, matrix);
  else
    local_matrix(node, matrix);
}

bool scene_instances_native_body(uint32_t type, std::shared_ptr<const ScxGeometry>& mesh,
                                 float& mass, float center[3]) {
  if (!g_resources || !type || !center) return false;
  const auto& native = native_collision_data(*g_resources, type);
  if (!native.mesh) return false;
  mesh = native.mesh;
  mass = native.mass;
  std::copy(native.center.begin(), native.center.end(), center);
  return true;
}

bool scene_instances_ground_frame(void *ground, float *matrix) {
  if (!ground || !matrix) return false;
  const auto* parent = gameref_get(ground);
  if (!parent) return false;
  for (auto* child : parent->children) {
    const auto* shell = gameref_get(child);
    if (!shell || !g_ground_props.contains(static_cast<uint32_t>(shell->type_id))) continue;
    if (const auto found = g_ground_collision.find(child); found != g_ground_collision.end()) {
      std::copy(found->second.matrix.begin(), found->second.matrix.end(), matrix);
      return true;
    }
    return scene_instances_script_matrix(child, matrix);
  }
  return false;
}

namespace {
void collect_render_nodes(GameRefState* node, std::vector<GameRefState*>& out,
                          unsigned depth = 0, bool physics = false) {
  if (!node || node->empty || node->streamed_hidden || (node->flags & kWorldTreeLeaf) ||
      (depth && (node->flags & kWorldTreeRoot)) || depth >= 128)
    return;
  const bool render = node->kind == GameRefKind::RenderRef &&
                      (node->type_id || node->line_geometry);
  const bool physical = node->physics_shape != PhysicsShape::None ||
                        (node->collision_active && node->type_id);
  if (render || (physics && physical)) out.push_back(node);
  for (auto* child : node->children)
    collect_render_nodes(gameref_get(child), out, depth + 1, physics);
}

std::vector<GameRefState*> render_nodes_in_world(void* root, bool physics = false) {
  std::vector<GameRefState*> nodes;
  collect_render_nodes(gameref_get(root), nodes, 0, physics);
  return nodes;
}

// One triangle of a floor query, already in script space: keeps the highest
// surface under (x, z) that is not above `below` (+5 cm).
// `surface` is the triangle's; `surface_out` receives it when it wins.
void floor_test(const float (&v)[3][3], float x, float z, float below, float &best,
                bool &hit, float *normal, const GroundSurface &surface,
                GroundSurface *surface_out) {
  const float x0 = v[0][0], z0 = v[0][2], x1 = v[1][0], z1 = v[1][2];
  const float x2 = v[2][0], z2 = v[2][2];
  const float denom = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2);
  if (std::fabs(denom) < 1e-8f)
    return;
  const float a = ((z1 - z2) * (x - x2) + (x2 - x1) * (z - z2)) / denom;
  const float b = ((z2 - z0) * (x - x2) + (x0 - x2) * (z - z2)) / denom;
  const float c = 1.f - a - b;
  if (a < -1e-4f || b < -1e-4f || c < -1e-4f)
    return;
  const float y = a * v[0][1] + b * v[1][1] + c * v[2][1];
  if (y > below + 0.05f)
    return;
  if (!hit || y > best) {
    best = y;
    hit = true;
    if (surface_out) *surface_out = surface;
    if (normal) {
      const float ax = v[1][0] - v[0][0], ay = v[1][1] - v[0][1],
                  az = v[1][2] - v[0][2];
      const float bx = v[2][0] - v[0][0], by = v[2][1] - v[0][1],
                  bz = v[2][2] - v[0][2];
      float nx = ay * bz - az * by, ny = az * bx - ax * bz,
            nz = ax * by - ay * bx;
      const float size = std::sqrt(nx * nx + ny * ny + nz * nz);
      const float sign = ny < 0 ? -1.f : 1.f;
      normal[0] = nx / size * sign;
      normal[1] = ny / size * sign;
      normal[2] = nz / size * sign;
    }
  }
}

// Triangle t (an offset in idx) transformed by m, with its X/Z bounds. False
// for an index outside the positions.
bool floor_triangle(const float *m, const std::vector<float> &pos,
                    const std::vector<uint32_t> &idx, size_t t, float scale,
                    float (&v)[3][3], float (&bounds)[4]) {
  for (int k = 0; k < 3; ++k) {
    const uint32_t vi = idx[t + static_cast<size_t>(k)];
    if (static_cast<size_t>(vi) * 3 + 2 >= pos.size())
      return false;
    const float vx = pos[vi * 3] * scale;
    const float vy = pos[vi * 3 + 1] * scale;
    const float vz = pos[vi * 3 + 2] * scale;
    v[k][0] = m[0] * vx + m[4] * vy + m[8] * vz + m[12];
    v[k][1] = m[1] * vx + m[5] * vy + m[9] * vz + m[13];
    v[k][2] = m[2] * vx + m[6] * vy + m[10] * vz + m[14];
    if (k == 0) {
      bounds[0] = bounds[1] = v[0][0];
      bounds[2] = bounds[3] = v[0][2];
    } else {
      bounds[0] = std::min(bounds[0], v[k][0]);
      bounds[1] = std::max(bounds[1], v[k][0]);
      bounds[2] = std::min(bounds[2], v[k][2]);
      bounds[3] = std::max(bounds[3], v[k][2]);
    }
  }
  return true;
}

// The triangles of `idx` (a cell list of `mesh`, or its large triangles),
// in script space. `surfaces` lists their surface ids, or is null.
void floor_triangles(const StaticMesh &mesh, const std::vector<uint32_t> &idx,
                     const std::vector<uint16_t> *surfaces, float x, float z,
                     float below, float &best, bool &hit, float *normal,
                     GroundSurface *surface_out) {
  if (idx.size() < 3 || mesh.positions.size() < 9)
    return;
  float eye[16];
  identity(eye);
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    float v[3][3], bounds[4];
    if (!floor_triangle(eye, mesh.positions, idx, t, mesh.scale, v, bounds) ||
        x < bounds[0] || x > bounds[1] || z < bounds[2] || z > bounds[3])
      continue;
    const GroundSurface surface =
        surfaces && t / 3 < surfaces->size() ? mesh.surface((*surfaces)[t / 3]) : GroundSurface{};
    floor_test(v, x, z, below, best, hit, normal, surface, surface_out);
  }
}

// The cell under (x, z) and the large triangles of one mesh.
void floor_mesh(const StaticMesh &mesh, float x, float z, float below, float &best,
                bool &hit, float *normal, GroundSurface *surface_out) {
  const uint64_t key = floor_cell(cell_coordinate(x, mesh), cell_coordinate(z, mesh));
  const auto cell = mesh.cells.find(key);
  if (cell != mesh.cells.end())
    floor_triangles(mesh, cell->second, mesh.surfaces_for(cell->second, key), x, z, below,
                    best, hit, normal, surface_out);
  floor_triangles(mesh, mesh.large_triangles, mesh.surfaces_for(mesh.large_triangles, 0), x,
                  z, below, best, hit, normal, surface_out);
}
} // namespace

bool scene_shadow_projection(const float *frame, const SceneFloorHit &hit,
                             float *matrix) {
  const float n[3] = {hit.normal[0], hit.normal[1], -hit.normal[2]};
  const float d[3] = {-.35f, -.85f, .45f};
  const float dot = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
  if (!std::isfinite(dot) || std::fabs(dot) <= .1f)
    return false;
  const float distance =
      n[0] * frame[12] + n[1] * hit.height - n[2] * frame[14] + .012f;
  std::fill(matrix, matrix + 16, 0.f);
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r)
      matrix[c * 4 + r] = (r == c ? 1.f : 0.f) - d[r] * n[c] / dot;
  for (int r = 0; r < 3; ++r)
    matrix[12 + r] = d[r] * distance / dot;
  matrix[15] = 1;
  return true;
}
bool scene_shadow_info(uint32_t type_id, uint32_t *flags,
                       SceneShadowDecal *decal) {
  if (flags)
    *flags = 0;
  if (decal)
    *decal = {};
  if (!type_id || !g_resources)
    return false;
  auto found = g_shadow_info.find(type_id);
  if (found == g_shadow_info.end()) {
    ShadowInfo info;
    const auto flag_lines = g_resources->payload_props(type_id, "flags");
    if (!flag_lines.empty())
      info.flags = static_cast<uint32_t>(
          std::strtoul(flag_lines.front().c_str(), nullptr, 0));
    float center[3]{}, axis_u[3]{}, axis_v[3]{};
    auto parse3 = [](const std::string &text, float *out) {
      return std::sscanf(text.c_str(), "%f %f %f", out, out + 1, out + 2) == 3;
    };
    parse3(g_resources->payload_prop(type_id, "shd_center"), center);
    const bool have_u =
        parse3(g_resources->payload_prop(type_id, "shd_diru"), axis_u);
    const bool have_v =
        parse3(g_resources->payload_prop(type_id, "shd_dirv"), axis_v);
    const float ulen = axis_u[0] * axis_u[0] + axis_u[1] * axis_u[1] +
                       axis_u[2] * axis_u[2];
    const float vlen = axis_v[0] * axis_v[0] + axis_v[1] * axis_v[1] +
                       axis_v[2] * axis_v[2];
    const std::string tex = g_resources->payload_prop(type_id, "shd_texture");
    if (!tex.empty() && have_u && have_v && ulen > 1e-8f && vlen > 1e-8f) {
      const uint32_t rid =
          static_cast<uint32_t>(std::strtoul(tex.c_str(), nullptr, 0));
      const uint32_t full = g_resources->resolve_embedded_id(
          RpkIndex::pack_from_full(type_id), rid);
      info.decal.texture = g_resources->texture_path_for_id(full);
      if (!info.decal.texture.empty()) {
        info.decal.present = true;
        std::memcpy(info.decal.center, center, sizeof(center));
        std::memcpy(info.decal.axis_u, axis_u, sizeof(axis_u));
        std::memcpy(info.decal.axis_v, axis_v, sizeof(axis_v));
        float uv[4];
        const std::string uvs = g_resources->payload_prop(type_id, "shd_uv");
        if (std::sscanf(uvs.c_str(), "%f %f %f %f", uv, uv + 1, uv + 2,
                        uv + 3) == 4)
          std::memcpy(info.decal.uv, uv, sizeof(uv));
      }
    }
    found = g_shadow_info.emplace(type_id, std::move(info)).first;
  }
  if (flags)
    *flags = found->second.flags;
  if (decal)
    *decal = found->second.decal;
  return true;
}
bool scene_instances_floor(float x, float z, float below, void *ignore,
                           float *height) {
  if (!height)
    return false;
  SceneFloorHit hit;
  if (!scene_instances_sample_floor(x, z, below, ignore, hit))
    return false;
  *height = hit.height;
  return true;
}
void scene_instances_prepare_physics() {
  part_physics_join();
  gameref_process_collision_requests();
  g_script_collision_shapes.clear();
  std::unordered_set<void *> active;
  std::unordered_set<void *> active_ground;
  // Moved or new collision is rebuilt after the walk, on the job pool: each
  // rebuild only reads its source mesh and writes its own cache slot.
  struct MeshRebuild {
    StaticMesh* out;
    const StaticMesh* source;    // a map's merged prop collision, or
    const ScxGeometry* geometry; // a render node's mesh
    const PartMatrix* matrix;
  };
  std::vector<MeshRebuild> rebuilds;
  std::vector<std::pair<GroundCollisionWorld*, const GroundPropMap*>> rebuilt_grounds;
  if (g_world)
    for (auto *n : render_nodes_in_world(g_world, true)) {
      if (!n || n->empty || n->sky_render || n->vehicle_material || n->line_geometry ||
          n->dynamic_native || n->physics_simulated ||
          n->trigger_shape != TriggerShape::None)
        continue;
      // A skinned character is not landscape. Its bind mesh was becoming a
      // solid obstacle, offset from the drawn body, and it destroyed the car.
      if (n->alias == "pedestrian" || n->alias == "driver") continue;
      std::array<float, 16> m;
      if (!chain_matrix(n->key, g_world, m.data(), false))
        continue;
      const auto id = static_cast<uint32_t>(n->type_id);
      // RenderRef creates a visual, not a physics body. Ground shells and
      // explicitly registered collision meshes retain their collision source.
      if (n->kind == GameRefKind::RenderRef && !n->collision_active &&
          n->physics_shape == PhysicsShape::None &&
          g_ground_props.find(id) == g_ground_props.end() &&
          g_collision_overrides.find(id) == g_collision_overrides.end())
        continue;
      if (n->kind == GameRefKind::RenderRef) {
        const auto ground = g_ground_props.find(id);
        if (ground != g_ground_props.end()) {
          active_ground.insert(n->key);
          auto& cached = g_ground_collision[n->key];
          const bool source_changed = cached.source != &ground->second;
          cached.source = &ground->second;
          if (source_changed || cached.matrix != m) {
            cached.matrix = m;
            cached.inverse = part_matrix_inverse_rigid(m);
            cached.meshes.assign(ground->second.collision_meshes.size(), {});
            for (size_t k = 0; k < cached.meshes.size(); ++k)
              rebuilds.push_back({&cached.meshes[k], &ground->second.collision_meshes[k],
                                  nullptr, &cached.matrix});
            rebuilt_grounds.emplace_back(&cached, &ground->second);
            ++g_floor_builds;
            ++g_floor_revision;
          }
        }
      }
      std::shared_ptr<const ScxGeometry> geo;
      if (n->physics_shape == PhysicsShape::Box ||
          n->physics_shape == PhysicsShape::Sphere) {
        geo = primitive_geometry(*n);
        if (n->physics_shape == PhysicsShape::Box)
          g_script_collision_shapes.add_box(
              m, {n->physics_size[0], n->physics_size[1], n->physics_size[2]}, n->key);
        else
          g_script_collision_shapes.add_sphere(m, n->physics_size[0], n->key);
      } else if (g_resources && (n->physics_shape == PhysicsShape::Resource ||
                                 (n->kind != GameRefKind::RenderRef && n->collision_active))) {
        const auto& native = native_collision_data(*g_resources, id);
        // Native CFG bodies are authoritative. In particular, an object with
        // no body (or `nocollision`) must not inherit its render mesh.
        geo = native.mesh;
        for (const auto& body : native.primitives) {
          const auto frame = part_matrix_multiply(m, part_pose_matrix(body.pose));
          if (body.shape == PartBodyShape::Box)
            g_script_collision_shapes.add_box(
                frame, {body.box_half[0], body.box_half[1], body.box_half[2]}, n->key);
          else if (body.shape == PartBodyShape::Sphere)
            g_script_collision_shapes.add_sphere(frame, body.sphere_radius, n->key);
        }
      } else {
        const auto collision = g_collision_overrides.find(id);
        geo = collision != g_collision_overrides.end() ? collision->second : geometry(id);
      }
      if (!geo || !geo->skin_weights.empty())
        continue;
      active.insert(n->key);
      auto &cached = g_floor_meshes[n->key];
      if (cached.source != geo || cached.matrix != m) {
        cached.source = geo;
        cached.matrix = m;
        rebuilds.push_back({&cached.mesh, nullptr, geo.get(), &cached.matrix});
        ++g_floor_builds;
        ++g_floor_revision;
      }
      cached.ancestors.clear();
      void *parent = n->key;
      for (int guard = 0; parent && guard < 32; ++guard) {
        cached.ancestors.push_back(parent);
        auto *node = gameref_get(parent);
        parent = node ? node->parent : nullptr;
      }
    }
  jobs_parallel_for(rebuilds.size(), [&](size_t i) {
    const auto& task = rebuilds[i];
    *task.out = task.geometry ? bake_static_mesh(*task.geometry, *task.matrix, false)
                              : transform_static_mesh(*task.source, *task.matrix, false);
    if (!pool_floor_index(*task.out)) index_floor(*task.out);
  });
  // A large mesh is indexed after the batch, one at a time across the pool.
  for (const auto& task : rebuilds)
    if (pool_floor_index(*task.out)) index_floor(*task.out);
  for (const auto& [rebuilt, source] : rebuilt_grounds) {
    auto& cached = *rebuilt;
    const auto& m = cached.matrix;
    cached.has_bounds = false;
    for (const auto& mesh : cached.meshes) {
      for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
        for (int axis = 0; axis < 3; ++axis) {
          const float value = mesh.positions[i + axis] * mesh.scale;
          if (!cached.has_bounds) {
            cached.bounds_min[axis] = cached.bounds_max[axis] = value;
          } else {
            cached.bounds_min[axis] = std::min(cached.bounds_min[axis], value);
            cached.bounds_max[axis] = std::max(cached.bounds_max[axis], value);
          }
        }
        cached.has_bounds = true;
      }
    }
    // Coarse sphere meshes can be smaller than their analytic shape.
    // Union exact primitive bounds before using the shell AABB as a
    // conservative rejection test.
    if (const auto analytic = source->collision_shapes.bounds()) {
      for (unsigned corners = 0; corners < 8; ++corners) {
        const double x = (corners & 1) ? analytic->maximum.x : analytic->minimum.x;
        const double y = (corners & 2) ? analytic->maximum.y : analytic->minimum.y;
        const double z = (corners & 4) ? analytic->maximum.z : analytic->minimum.z;
        const float world[3] = {
            static_cast<float>(m[0] * x + m[4] * y + m[8] * z + m[12]),
            static_cast<float>(m[1] * x + m[5] * y + m[9] * z + m[13]),
            static_cast<float>(m[2] * x + m[6] * y + m[10] * z + m[14])};
        for (int axis = 0; axis < 3; ++axis) {
          if (!cached.has_bounds) {
            cached.bounds_min[axis] = cached.bounds_max[axis] = world[axis];
          } else {
            cached.bounds_min[axis] = std::min(cached.bounds_min[axis], world[axis]);
            cached.bounds_max[axis] = std::max(cached.bounds_max[axis], world[axis]);
          }
        }
        cached.has_bounds = true;
      }
    }
  }
  for (auto it = g_floor_meshes.begin(); it != g_floor_meshes.end();)
    if (!active.count(it->first)) {
      ++g_floor_revision;
      it = g_floor_meshes.erase(it);
    } else
      ++it;
  for (auto it = g_ground_collision.begin(); it != g_ground_collision.end();)
    if (!active_ground.count(it->first)) {
      ++g_floor_revision;
      it = g_ground_collision.erase(it);
    } else
      ++it;
  for (auto it = g_primitive_sources.begin(); it != g_primitive_sources.end();)
    if (!active.count(it->first))
      it = g_primitive_sources.erase(it);
    else
      ++it;
  g_floor_prepared = true;
}
uint64_t scene_instances_floor_builds() { return g_floor_builds; }
uint64_t scene_instances_floor_revision() { return g_floor_revision; }

namespace {
using physics::Vec3;

Vec3 closest_on_edge(Vec3 p, Vec3 a, Vec3 b) {
  const Vec3 edge = b - a;
  const double length2 = edge.dot(edge);
  return length2 > 0.0
             ? a + edge * std::clamp((p - a).dot(edge) / length2, 0.0, 1.0)
             : a;
}

Vec3 closest_on_triangle(Vec3 point, const Vec3 (&v)[3], Vec3 normal) {
  const Vec3 projected = point - normal * (point - v[0]).dot(normal);
  Vec3 closest = projected;
  double edge_distance2 = std::numeric_limits<double>::infinity();
  for (int j = 0; j < 3; ++j) {
    const Vec3 a = v[j], b = v[(j + 1) % 3];
    if ((b - a).cross(projected - a).dot(normal) >= 0) continue;
    const Vec3 edge = closest_on_edge(projected, a, b);
    const Vec3 offset = edge - projected;
    const double distance2 = offset.dot(offset);
    if (distance2 < edge_distance2) {
      closest = edge;
      edge_distance2 = distance2;
    }
  }
  return closest;
}

bool triangle_vertices(const std::vector<float>& positions,
                       const std::vector<uint32_t>& indices, size_t first,
                       double scale, Vec3 (&v)[3]) {
  for (int j = 0; j < 3; ++j) {
    const size_t offset = static_cast<size_t>(indices[first + j]) * 3;
    if (offset + 2 >= positions.size()) return false;
    const float* p = positions.data() + offset;
    v[j] = Vec3{p[0], p[1], p[2]} * scale;
  }
  return true;
}

void sweep_triangles(const std::vector<float>& positions, double scale,
                     const std::vector<uint32_t> &indices,
                     Vec3 start, Vec3 delta, double radius, SceneSphereHit &hit,
                     float max_vertical_normal) {
  const Vec3 end = start + delta;
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    Vec3 v[3];
    if (!triangle_vertices(positions, indices, i, scale, v) ||
        std::min({v[0].y, v[1].y, v[2].y}) > std::max(start.y, end.y) + radius ||
        std::max({v[0].y, v[1].y, v[2].y}) < std::min(start.y, end.y) - radius)
      continue;
    const Vec3 n = (v[1] - v[0]).cross(v[2] - v[0]).normalized();
    if (n.dot(n) < 0.5 || std::abs(n.y) > max_vertical_normal) continue;

    double t = 0.0;
    // Conservative advancement against the face, edges and vertices.
    for (int step = 0; step < 12 && t < hit.fraction; ++step) {
      const Vec3 p = start + delta * t;
      const Vec3 offset = p - closest_on_triangle(p, v, n);
      const double distance = offset.length();
      const Vec3 normal = distance > 1e-8 ? offset / distance
                                         : (delta.dot(n) > 0.0 ? -n : n);
      const double closing = -delta.dot(normal);
      if (closing <= 1e-8) break;
      if (distance <= radius + 1e-5) {
        if (std::abs(normal.y) > max_vertical_normal) break;
        hit.fraction = static_cast<float>(t);
        hit.normal[0] = static_cast<float>(normal.x);
        hit.normal[1] = static_cast<float>(normal.y);
        hit.normal[2] = static_cast<float>(normal.z);
        hit.penetration = static_cast<float>(std::max(0., radius - distance));
        break;
      }
      t += (distance - radius) / closing;
    }
  }
}
} // namespace

bool scene_mesh_sweep_sphere(const ScxGeometry& mesh, Vec3 start, Vec3 movement,
                             double radius, SceneSphereHit& hit) {
  const Vec3 end = start + movement * hit.fraction;
  const double a[]{start.x, start.y, start.z}, b[]{end.x, end.y, end.z};
  for (int axis = 0; axis < 3; ++axis)
    if (std::min(a[axis], b[axis]) > mesh.info.bbox_max[axis] * mesh.position_scale + radius ||
        std::max(a[axis], b[axis]) < mesh.info.bbox_min[axis] * mesh.position_scale - radius)
      return false;
  const auto fraction = hit.fraction;
  sweep_triangles(mesh.positions, mesh.position_scale, mesh.indices, start, movement,
                  radius, hit, 1.f);
  return hit.fraction < fraction;
}

double scene_mesh_nearest(const ScxGeometry& mesh, Vec3 point, Vec3& closest) {
  double best = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    Vec3 v[3];
    if (!triangle_vertices(mesh.positions, mesh.indices, i, mesh.position_scale, v)) continue;
    const Vec3 normal = (v[1] - v[0]).cross(v[2] - v[0]).normalized();
    if (normal.dot(normal) < .5) continue;
    const Vec3 candidate = closest_on_triangle(point, v, normal);
    const Vec3 offset = point - candidate;
    const double distance2 = offset.dot(offset);
    if (distance2 < best) {
      best = distance2;
      closest = candidate;
    }
  }
  return std::sqrt(best);
}

bool scene_instances_sweep_sphere(const float start[3], const float delta[3],
                                  float radius, SceneSphereHit &hit,
                                  float max_vertical_normal) {
  const Vec3 from{start[0], start[1], start[2]};
  const Vec3 movement{delta[0], delta[1], delta[2]};
  if (!from.finite() || !movement.finite() || movement.dot(movement) < 1e-12)
    return false;
  if (!g_floor_prepared) scene_instances_prepare_physics();
  hit = {};
  const Vec3 to = from + movement;
  const auto sweep_mesh = [&](const StaticMesh &mesh) {
    const int32_t x0 = cell_coordinate(std::min(from.x, to.x) - radius, mesh);
    const int32_t x1 = cell_coordinate(std::max(from.x, to.x) + radius, mesh);
    const int32_t z0 = cell_coordinate(std::min(from.z, to.z) - radius, mesh);
    const int32_t z1 = cell_coordinate(std::max(from.z, to.z) + radius, mesh);
    for (int32_t x = x0; x <= x1; ++x)
      for (int32_t z = z0; z <= z1; ++z) {
        const auto cell = mesh.cells.find(floor_cell(x, z));
        if (cell != mesh.cells.end())
          sweep_triangles(mesh.positions, mesh.scale, cell->second, from, movement, radius, hit,
                          max_vertical_normal);
      }
    sweep_triangles(mesh.positions, mesh.scale, mesh.large_triangles, from, movement, radius, hit,
                    max_vertical_normal);
  };
  if (g_static_enabled)
    for (const auto &mesh : g_static) sweep_mesh(mesh);
  for (const auto& [key, ground] : g_ground_collision)
    for (const auto& mesh : ground.meshes) sweep_mesh(mesh);
  for (const auto &entry : g_floor_meshes) sweep_mesh(entry.second.mesh);
  return hit.fraction < 1.f;
}

bool scene_instances_contact_box(const float frame[16], const float half[3],
                                 SceneBodyContact& contact, const void* ignore) {
  contact = {};
  if (!frame || !half) return false;
  PartMatrix moving{};
  std::copy(frame, frame + 16, moving.begin());
  const physics::Vec3 extents{half[0], half[1], half[2]};
  std::array<float, 3> moving_min{}, moving_max{};
  for (int axis = 0; axis < 3; ++axis) {
    float radius = 0.f;
    for (int local = 0; local < 3; ++local)
      radius += std::fabs(moving[local * 4 + axis]) * half[local];
    moving_min[axis] = moving[12 + axis] - radius;
    moving_max[axis] = moving[12 + axis] + radius;
  }
  std::optional<collision::Contact> best;
  const GroundCollisionWorld* best_ground = nullptr;
  const auto consider = [&](const std::optional<collision::Contact>& candidate,
                            const GroundCollisionWorld* ground = nullptr) {
    if (candidate && (!best || candidate->penetration > best->penetration)) {
      best = candidate;
      best_ground = ground;
    }
  };
  for (const auto& [key, ground] : g_ground_collision) {
    if (!ground.source) continue;
    bool separated = false;
    if (ground.has_bounds)
      for (int axis = 0; axis < 3; ++axis)
        separated = separated || moving_max[axis] < ground.bounds_min[axis] ||
                    moving_min[axis] > ground.bounds_max[axis];
    if (separated) continue;
    const auto local = part_matrix_multiply(ground.inverse, moving);
    consider(ground.source->collision_shapes.overlap_box(local, extents, ignore), &ground);
  }
  consider(g_script_collision_shapes.overlap_box(moving, extents, ignore));
  if (!best) return false;
  contact.owner = const_cast<void*>(best->owner);
  contact.penetration = static_cast<float>(best->penetration);
  physics::Vec3 normal = best->normal;
  physics::Vec3 point = best->point;
  if (best_ground) {
    const auto& m = best_ground->matrix;
    normal = {m[0] * normal.x + m[4] * normal.y + m[8] * normal.z,
              m[1] * normal.x + m[5] * normal.y + m[9] * normal.z,
              m[2] * normal.x + m[6] * normal.y + m[10] * normal.z};
    point = {m[0] * point.x + m[4] * point.y + m[8] * point.z + m[12],
             m[1] * point.x + m[5] * point.y + m[9] * point.z + m[13],
             m[2] * point.x + m[6] * point.y + m[10] * point.z + m[14]};
  }
  contact.normal[0] = static_cast<float>(normal.x);
  contact.normal[1] = static_cast<float>(normal.y);
  contact.normal[2] = static_cast<float>(normal.z);
  contact.point[0] = static_cast<float>(point.x);
  contact.point[1] = static_cast<float>(point.y);
  contact.point[2] = static_cast<float>(point.z);
  return true;
}

bool scene_instances_contact_walls(const float frame[16], const float half[3],
                                   SceneBodyContact& contact, float max_vertical_normal) {
  contact = {};
  if (!frame || !half) return false;
  PartMatrix matrix{};
  std::copy(frame, frame + 16, matrix.begin());
  collision::Box box;
  if (!collision::make_box(matrix, {half[0], half[1], half[2]}, box)) return false;
  float lower[3], upper[3];
  for (int axis = 0; axis < 3; ++axis) {
    float radius = 0.f;
    for (int local = 0; local < 3; ++local)
      radius += std::fabs(matrix[local * 4 + axis]) * half[local];
    lower[axis] = matrix[12 + axis] - radius;
    upper[axis] = matrix[12 + axis] + radius;
  }
  std::optional<collision::Contact> best;
  const auto test = [&](const StaticMesh& mesh, const std::vector<uint32_t>& indices) {
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
      std::array<physics::Vec3, 3> triangle;
      bool inside = true;
      float tri_lower[3], tri_upper[3];
      for (int k = 0; k < 3; ++k) {
        const size_t offset = static_cast<size_t>(indices[i + k]) * 3;
        if (offset + 2 >= mesh.positions.size()) {
          inside = false;
          break;
        }
        const float* p = mesh.positions.data() + offset;
        triangle[k] = {p[0] * mesh.scale, p[1] * mesh.scale, p[2] * mesh.scale};
        for (int axis = 0; axis < 3; ++axis) {
          const float value = p[axis] * mesh.scale;
          tri_lower[axis] = k ? std::min(tri_lower[axis], value) : value;
          tri_upper[axis] = k ? std::max(tri_upper[axis], value) : value;
        }
      }
      if (!inside) continue;
      for (int axis = 0; axis < 3 && inside; ++axis)
        inside = tri_upper[axis] >= lower[axis] && tri_lower[axis] <= upper[axis];
      if (!inside) continue;
      const physics::Vec3 face =
          (triangle[1] - triangle[0]).cross(triangle[2] - triangle[0]).normalized();
      if (face.dot(face) < .5 || std::abs(face.y) > max_vertical_normal) continue;
      const auto candidate = collision::overlap_triangle(box, triangle);
      if (!candidate || std::abs(candidate->normal.y) > max_vertical_normal) continue;
      if (!best || candidate->penetration > best->penetration) best = candidate;
    }
  };
  const auto test_mesh = [&](const StaticMesh& mesh) {
    const int32_t x0 = cell_coordinate(lower[0], mesh), x1 = cell_coordinate(upper[0], mesh);
    const int32_t z0 = cell_coordinate(lower[2], mesh), z1 = cell_coordinate(upper[2], mesh);
    if (static_cast<int64_t>(x1 - x0 + 1) * (z1 - z0 + 1) > 256) return;
    for (int32_t x = x0; x <= x1; ++x)
      for (int32_t z = z0; z <= z1; ++z) {
        const auto cell = mesh.cells.find(floor_cell(x, z));
        if (cell != mesh.cells.end()) test(mesh, cell->second);
      }
    test(mesh, mesh.large_triangles);
  };
  // The same triangle sets as scene_instances_sweep_sphere: map collision
  // (fences and walls included), native placements and activated objects.
  if (!g_floor_prepared) scene_instances_prepare_physics();
  if (g_static_enabled)
    for (const auto& mesh : g_static) test_mesh(mesh);
  for (const auto& [key, ground] : g_ground_collision) {
    (void)key;
    bool separated = false;
    if (ground.has_bounds)
      for (int axis = 0; axis < 3; ++axis)
        separated = separated || upper[axis] < ground.bounds_min[axis] ||
                    lower[axis] > ground.bounds_max[axis];
    if (separated) continue;
    for (const auto& mesh : ground.meshes) test_mesh(mesh);
  }
  for (const auto& entry : g_floor_meshes) test_mesh(entry.second.mesh);
  if (!best) return false;
  for (int axis = 0; axis < 3; ++axis) {
    contact.normal[axis] = static_cast<float>(axis == 0 ? best->normal.x
                                              : axis == 1 ? best->normal.y : best->normal.z);
    contact.point[axis] = static_cast<float>(axis == 0 ? best->point.x
                                             : axis == 1 ? best->point.y : best->point.z);
  }
  contact.penetration = static_cast<float>(best->penetration);
  return true;
}

bool scene_instances_sample_floor(float x, float z, float below, void *ignore,
                                  SceneFloorHit &sample) {
  if (!g_floor_prepared) scene_instances_prepare_physics();
  float best = 0.f;
  bool hit = false;
  GroundSurface surface;
  // Preloaded garage shells are drawn at identity with unflipped script XZ.
  if (g_static_enabled)
    for (const auto &mesh : g_static)
      floor_mesh(mesh, x, z, below, best, hit, sample.normal, &surface);
  for (const auto& [key, ground] : g_ground_collision)
    for (const auto& mesh : ground.meshes)
      floor_mesh(mesh, x, z, below, best, hit, sample.normal, &surface);
  for (const auto &entry : g_floor_meshes) {
    const auto &cached = entry.second;
    if (ignore && std::find(cached.ancestors.begin(), cached.ancestors.end(),
                            ignore) != cached.ancestors.end())
      continue;
    floor_mesh(cached.mesh, x, z, below, best, hit, sample.normal, &surface);
  }
  if (!hit)
    return false;
  sample.height = best;
  sample.surface = surface;
  return true;
}

namespace {
constexpr float kPatchCell = .5f;  // metres
constexpr int kPatchCells = 64;    // per axis at most
}  // namespace

void scene_instances_floor_patch(float x0, float z0, float x1, float z1, void *ignore,
                                 SceneFloorPatch &patch) {
  if (!g_floor_prepared) scene_instances_prepare_physics();
  patch.triangles.clear();
  patch.start.clear();
  patch.list.clear();
  patch.ignore = ignore;
  patch.revision = g_floor_revision;
  patch.x0 = x0;
  patch.z0 = z0;
  patch.x1 = x1;
  patch.z1 = z1;
  patch.nx = patch.nz = 0;
  if (!(x1 >= x0 && z1 >= z0) || x1 - x0 > kPatchCell * kPatchCells ||
      z1 - z0 > kPatchCell * kPatchCells)
    return;  // no grid: every query falls back
  float eye[16];
  identity(eye);
  // The meshes in the order scene_instances_sample_floor tests them; each
  // mesh's cell triangles before its large ones.
  const auto add_list = [&](const StaticMesh &mesh, const std::vector<uint32_t> &idx,
                            uint64_t key) {
    if (idx.size() < 3 || mesh.positions.size() < 9) return;
    const auto *surfaces = mesh.surfaces_for(idx, key);
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
      SceneFloorPatch::Triangle tri;
      if (!floor_triangle(eye, mesh.positions, idx, t, mesh.scale, tri.v, tri.bounds) ||
          tri.bounds[1] < x0 || tri.bounds[0] > x1 || tri.bounds[3] < z0 || tri.bounds[2] > z1)
        continue;
      if (surfaces && t / 3 < surfaces->size()) tri.surface = mesh.surface((*surfaces)[t / 3]);
      patch.triangles.push_back(tri);
    }
  };
  const auto add_mesh = [&](const StaticMesh &mesh) {
    const int32_t cx0 = cell_coordinate(x0, mesh), cx1 = cell_coordinate(x1, mesh);
    const int32_t cz0 = cell_coordinate(z0, mesh), cz1 = cell_coordinate(z1, mesh);
    for (int32_t cx = cx0; cx <= cx1; ++cx)
      for (int32_t cz = cz0; cz <= cz1; ++cz) {
        const uint64_t key = floor_cell(cx, cz);
        const auto cell = mesh.cells.find(key);
        if (cell != mesh.cells.end()) add_list(mesh, cell->second, key);
      }
    add_list(mesh, mesh.large_triangles, 0);
  };
  if (g_static_enabled)
    for (const auto &mesh : g_static) add_mesh(mesh);
  for (const auto &[key, ground] : g_ground_collision)
    for (const auto &mesh : ground.meshes) add_mesh(mesh);
  for (const auto &entry : g_floor_meshes) {
    const auto &cached = entry.second;
    if (ignore && std::find(cached.ancestors.begin(), cached.ancestors.end(), ignore) !=
                      cached.ancestors.end())
      continue;
    add_mesh(cached.mesh);
  }
  // A grid of kPatchCell cells over the rectangle, each listing its
  // triangles in the order above (counted, then filled).
  patch.nx = std::clamp(static_cast<int>(std::ceil((x1 - x0) / kPatchCell)), 1, kPatchCells);
  patch.nz = std::clamp(static_cast<int>(std::ceil((z1 - z0) / kPatchCell)), 1, kPatchCells);
  const auto column = [&](float x) {
    return std::clamp(static_cast<int>(std::floor((x - x0) / kPatchCell)), 0, patch.nx - 1);
  };
  const auto row = [&](float z) {
    return std::clamp(static_cast<int>(std::floor((z - z0) / kPatchCell)), 0, patch.nz - 1);
  };
  patch.start.assign(static_cast<size_t>(patch.nx * patch.nz) + 1, 0);
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1) {
      for (size_t c = 1; c < patch.start.size(); ++c) patch.start[c] += patch.start[c - 1];
      patch.list.resize(patch.start.back());
    }
    for (uint32_t t = 0; t < patch.triangles.size(); ++t) {
      const auto &b = patch.triangles[t].bounds;
      const int cx0 = column(b[0]), cx1 = column(b[1]), cz0 = row(b[2]), cz1 = row(b[3]);
      for (int cz = cz0; cz <= cz1; ++cz)
        for (int cx = cx0; cx <= cx1; ++cx) {
          const size_t c = static_cast<size_t>(cz * patch.nx + cx);
          if (pass == 0) ++patch.start[c + 1];
          else patch.list[patch.start[c]++] = t;
        }
    }
  }
  // The fill advanced each start to the next cell's: shift them back.
  for (size_t c = patch.start.size() - 1; c > 0; --c) patch.start[c] = patch.start[c - 1];
  patch.start[0] = 0;
}

bool scene_instances_floor_patch_covers(const SceneFloorPatch &patch, float x0, float z0,
                                        float x1, float z1, void *ignore) {
  return patch.nx && g_floor_prepared && patch.revision == g_floor_revision &&
         patch.ignore == ignore && x0 >= patch.x0 && x1 <= patch.x1 && z0 >= patch.z0 &&
         z1 <= patch.z1;
}

bool scene_instances_sample_floor(const SceneFloorPatch &patch, float x, float z,
                                  float below, SceneFloorHit &sample) {
  if (!patch.nx || !(x >= patch.x0 && x <= patch.x1 && z >= patch.z0 && z <= patch.z1))
    return scene_instances_sample_floor(x, z, below, patch.ignore, sample);
  const int cx = std::clamp(static_cast<int>(std::floor((x - patch.x0) / kPatchCell)), 0,
                            patch.nx - 1);
  const int cz = std::clamp(static_cast<int>(std::floor((z - patch.z0) / kPatchCell)), 0,
                            patch.nz - 1);
  const size_t c = static_cast<size_t>(cz * patch.nx + cx);
  float best = 0.f;
  bool hit = false;
  GroundSurface surface;
  for (uint32_t i = patch.start[c]; i < patch.start[c + 1]; ++i) {
    const auto &tri = patch.triangles[patch.list[i]];
    if (x < tri.bounds[0] || x > tri.bounds[1] || z < tri.bounds[2] || z > tri.bounds[3])
      continue;
    floor_test(tri.v, x, z, below, best, hit, sample.normal, tri.surface, &surface);
  }
  if (!hit)
    return false;
  sample.height = best;
  sample.surface = surface;
  return true;
}

bool scene_instances_supports(void *root,
                              std::vector<std::array<float, 3>> &points) {
  points.clear();
  if (!g_world || !root)
    return false;
  for (auto *n : render_nodes_in_world(root)) {
    if (!n || n->empty || n->sky_render || n->line_geometry)
      continue;
    bool under = false;
    void *p = n->key;
    for (int guard = 0; p && guard < 32; ++guard) {
      if (p == root) {
        under = true;
        break;
      }
      auto *s = gameref_get(p);
      p = s ? s->parent : nullptr;
    }
    if (!under)
      continue;
    float m[16];
    if (!chain_matrix(n->key, g_world, m, false))
      continue;
    auto geo = geometry(static_cast<uint32_t>(n->type_id));
    if (!geo || geo->positions.size() < 3)
      continue;
    const float scale = geo->position_scale;
    bool any = false;
    float best = 0.f, bx = 0.f, bz = 0.f;
    for (size_t i = 0; i + 2 < geo->positions.size(); i += 3) {
      const float vx = geo->positions[i] * scale;
      const float vy = geo->positions[i + 1] * scale;
      const float vz = geo->positions[i + 2] * scale;
      const float y = m[1] * vx + m[5] * vy + m[9] * vz + m[13];
      if (any && y >= best)
        continue;
      best = y;
      bx = m[0] * vx + m[4] * vy + m[8] * vz + m[12];
      bz = m[2] * vx + m[6] * vy + m[10] * vz + m[14];
      any = true;
    }
    if (any)
      points.push_back({bx, best, bz});
  }
  return !points.empty();
}

namespace {
// The world's light before the day cycle, and whether the cycle applies.
SceneSun base_light(void* root, bool* cycle) {
  SceneSun selected=scene_sun();int newest=0;
  for(auto* node:gameref_directional_lights()) {
    if(!node->directional_light.set || node->live_id<=newest)continue;
    float frame[16];
    // chain_matrix rejects inventory world roots and hidden subtrees.
    if(!chain_matrix(node->key,root,frame,false))continue;
    selected=node->directional_light;newest=node->live_id;
    const auto& d=node->directional_light.direction;
    for(int i=0;i<3;++i)selected.direction[i]=frame[i]*d[0]+frame[4+i]*d[1]+frame[8+i]*d[2];
  }
  *cycle = root == g_world && scene_instances_outdoor();
  return selected;
}
float luminance(const float* rgb) {
  return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
}
}  // namespace

SceneSun scene_instances_light(void* root) {
  if(!root)root=g_world;
  bool cycle=false;
  SceneSun selected=base_light(root,&cycle);
  if (cycle)
    scene_sun_apply_day_cycle(selected);
  return selected;
}

SceneSunState scene_instances_sun_state() {
  SceneSunState state;
  bool cycle=false;
  const SceneSun base=base_light(g_world,&cycle);
  SceneSun now=base, noon=base;
  if (cycle) {
    scene_sun_apply_day_cycle(now);
    scene_sun_apply_day_cycle_at(noon, 12.f * 3600.f);
  }
  const float length=std::sqrt(now.direction[0]*now.direction[0]+now.direction[1]*now.direction[1]+
                               now.direction[2]*now.direction[2]);
  // The direction is where the light travels: a sun above the horizon points down.
  state.elevation = now.has_direction && length > 1e-6f
      ? std::asin(std::clamp(-now.direction[1] / length, -1.f, 1.f)) : 0.f;
  state.diffuse = now.set ? luminance(now.diffuse) : 0.f;
  state.noon = noon.set ? luminance(noon.diffuse) : 0.f;
  // update_static_shadow casts no sun shadow from a direction with y >= 0.02.
  state.shadowing = now.has_direction && now.direction[1] < 0.02f && state.diffuse > 0.f;
  state.direct = state.noon > 1e-4f ? std::clamp(state.diffuse / state.noon, 0.f, 1.f) : 0.f;
  state.night = scene_sun_night_factor(state.direct);
  return state;
}
namespace {
constexpr int kSkinBones = 48;

// False when the clip gives no bones; the instance then keeps the CPU pose.
// Safe on the job pool for distinct characters (#268): it writes only the
// instance and the prepared character.
bool skin_palette_prepared(SceneInstance& instance, void* prepared) {
  instance.skin.assign(static_cast<size_t>(kSkinBones) * 16, 0.f);
  for (int bone = 0; bone < kSkinBones; ++bone) {
    float* m = instance.skin.data() + static_cast<size_t>(bone) * 16;
    m[0] = m[5] = m[10] = m[15] = 1.f;
  }
  const int count = character_skin_palette_prepared(prepared, instance.skin.data(), kSkinBones);
  if (count <= 0) {
    instance.skin.clear();
    instance.gpu_skin = false;
    return false;
  }
  // Vertices are uploaded as position_scale * (x, y, -z). The palette
  // has to land in that space: F*M*F, then scale the translation.
  const float scale = instance.geometry->position_scale > 0.f
      ? instance.geometry->position_scale : 0.01f;
  for (int bone = 0; bone < kSkinBones; ++bone) {
    float* m = instance.skin.data() + static_cast<size_t>(bone) * 16;
    m[2] = -m[2];
    m[6] = -m[6];
    m[10] = -m[10];
    m[14] = -m[14];
    m[8] = -m[8];
    m[9] = -m[9];
    m[10] = -m[10];
    m[11] = -m[11];
    m[12] *= scale;
    m[13] *= scale;
    m[14] *= scale;
  }
  instance.gpu_skin = true;
  return true;
}
bool skin_palette(SceneInstance& instance, void* key, int32_t type_id) {
  return skin_palette_prepared(instance, character_skin_prepare(key, type_id, *g_resources));
}

// Lowercase texture path without extension or game/ prefix: the mesh list
// may prefer a DDS sidecar while the resource names the original image.
std::string texture_key(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  for (char &c : path)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto slash = path.find_last_of('/');
  const auto dot = path.find_last_of('.');
  if (dot != std::string::npos &&
      (slash == std::string::npos || dot > slash))
    path.resize(dot);
  const auto game = path.rfind("game/");
  if (game != std::string::npos)
    path = path.substr(game + 5);
  return path;
}

// texture_key of every texture of a mesh, kept per mesh and revision (#268):
// a painted or retextured part matched them string by string every frame.
// The weak pointer catches a freed mesh whose address was reused. Host
// thread only.
const std::vector<std::string>& mesh_texture_keys(const std::shared_ptr<const ScxGeometry>& mesh) {
  struct Entry {
    std::weak_ptr<const ScxGeometry> owner;
    uint32_t revision = 0;
    std::vector<std::string> keys;
  };
  static std::unordered_map<const ScxGeometry*, Entry> cache;
  static size_t prune_at = 256;
  if (cache.size() >= prune_at) {
    std::erase_if(cache, [](const auto& item) { return item.second.owner.expired(); });
    prune_at = std::max<size_t>(256, cache.size() * 2);
  }
  auto& entry = cache[mesh.get()];
  if (entry.owner.lock() != mesh || entry.revision != mesh->revision ||
      entry.keys.size() != mesh->textures.size()) {
    entry.owner = mesh;
    entry.revision = mesh->revision;
    entry.keys.clear();
    for (const auto& texture : mesh->textures) entry.keys.push_back(texture_key(texture));
  }
  return entry.keys;
}

// defer_skin leaves pedestrians with gpu_skin set and an empty palette.
std::vector<SceneInstance> collect_root(void *root, bool defer_skin) {
  std::vector<SceneInstance> out;
  if (!g_resources || !root)
    return out;
  const auto lighting=scene_instances_light(root);
  // Rectangle.setMatrix already projected this instance into the 2D OSD.
  // Cursor/OSD cameras must not draw the unscaled template mesh again.
  // Keep hidden/retired UI rectangles out too: their visibility is owned
  // by the OSD registry, not by the surviving resource instance.
  const auto osd_rectangle = [](const GameRefState* n) {
    const auto* script = static_cast<Object*>(n->script);
    if (!script) return false;
    const auto rect = script->fields.find("_rect");
    const auto widget = script->fields.find("_osd_wi");
    return rect != script->fields.end() && rect->second.tag == ValueTag::Int &&
           rect->second.v.i != 0 && widget != script->fields.end() &&
           widget->second.tag == ValueTag::Int;
  };
  const auto nodes = render_nodes_in_world(root);
  out.reserve(nodes.size());  // SceneInstance is large; one allocation (#268)
  {
    // A new car or scene brings many meshes at once: read them on the job
    // pool instead of one by one in the loop below. Lamp renders keep their
    // lazy load, as they are only drawn while lit.
    std::vector<uint32_t> missing;
    for (const auto* n : nodes) {
      if ((n->sky_render && n->key != g_sky) || n->light_flags || n->line_geometry ||
          (n->deformed_geometry && n->deformed_type == n->type_id) || osd_rectangle(n))
        continue;
      const auto id = static_cast<uint32_t>(n->type_id);
      if (!g_meshes.contains(id) && !g_overrides.contains(id)) missing.push_back(id);
    }
    if (missing.size() > 1) {
      std::sort(missing.begin(), missing.end());
      missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
      preload_meshes(missing);
    }
  }
  for (auto *n : nodes) {
    if (osd_rectangle(n)) continue;
    // Source scripts replace Scene.skydome; a superseded handle may remain
    // alive until script cleanup. Never render it as ordinary foreground.
    if (n->sky_render && n->key != g_sky)
      continue;
    if (n->light_flags &&
        !part_lamp_lit(n->parent, n->light_flags, root == g_world && g_preview_lamps))
      continue;
    SceneInstance instance;
    instance.lighting=lighting;
    if (!scene_instance_transform(n->key, root, instance.transform.data()))
      continue;
    if (n->line_geometry) {
      if (n->line_geometry->indices.empty()) continue;
      instance.geometry = n->line_geometry;
    } else {
      instance.geometry = n->deformed_geometry && n->deformed_type == n->type_id
          ? n->deformed_geometry : geometry(static_cast<uint32_t>(n->type_id));
    }
    if (!instance.geometry)
      continue;
    // Pedestrians share the bind mesh. The bone palette is a uniform, so the
    // vertex buffer stays put while the clip advances.
    if (!n->line_geometry && g_resources && n->alias == "pedestrian" &&
        !instance.geometry->skin_weights.empty()) {
      // A deferred palette is filled per view, only for walkers in its frustum.
      if (defer_skin) instance.gpu_skin = true;
      else skin_palette(instance, n->key, n->type_id);
    }
    if (!instance.gpu_skin && !n->line_geometry && g_resources) {
      if (auto posed = character_posed(n->key, n->type_id, instance.geometry.get(), *g_resources))
        instance.geometry = std::move(posed);
    }
    // Legacy resource cues (flags 640, e.g. finishObject) animate independently
    // of the script pose. Host timing/opacity defaults are documented in
    // docs/scripting/runtime.md; original per-bit semantics are not established.
    uint32_t resource_flags = 0;
    scene_shadow_info(static_cast<uint32_t>(n->type_id), &resource_flags, nullptr);
    if (root == g_world && n->kind == GameRefKind::RenderRef && !n->vehicle_material &&
        !part_render_owner(n->key) && (resource_flags & 0x280u) == 0x280u) {
      const float t = gameref_now();
      const float c = std::cos(t), s = std::sin(t);
      auto& m = instance.transform;
      for (int row = 0; row < 3; ++row) {
        const float x = m[row], z = m[8 + row];
        m[row] = c * x - s * z;
        m[8 + row] = s * x + c * z;
      }
      m[13] += .5f * std::sin(2.f * t);
      instance.opacity = .65f;
    }
    instance.cull_backfaces = instance.geometry->ground_polygons;
    instance.key = n->key;
    instance.type_id = n->type_id;
    instance.sky = n->key == g_sky;
    instance.color = n->color;
    instance.vehicle_material = n->vehicle_material;
    instance.light_flags = n->light_flags;
    if (n->paint_source || !n->texture_replacements.empty() || !n->texture_file_replacements.empty()) {
      // Match paths without the extension (texture_key); the mesh side is
      // cached per mesh (#268).
      const auto& keys = mesh_texture_keys(instance.geometry);
      const auto paint = texture_key(scene_instances_texture(n->paint_source));
      if (!paint.empty()) {
        for (size_t i = 0; i < keys.size(); ++i) {
          if (keys[i] == paint) {
            instance.paint_slot = static_cast<int>(i);
            break;
          }
        }
      }
      for (const auto& [from, to] : n->texture_replacements) {
        const auto source = texture_key(scene_instances_texture(from));
        if (source.empty()) continue;
        for (size_t i = 0; i < keys.size(); ++i)
          if (source == keys[i])
            instance.texture_replacements.emplace_back(i, scene_instances_texture(to));
      }
      for (const auto& [from, path] : n->texture_file_replacements) {
        const auto source = texture_key(scene_instances_texture(from));
        if (source.empty()) continue;
        for (size_t i = 0; i < keys.size(); ++i)
          if (source == keys[i])
            instance.texture_replacements.emplace_back(i, path);
      }
      if (n->paint_texture)
        instance.paint_texture = scene_instances_texture(n->paint_texture);
    }
    out.push_back(std::move(instance));
  }
  return out;
}
}  // namespace

std::vector<SceneInstance> scene_instances_collect_root(void *root) {
  return collect_root(root, false);
}
void* scene_instances_world() { return g_world; }

namespace {
bool g_frame_cache = false;
bool g_frame_world_valid = false;
void* g_frame_world_root = nullptr;
std::vector<SceneInstance> g_frame_world;
// Palettes (or CPU poses) already built this frame, shared by every view.
struct FrameCharacter {
  std::vector<float> skin;
  std::shared_ptr<const ScxGeometry> geometry;
  bool gpu_skin = false;
  bool pending = false;  // reserved by a palette batch, not built yet (#268)
};
std::unordered_map<void*, FrameCharacter> g_frame_characters;

// Outside a frame scope the caller's `local` receives a fresh collection.
const std::vector<SceneInstance>& world_instances(std::vector<SceneInstance>& local) {
  if (!g_frame_cache) {
    local = scene_instances_collect_root(g_world);
    return local;
  }
  if (!g_frame_world_valid || g_frame_world_root != g_world) {
    perf::Scope scope(perf::Zone::WorldList);
    g_frame_world = collect_root(g_world, true);
    g_frame_world_root = g_world;
    g_frame_world_valid = true;
  }
  return g_frame_world;
}
// A walker's bind mesh with a metre of slack for the swinging limbs.
bool character_in_view(const SceneInstance& instance, const SceneFrustum& frustum) {
  const auto& info = instance.geometry->info;
  const float scale = instance.geometry->position_scale > 0.f
      ? instance.geometry->position_scale : 0.01f;
  float local[3], half[3];
  for (int axis = 0; axis < 3; ++axis) {
    local[axis] = (info.bbox_min[axis] + info.bbox_max[axis]) * .5f * scale;
    half[axis] = (info.bbox_max[axis] - info.bbox_min[axis]) * .5f * scale;
  }
  local[2] = -local[2];
  const auto& m = instance.transform;
  float centre[3];
  for (int axis = 0; axis < 3; ++axis)
    centre[axis] = m[axis] * local[0] + m[4 + axis] * local[1] + m[8 + axis] * local[2] +
                   m[12 + axis];
  const float radius = std::hypot(half[0], half[1], half[2]) + 1.f;
  for (const auto& plane : frustum)
    if (plane[0] * centre[0] + plane[1] * centre[1] + plane[2] * centre[2] + plane[3] < -radius)
      return false;
  return true;
}

void complete_skin(SceneInstance& instance) {
  auto [found, fresh] = g_frame_characters.try_emplace(instance.key);
  auto& cached = found->second;
  if (fresh) {
    if (!skin_palette(instance, instance.key, instance.type_id))
      if (auto posed = character_posed(instance.key, instance.type_id,
                                       instance.geometry.get(), *g_resources))
        instance.geometry = std::move(posed);
    cached = {instance.skin, instance.geometry, instance.gpu_skin};
    return;
  }
  instance.skin = cached.skin;
  instance.geometry = cached.geometry;
  instance.gpu_skin = cached.gpu_skin;
}
}  // namespace

void scene_instances_frame_cache(bool enabled) {
  g_frame_cache = enabled;
  g_frame_world_valid = false;
  g_frame_characters.clear();
  if (!enabled) {
    g_frame_world.clear();
    g_frame_world_root = nullptr;
  }
}

void scene_instances_set_size_filter(SceneSizeFilter test, void* user) {
  g_size_filter = test;
  g_size_filter_user = user;
}

void scene_instances_set_lod_view(const ScenePropLodView& view) {
  g_lod_view = view;
  if (view.update) ++g_lod_epoch;
}

int scene_prop_lod_level(int current, float size, const std::vector<float>& thresholds,
                         float hysteresis) {
  const int count = static_cast<int>(thresholds.size());
  if (!std::isfinite(size)) return count;
  if (current < 0 || current > count) {
    int level = 0;
    while (level < count && size >= thresholds[static_cast<size_t>(level)]) ++level;
    return level;
  }
  const float h = std::max(hysteresis, 1.f);
  int level = current;
  while (level < count && size >= thresholds[static_cast<size_t>(level)] * h) ++level;
  while (level > 0 && size < thresholds[static_cast<size_t>(level - 1)] / h) --level;
  return level;
}

size_t scene_instances_lod_placements() {
  size_t count = 0;
  for (const auto& [id, map] : g_ground_props) count += map.lods.size();
  return count;
}

namespace {
std::shared_ptr<const ScxGeometry> signal_lens(int state) {
  static std::array<std::shared_ptr<const ScxGeometry>, 3> lenses;
  if (state < 0 || state > 2)
    return {};
  if (lenses[state])
    return lenses[state];
  auto geo = std::make_shared<ScxGeometry>();
  geo->info.ok = true;
  geo->info.version = 4;
  geo->position_scale = 1;
  float y = state == 0 ? .32f : state == 1 ? -.005f : -.335f;
  geo->positions = {1, y, .17f};
  geo->normals = {0, 0, 1};
  geo->uvs = {.5f, .5f};
  for (int i = 0; i <= 16; ++i) {
    float a = 6.2831853f * i / 16;
    geo->positions.insert(geo->positions.end(), {1 + .11f * std::cos(a),
                                                 y + .11f * std::sin(a), .17f});
    geo->normals.insert(geo->normals.end(), {0, 0, 1});
    geo->uvs.insert(geo->uvs.end(),
                    {.5f + .5f * std::cos(a), .5f + .5f * std::sin(a)});
    if (i)
      geo->indices.insert(geo->indices.end(),
                          {0u, uint32_t(i), uint32_t(i + 1)});
  }
  ScxGroup group;
  group.index_count = uint32_t(geo->indices.size());
  group.diffuse[0] = group.diffuse[1] = group.diffuse[2] = 0;
  group.emission_color[0] = state == 0 ? 1 : state == 1 ? 1 : .05f;
  group.emission_color[1] = state == 0 ? .01f : state == 1 ? .65f : 1;
  group.emission_color[2] = state == 2 ? .03f : 0;
  geo->groups.push_back(group);
  geo->info.bbox_min[0] = .89f;
  geo->info.bbox_max[0] = 1.11f;
  geo->info.bbox_min[1] = y - .11f;
  geo->info.bbox_max[1] = y + .11f;
  geo->info.bbox_min[2] = geo->info.bbox_max[2] = .17f;
  return lenses[state] = geo;
}
std::shared_ptr<const ScxGeometry> signal_geometry(GroundPropMap &map) {
  const auto stamp = race_signal_revision();
  if (map.signal_stamp == stamp && map.signal_geometry)
    return map.signal_geometry;
  std::array<ScxGeometry, 3> colors;
  for (const auto &entry : map.cells)
    for (const auto &prop : entry.second.props)
      if (prop.signal) {
        int state = race_signal_at(prop.signal_position);
        auto lens = signal_lens(state);
        if (!lens)
          continue;
        auto &color = colors[state];
        uint32_t offset = uint32_t(color.positions.size() / 3);
        for (size_t i = 0; i < lens->positions.size(); i += 3) {
          auto point = transform_point(prop.pose, {lens->positions[i],
                                                   lens->positions[i + 1],
                                                   lens->positions[i + 2]});
          color.positions.insert(color.positions.end(), point.begin(),
                                 point.end());
          color.normals.insert(color.normals.end(),
                               {prop.pose[8], prop.pose[9], prop.pose[10]});
        }
        color.uvs.insert(color.uvs.end(), lens->uvs.begin(), lens->uvs.end());
        for (auto index : lens->indices)
          color.indices.push_back(offset + index);
      }
  auto merged = std::make_shared<ScxGeometry>();
  merged->info.ok = true;
  merged->info.version = 4;
  merged->position_scale = 1;
  for (int state = 0; state < 3; ++state) {
    auto &color = colors[state];
    if (color.indices.empty())
      continue;
    auto lens = signal_lens(state);
    ScxGroup group = lens->groups.front();
    group.name = "traffic signal";
    group.index_start = uint32_t(merged->indices.size());
    group.index_count = uint32_t(color.indices.size());
    uint32_t offset = uint32_t(merged->positions.size() / 3);
    merged->positions.insert(merged->positions.end(), color.positions.begin(),
                             color.positions.end());
    merged->normals.insert(merged->normals.end(), color.normals.begin(),
                           color.normals.end());
    merged->uvs.insert(merged->uvs.end(), color.uvs.begin(), color.uvs.end());
    for (auto index : color.indices)
      merged->indices.push_back(offset + index);
    merged->groups.push_back(group);
  }
  for (size_t i = 0; i < merged->positions.size(); i += 3)
    for (int axis = 0; axis < 3; ++axis) {
      float value = merged->positions[i + axis];
      if (i == 0)
        merged->info.bbox_min[axis] = merged->info.bbox_max[axis] = value;
      else {
        merged->info.bbox_min[axis] =
            std::min(merged->info.bbox_min[axis], value);
        merged->info.bbox_max[axis] =
            std::max(merged->info.bbox_max[axis], value);
      }
    }
  map.signal_stamp = stamp;
  return map.signal_geometry = merged;
}
bool night_props_visible() { return scene_sun_is_night(); }

// Brings the map's world cache to `shell` (#260). Ground shells do not move,
// so this runs once per shell instead of a matrix product per placement and
// frame.
void refresh_world_cache(GroundPropMap& map, const PartMatrix& shell) {
  if (map.world_cache_valid && map.cached_shell == shell) return;
  for (auto& [key, cell] : map.cells) {
    cell.world_center = transform_point(shell, cell.center);
    for (auto& prop : cell.props) {
      prop.world_center = transform_point(shell, prop.center);
      prop.render_matrix = part_matrix_multiply(shell, prop.pose);
      for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
          if ((col == 2) != (row == 2)) prop.render_matrix[col * 4 + row] *= -1.f;
    }
  }
  map.cached_shell = shell;
  map.world_cache_valid = true;
}

// Frustum test of a script-space sphere (planes are render space).
bool sphere_visible(const SceneFrustum* frustum, const std::array<float, 3>& world, float radius) {
  if (!frustum) return true;
  for (const auto& plane : *frustum)
    if (plane[0] * world[0] + plane[1] * world[1] - plane[2] * world[2] + plane[3] < -radius)
      return false;
  return true;
}

// Whether `prop` draws at its placement's current LOD (#252).
// The batch loop calls this from several jobs at once (#266). A placement's
// level and epoch are read and written through atomic_ref: two jobs may meet
// the same placement (both chain members), and scene_prop_lod_level is
// idempotent (the level it returns maps to itself for the same size), so a
// job that reads a level another job has just stored still gets the same
// answer, and the frame's choices match a serial loop's.
bool prop_lod_visible(GroundPropMap& map, const GroundProp& prop, const PartMatrix& shell) {
  if (prop.lod_min < 0 || prop.lod_index < 0 ||
      static_cast<size_t>(prop.lod_index) >= map.lods.size())
    return true;
  auto& entry = map.lods[static_cast<size_t>(prop.lod_index)];
  int level = static_cast<int>(entry.sizes.size());
  if (g_lod_view.valid && !g_lod_view.force_highest) {
    std::atomic_ref<int> stored_level(entry.level);
    std::atomic_ref<uint64_t> stored_epoch(entry.epoch);
    const uint64_t epoch = stored_epoch.load(std::memory_order_acquire);
    const int previous = stored_level.load(std::memory_order_relaxed);
    if (previous >= 0 && (!g_lod_view.update || epoch == g_lod_epoch)) {
      // Chosen this frame (the other chain member, the other collection
      // path) or a non-updating pass reusing the main view's choice.
      level = previous;
    } else {
      const auto world = transform_point(shell, entry.center);
      const float dx = world[0] - g_lod_view.eye[0], dy = world[1] - g_lod_view.eye[1],
                  dz = world[2] - g_lod_view.eye[2];
      const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) *
                             std::clamp(g_lod_view.bias, .1f, 10.f);
      level = scene_prop_lod_level(previous, entry.radius / std::max(distance, .01f),
                                   entry.sizes);
      if (g_lod_view.update) {
        stored_level.store(level, std::memory_order_relaxed);
        stored_epoch.store(g_lod_epoch, std::memory_order_release);
      }
    }
  }
  return level >= prop.lod_min && level <= prop.lod_max;
}
}

bool scene_instances_has_ground_props() {
  const auto* world = gameref_get(g_world);
  if (!world || g_ground_props.empty()) return false;
  for (auto* child : world->children)
    if (const auto* shell = gameref_get(child);
        shell && g_ground_props.contains(static_cast<uint32_t>(shell->type_id)))
      return true;
  return false;
}

// Fewer visible cells than this keep the batch loop on the host thread (#266).
constexpr size_t kParallelCellMin = 16;
constexpr size_t kParallelThreadMin = 4;
// Fewer new walker palettes than this are built on the host thread (#268).
constexpr size_t kParallelSkinMin = 8;
// Placements below which collect_batches merges on the calling thread (#359).
constexpr size_t kParallelMergeMin = 8192;

std::vector<SceneInstance> scene_instances_collect(
    const SceneFrustum* frustum,
    std::vector<std::shared_ptr<const ScxGeometry>>* resident_props,
    bool complete_character_palettes, std::vector<SceneBatch>* night_batches) {
  std::vector<SceneInstance> out;
  // Night layout (#264): entries keep their instance vectors' capacity.
  size_t night_total = 0;
  if (night_batches)
    for (auto& batch : *night_batches) batch.instances.clear();
  // Copying the frame's world list measured at ~0 in the harness (#260):
  // taking it instead would make later collections (reflections, shadows)
  // rebuild it, so it is copied.
  if (const auto& world = world_instances(out); &world != &out) {
    perf::Scope scope(perf::Zone::WorldCopy);
    out = world;
  }
  // Walkers outside this view keep an empty palette; the renderer skips them.
  if (complete_character_palettes) {
    perf::Scope skin_scope(perf::Zone::CollectSkins);
    // Palettes new this frame are built on the job pool (#268): the host
    // thread finds or loads each character first, every job then rebuilds a
    // different character into its own instance, and the host thread
    // publishes them in order. Other views copy the frame's palette.
    struct Fresh {
      SceneInstance* instance;
      void* prepared;
      bool skinned = false;
    };
    static std::vector<Fresh> fresh;  // host thread only, capacity kept
    fresh.clear();
    std::vector<SceneInstance*> repeats;
    for (auto& instance : out) {
      if (!instance.gpu_skin || !instance.skin.empty() ||
          (frustum && !character_in_view(instance, *frustum)))
        continue;
      auto [found, added] = g_frame_characters.try_emplace(instance.key);
      if (!added) {
        if (found->second.pending) repeats.push_back(&instance);
        else {
          instance.skin = found->second.skin;
          instance.geometry = found->second.geometry;
          instance.gpu_skin = found->second.gpu_skin;
        }
        continue;
      }
      found->second.pending = true;
      fresh.push_back({&instance, character_skin_prepare(instance.key, instance.type_id,
                                                         *g_resources)});
    }
    const auto build = [&](size_t i) {
      fresh[i].skinned = skin_palette_prepared(*fresh[i].instance, fresh[i].prepared);
    };
    if (fresh.size() >= kParallelSkinMin && jobs_concurrency() >= kParallelThreadMin)
      jobs_parallel_for(fresh.size(), build);
    else
      for (size_t i = 0; i < fresh.size(); ++i) build(i);
    for (auto& item : fresh) {
      auto& instance = *item.instance;
      if (!item.skinned)
        if (auto posed = character_posed(instance.key, instance.type_id,
                                         instance.geometry.get(), *g_resources))
          instance.geometry = std::move(posed);
      g_frame_characters[instance.key] = {instance.skin, instance.geometry, instance.gpu_skin};
    }
    for (auto* instance : repeats) complete_skin(*instance);
  }
  if (g_ground_props.empty()) {
    if (night_batches) night_batches->clear();
    return out;
  }
  const size_t base_count = out.size();
  for (size_t i = 0; i < base_count; ++i) {
    const auto found = g_ground_props.find(static_cast<uint32_t>(out[i].type_id));
    if (found == g_ground_props.end()) continue;
    PartMatrix shell;
    if (!scene_instances_script_matrix(out[i].key, shell.data())) continue;
    if (resident_props)
      for (const auto id : found->second.resources)
        if (auto mesh = geometry(id)) resident_props->push_back(std::move(mesh));
    refresh_world_cache(found->second, shell);
    const SceneSun lighting = out[i].lighting;
    const size_t night_offset = night_total;
    if (night_batches) {
      const auto& meshes = found->second.night_geometry;
      night_total += meshes.size();
      if (night_batches->size() < night_total) night_batches->resize(night_total);
      for (size_t slot = 0; slot < meshes.size(); ++slot) {
        auto& batch = (*night_batches)[night_offset + slot];
        if (batch.geometry != meshes[slot]) batch.geometry = meshes[slot];
        batch.lighting = lighting;
      }
    }
    auto glow = signal_geometry(found->second);
    if (glow && !glow->indices.empty()) {
      SceneInstance instance;
      instance.geometry = std::move(glow);
      instance.emissive_overlay = true;
      instance.lighting = lighting;
      instance.transform = shell;
      for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
          if ((col == 2) != (row == 2))
            instance.transform[col * 4 + row] *= -1;
      out.push_back(std::move(instance));
    }
    for (const auto& [key, cell] : found->second.cells) {
        if (!sphere_visible(frustum, cell.world_center, cell.radius)) continue;
        if (g_size_filter &&
            !g_size_filter(cell.world_center.data(), cell.radius, g_size_filter_user)) continue;
        for (const auto& prop : cell.props) {
          if (prop.signal || prop.batched || prop.hidden || (prop.night_only && !night_props_visible())) continue;
          if (!prop_lod_visible(found->second, prop, shell)) continue;
          if (!sphere_visible(frustum, prop.world_center, prop.radius)) continue;
          if (g_size_filter &&
              !g_size_filter(prop.world_center.data(), prop.radius, g_size_filter_user)) continue;
          if (night_batches && prop.night_only && prop.night_slot >= 0) {
            auto& batch = (*night_batches)[night_offset + static_cast<size_t>(prop.night_slot)];
            if (batch.geometry) batch.instances.push_back({prop.render_matrix, prop.color});
            continue;
          }
          auto mesh = geometry(prop.render_id);
          if (!mesh) continue;
          SceneInstance instance;
          instance.cull_backfaces = true;
          instance.ground_prop = true;
          instance.type_id = static_cast<int32_t>(prop.render_id);
          instance.color = prop.color;
          instance.emissive_overlay = prop.night_only;
          instance.lighting = lighting;
          instance.geometry = std::move(mesh);
          instance.transform = prop.render_matrix;
          out.push_back(std::move(instance));
        }
      }
  }
  if (night_batches) night_batches->resize(night_total);
  return out;
}


void scene_instances_collect_batches(const SceneFrustum* frustum,
                                     std::vector<SceneBatch>& out,
                                     const float* reflection_origin) {
  if (g_ground_props.empty()) {
    out.clear();
    return;
  }
  std::vector<SceneInstance> local;
  // Persistent layout (#260): one entry per batchable mesh and shell, in
  // shell order, kept in `out` across frames (its instance vectors keep their
  // capacity). Entries may be empty; consumers skip empty batches.
  struct Shell {
    GroundPropMap* map;
    PartMatrix shell;
    const SceneInstance* base;
    size_t offset;
  };
  std::vector<Shell> shells;
  size_t total = 0;
  for (const auto& base : world_instances(local)) {
    const auto found = g_ground_props.find(static_cast<uint32_t>(base.type_id));
    if (found == g_ground_props.end()) continue;
    PartMatrix shell;
    if (!scene_instances_script_matrix(base.key, shell.data())) continue;
    refresh_world_cache(found->second, shell);
    shells.push_back({&found->second, shell, &base, total});
    total += found->second.batch_ids.size();
  }
  bool same_layout = out.size() == total;
  for (size_t s = 0; same_layout && s < shells.size(); ++s)
    for (size_t slot = 0; slot < shells[s].map->batch_geometry.size(); ++slot)
      if (out[shells[s].offset + slot].geometry != shells[s].map->batch_geometry[slot]) {
        same_layout = false;
        break;
      }
  if (!same_layout) {
    out.resize(total);
    for (const auto& entry : shells)
      for (size_t slot = 0; slot < entry.map->batch_geometry.size(); ++slot)
        out[entry.offset + slot].geometry = entry.map->batch_geometry[slot];
  }
  for (const auto& entry : shells)
    for (size_t slot = 0; slot < entry.map->batch_geometry.size(); ++slot) {
      auto& draw = out[entry.offset + slot];
      draw.instances.clear();
      draw.lighting = entry.base->lighting;
    }
  // Visible cells in serial order (#266). The placement loop runs on the
  // job pool over contiguous runs of this list, and the runs are appended in
  // order, so every batch lists its placements as the serial loop did.
  struct VisibleCell {
    const Shell* shell;
    const GroundPropCell* cell;
  };
  static std::vector<VisibleCell> cells;  // host thread only, capacity kept
  cells.clear();
  for (const auto& entry : shells)
    for (const auto& [key, cell] : entry.map->cells) {
      (void)key;
      if (cell.batches.empty() || !sphere_visible(frustum, cell.world_center, cell.radius)) continue;
      if (g_size_filter &&
          !g_size_filter(cell.world_center.data(), cell.radius, g_size_filter_user)) continue;
      cells.push_back({&entry, &cell});
    }
  if (cells.empty()) return;
  const bool night = night_props_visible();
  // A few runs per thread balance uneven cells. Tiny lists, and machines with
  // fewer than kParallelThreadMin threads (the bgfx render thread and the
  // walker stepper need theirs; on two cores the pool was slower), stay on
  // this thread.
  const size_t threads = jobs_concurrency();
  const size_t runs = cells.size() < kParallelCellMin || threads < kParallelThreadMin
      ? 1 : std::min(cells.size(), threads * 4);
  const auto run_cells = [&](size_t run) {
    return std::pair<size_t, size_t>{cells.size() * run / runs, cells.size() * (run + 1) / runs};
  };
  // Placements past the per-placement flags, as the serial loop tested them.
  const auto each_candidate = [&](const VisibleCell& visible, auto&& visit) {
    const auto& map = *visible.shell->map;
    for (const auto& batch : visible.cell->batches) {
      if (batch.slot >= map.batch_geometry.size()) continue;
      const auto& draw = out[visible.shell->offset + batch.slot];
      if (!draw.geometry) continue;
      for (const auto index : batch.indices) {
        if (index >= visible.cell->props.size()) continue;
        const auto& prop = visible.cell->props[index];
        if (prop.hidden || (prop.night_only && !night)) continue;
        visit(batch, draw, prop);
      }
    }
  };
  struct Hit {
    uint32_t slot;
    uint32_t pos;  // index in its batch, assigned in serial order below
    const GroundProp* prop;
  };
  static std::vector<std::vector<Hit>> hit_runs;
  if (hit_runs.size() < runs) hit_runs.resize(runs);
  jobs_parallel_for(runs, [&](size_t run) {
    auto& list = hit_runs[run];
    list.clear();
    const auto [first, last] = run_cells(run);
    for (size_t i = first; i < last; ++i) {
      const auto& visible = cells[i];
      const SceneBatch* extent_of = nullptr;
      float extent = 0.f;
      each_candidate(visible, [&](const GroundPropBatch& batch, const SceneBatch& draw,
                                  const GroundProp& prop) {
        if (reflection_origin && extent_of != &draw) {
          extent_of = &draw;
          extent = 0.f;
          const auto& mesh = *draw.geometry;
          for (int axis = 0; axis < 3; ++axis)
            extent = std::max(extent, (mesh.info.bbox_max[axis] - mesh.info.bbox_min[axis]) *
                                          mesh.position_scale);
        }
        // LOD first (#257): the non-selected chain member costs one cached
        // lookup instead of a frustum test.
        if (!prop_lod_visible(*visible.shell->map, prop, visible.shell->shell) ||
            !sphere_visible(frustum, prop.world_center, prop.radius)) return;
        if (g_size_filter &&
            !g_size_filter(prop.world_center.data(), prop.radius, g_size_filter_user)) return;
        const auto& matrix = prop.render_matrix;
        if (reflection_origin && extent > 0.f) {
          const float dx = matrix[12] - reflection_origin[0];
          const float dy = matrix[13] - reflection_origin[1];
          const float dz = matrix[14] - reflection_origin[2];
          const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
          if (dist > 12.f && extent / dist * 128.f < 4.f) return;
        }
        list.push_back({static_cast<uint32_t>(visible.shell->offset + batch.slot), 0, &prop});
      });
    }
  });
  // Merge. A few thousand placements (the main view, about 3,000 in the
  // city; reflection faces, about 200) are cheapest copied in order on this
  // thread. The outer sun-cascade frustum lists about 21,000: there (#359)
  // every hit is numbered within its batch in run order, each batch is sized
  // once and the copies run on the pool. Either way each batch lists its
  // placements in exactly the serial order.
  size_t hit_total = 0;
  for (size_t run = 0; run < runs; ++run) hit_total += hit_runs[run].size();
  if (runs == 1 || hit_total < kParallelMergeMin) {
    for (size_t run = 0; run < runs; ++run)
      for (const auto& hit : hit_runs[run])
        out[hit.slot].instances.push_back({hit.prop->render_matrix, hit.prop->color});
    return;
  }
  static std::vector<uint32_t> slot_fill;  // host thread only, capacity kept
  slot_fill.assign(out.size(), 0);
  for (size_t run = 0; run < runs; ++run)
    for (auto& hit : hit_runs[run]) hit.pos = slot_fill[hit.slot]++;
  for (size_t slot = 0; slot < out.size(); ++slot)
    if (slot_fill[slot]) out[slot].instances.resize(slot_fill[slot]);
  jobs_parallel_for(runs, [&](size_t run) {
    for (const auto& hit : hit_runs[run]) {
      auto& instance = out[hit.slot].instances[hit.pos];
      instance.transform = hit.prop->render_matrix;
      instance.color = hit.prop->color;
    }
  });
}

void scene_instances_override_mesh(uint32_t id,
                                   std::shared_ptr<const ScxGeometry> geo) {
  if (id && geo)
    g_overrides[id] = std::move(geo);
}
void scene_instances_enable_static_floor(bool enabled) {
  if (g_static_enabled == enabled) return;  // every frame: join only on a change
  part_physics_join();
  ++g_floor_revision;
  g_static_enabled = enabled;
}
void scene_instances_add_static_mesh(const ScxGeometry &geo) {
  part_physics_join();
  if (!geo.info.ok || geo.indices.size() < 3 || geo.positions.size() < 9)
    return;
  StaticMesh mesh;
  mesh.positions = geo.positions;
  mesh.indices = geo.indices;
  mesh.scale = geo.position_scale;
  index_floor(mesh);
  ++g_floor_revision;
  g_static.push_back(std::move(mesh));
}

void scene_instances_override_collision(uint32_t id,
                                        std::shared_ptr<const ScxGeometry> geo) {
  part_physics_join();
  if (id && geo) g_collision_overrides[id] = std::move(geo);
}

// The map SCM's surfaces for the collision faces (#452). The SCM is the
// 8th field of the ground native's params; its material i is the SCY's
// material i, whose texture names a surface the SCM leaves unset.
// Config.ground_friction when the ground native ran (natives_resref).
static std::atomic<float> g_ground_friction{1.f};

// The average colour (ARGB) of a ground texture, decoded once on the CPU, or 0
// when it cannot be read: the dust a tyre raises from that ground (#454).
static uint32_t ground_texture_color(const std::string &absolute) {
  static std::unordered_map<std::string, uint32_t> cache;
  if (const auto it = cache.find(absolute); it != cache.end()) return it->second;
  uint32_t colour = 0;
  PaintImage image;
  if (paint_image_from_texture(load_texture_file(absolute), &image) && image.valid()) {
    double sum[3] = {};
    double count = 0;
    const size_t pixels = size_t(image.width) * image.height;
    const size_t step = std::max<size_t>(1, pixels / 4096);
    for (size_t i = 0; i < pixels; i += step) {
      for (int k = 0; k < 3; ++k) sum[k] += image.rgba[i * 4 + k];
      ++count;
    }
    if (count > 0)
      colour = 0xff000000u | uint32_t(std::lround(sum[0] / count)) << 16 |
               uint32_t(std::lround(sum[1] / count)) << 8 | uint32_t(std::lround(sum[2] / count));
  }
  cache.emplace(absolute, colour);
  return colour;
}

static void attach_ground_surfaces(const RpkIndex &rpk, int32_t pack, const std::string &params,
                            const std::vector<GroundMat> &mats,
                            const RpkIndex::ModelRef &model, ScxGeometry &collision) {
  std::string path;
  {
    size_t field = 0, start = 0;
    for (size_t i = 0; i <= params.size(); ++i)
      if (i == params.size() || params[i] == ',') {
        if (field++ == 7) {
          path = params.substr(start, i - start);
          break;
        }
        start = i + 1;
      }
  }
  while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back()))) path.pop_back();
  while (!path.empty() && std::isspace(static_cast<unsigned char>(path.front()))) path.erase(0, 1);
  std::vector<GroundMaterialEntry> entries;
  if (!path.empty()) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::ifstream in(std::filesystem::path(rpk.game_root()) / path, std::ios::binary);
    if (in)
      entries = parse_ground_materials(
          {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});
  }
  std::vector<std::string> textures(mats.size());
  for (size_t i = 0; i < mats.size(); ++i)
    if (model.ok && mats[i].tex < model.textures.size()) textures[i] = model.textures[mats[i].tex];
  // Without an SCM the SCY material count still lets the texture names speak.
  if (entries.empty()) entries.resize(mats.size());
  collision.surfaces = ground_surfaces(entries, textures, g_ground_friction.load());
  // Loose ground: its texture's colour for the dust.
  for (size_t i = 0; i < collision.surfaces.size() && i < textures.size(); ++i)
    if (collision.surfaces[i].rolling > 0 && !textures[i].empty()) {
      std::string relative = textures[i];
      std::replace(relative.begin(), relative.end(), '\\', '/');
      collision.surfaces[i].dust_color =
          ground_texture_color((std::filesystem::path(rpk.game_root()) / relative).string());
    }
  // skid_SFX names a sound in the ground pack's dependency slots.
  for (auto &surface : collision.surfaces)
    if (surface.slide_sound)
      surface.slide_sound = (surface.slide_sound >> 16) == 0xffff
                                ? 0
                                : rpk.resolve_embedded_id(pack, surface.slide_sound);
  // skid_PS and skid_SM (#454) too, except the name fallbacks' local ids of
  // particles.rpk. 0 stays 0: the effects use the stock defaults.
  const RpkPack *particles = rpk.pack_by_name("particles.rpk");
  const auto resolve_effect = [&](uint32_t id) -> uint32_t {
    if (!id) return 0;
    if ((id & 0xffff0000u) == kParticlesLocal)
      return particles ? RpkIndex::make_full_id(particles->pack_id, id) : 0;
    return rpk.resolve_embedded_id(pack, id);
  };
  for (auto &surface : collision.surfaces) {
    surface.skid_particles = resolve_effect(surface.skid_particles);
    surface.skid_mark = resolve_effect(surface.skid_mark);
  }
  // The materials off the road, with their texture: what to check in game.
  std::string soft;
  size_t count = 0;
  for (size_t i = 0; i < collision.surfaces.size(); ++i) {
    const auto &surface = collision.surfaces[i];
    if (surface.friction == 1.f && surface.rolling == 0.f && surface.bounce == .2f &&
        !surface.mark_color && !surface.slide_sound && !surface.skid_particles &&
        !surface.skid_mark)
      continue;
    if (++count > 16) continue;
    char line[160];
    std::snprintf(line, sizeof line, " %zu:%s(%s) %.2f/%.3f", i, entries[i].name.c_str(),
                  std::filesystem::path(textures[i]).filename().string().c_str(),
                  surface.friction, surface.rolling);
    soft += line;
    if (surface.bounce != .2f || surface.mark_color || surface.slide_sound) {
      std::snprintf(line, sizeof line, "[bounce %.2f mark %08x sfx %08x]", surface.bounce,
                    surface.mark_color, surface.slide_sound);
      soft += line;
    }
    if (surface.skid_particles || surface.skid_mark) {
      std::snprintf(line, sizeof line, "[ps %08x sm %08x dust %08x]", surface.skid_particles,
                    surface.skid_mark, surface.dust_color);
      soft += line;
    }
  }
  jobs_printf("[scene] ground surfaces %s: %zu materials, %zu not road%s\n",
              path.empty() ? "(no scm)" : path.c_str(), collision.surfaces.size(), count,
              soft.c_str());
}

void scene_instances_set_ground_friction(float friction) {
  g_ground_friction.store(std::isfinite(friction) && friction > 0 ? friction : 1.f);
}

GroundMapData scene_instances_ground_map(const RpkIndex &rpk, uint32_t ground_id) {
  GroundMapData map;
  const RpkEntry *entry = rpk.resolve(ground_id);
  if (!entry || entry->typeof_ != 1 || entry->size < 8)
    return map;
  const int32_t pack = RpkIndex::pack_from_full(ground_id);
  const std::string disk = rpk.disk_path_for_pack(pack);
  std::ifstream in(disk, std::ios::binary | std::ios::ate);
  if (!in)
    return map;
  const auto sz = static_cast<std::streamoff>(in.tellg());
  if (sz <= 0)
    return map;
  in.seekg(0);
  std::vector<uint8_t> file(static_cast<size_t>(sz));
  in.read(reinterpret_cast<char *>(file.data()), sz);
  if (!in)
    return map;

  GroundWalk walk;
  walk.file = &file;
  walk_ground_node(walk, entry->offset, entry->size, 0);
  if (walk.params.size() < 3)
    return map;
  char *end = nullptr;
  const unsigned long local = std::strtoul(walk.params.c_str(), &end, 0);
  if (!end || end == walk.params.c_str() || local == 0)
    return map;
  map.render_id = rpk.resolve_embedded_id(pack, static_cast<uint32_t>(local));
  for (auto& placement : walk.placements)
    placement.type = rpk.resolve_embedded_id(pack, placement.type);
  map.placements = std::move(walk.placements);
  if (const auto* owner = rpk.pack_by_id(pack)) {
    for (uint32_t slot : walk.exterior_slots)
      for (const auto& dep : owner->deps)
        if ((dep.id >> 16) == slot)
          if (const auto* exterior = rpk.pack_by_path(dep.path))
            map.exterior_maps.push_back(RpkIndex::make_full_id(exterior->pack_id, 1));
  }
  const RpkIndex::ModelRef model = rpk.model_for_resource(
      RpkIndex::pack_from_full(map.render_id), map.render_id & 0xffffu);
  std::filesystem::path table;
  if (model.ok && !model.source.empty()) {
    std::string source = model.source;
    std::replace(source.begin(), source.end(), '\\', '/');
    table = std::filesystem::path(rpk.game_root()) / source;
  }
  const std::vector<GroundMat> mats = read_ground_materials(table);
  if (!walk.collision.indices.empty()) {
    walk.collision.position_scale = 1.f;
    attach_ground_surfaces(rpk, pack, walk.params_line, mats, model, walk.collision);
    map.collision = std::make_shared<ScxGeometry>(std::move(walk.collision));
  }
  if (walk.poly.empty())
    return map;

  auto geo = std::make_shared<ScxGeometry>();
  geo->ground_polygons = true;
  geo->position_scale = 1.f;
  geo->info.ok = true;
  geo->info.version = 3;
  geo->info.path = table.string();
  if (model.ok) {
    geo->textures = model.textures;
    for (auto &texture : geo->textures) {
      std::replace(texture.begin(), texture.end(), '\\', '/');
      if (!texture.empty() && std::filesystem::path(texture).is_relative())
        texture = (std::filesystem::path(rpk.game_root()) / texture).string();
    }
  }
  // The city SCY tables use this slot for black-backed window emission.
  // Multiplying the façade by either atlas erases the unlit wall.
  std::vector<bool> emission_slots(geo->textures.size());
  for (size_t i = 0; i < geo->textures.size(); ++i) {
    auto name = std::filesystem::path(geo->textures[i]).stem().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    emission_slots[i] = name == "f_light" || name == "n_light";
  }
  for (const PolySection &sec : walk.poly) {
    const bool known = sec.material < mats.size();
    ScxGroup group;
    group.index_start = static_cast<uint32_t>(geo->indices.size());
    group.index_count = static_cast<uint32_t>(sec.indices.size());
    group.texture_index = known ? mats[sec.material].tex : 0xffff;
    if (known) {
      group.base_map.channel = mats[sec.material].tex_channel;
      group.mix_index = mats[sec.material].mix;
      group.mix_map.channel = mats[sec.material].mix_channel;
    }
    const int illumination = known ? mats[sec.material].lm : -1;
    const bool has_illumination = illumination >= 0 &&
        static_cast<size_t>(illumination) < emission_slots.size();
    group.mix_vertex_color = has_illumination && emission_slots[illumination];
    if (has_illumination && !group.mix_vertex_color) {
      group.lightmap_index = static_cast<uint16_t>(illumination);
      group.lightmap_map.channel = 1;
    }
    if (known && ((mats[sec.material].flags & 1u) ||
                  (mats[sec.material].flags & 0x20000000u)))
      group.material_flags = 1;
    if (known) group.material_flags |= mats[sec.material].flags & 0x02000000u;
    const uint32_t base = static_cast<uint32_t>(geo->positions.size() / 3);
    geo->positions.insert(geo->positions.end(), sec.pos.begin(), sec.pos.end());
    geo->normals.insert(geo->normals.end(), sec.nrm.begin(), sec.nrm.end());
    geo->uvs.insert(geo->uvs.end(), sec.uv0.begin(), sec.uv0.end());
    geo->uv2.insert(geo->uv2.end(), sec.uv1.begin(), sec.uv1.end());
    geo->uv3.insert(geo->uv3.end(), sec.uv2.begin(), sec.uv2.end());
    geo->vertex_colors.insert(geo->vertex_colors.end(), sec.colors.begin(), sec.colors.end());
    for (uint32_t index : sec.indices)
      geo->indices.push_back(base + index);
    geo->groups.push_back(std::move(group));
    geo->info.vertices += static_cast<int32_t>(sec.pos.size() / 3);
    geo->info.triangles += static_cast<int32_t>(sec.indices.size() / 3);
  }
  geo->info.groups = static_cast<int32_t>(geo->groups.size());
  if (geo->indices.empty())
    return map;
  // POLY positions are already in script metres. The SCY is only a material
  // table, so no SCX decoder supplies bounds for this assembled map mesh.
  // Shadow cascades must cull the geometry, not a zero-size box at its origin.
  for (int axis = 0; axis < 3; ++axis) {
    geo->info.bbox_min[axis] = geo->info.bbox_max[axis] = geo->positions[axis];
    for (size_t i = 3 + axis; i < geo->positions.size(); i += 3) {
      geo->info.bbox_min[axis] = std::min(geo->info.bbox_min[axis], geo->positions[i]);
      geo->info.bbox_max[axis] = std::max(geo->info.bbox_max[axis], geo->positions[i]);
    }
  }
  jobs_printf("[scene] ground poly 0x%x tris=%d materials=%zu\n", map.render_id,
              geo->info.triangles, mats.size());
  map.poly = std::move(geo);
  return map;
}

namespace {
bool ground_prop_config(const RpkIndex& rpk, uint32_t type, PartConfig& out) {
  std::string subtype;
  std::string cfg = rpk.native_config(type, &subtype);
  if (cfg.empty()) return false;
  if (subtype != "object" && subtype != "trlight" && subtype != "building") return false;
  // Several stock lane supports have a malformed yaw token on their second render.
  constexpr std::string_view joined_yaw = "-1.570.000000";
  for (size_t pos = cfg.find(joined_yaw); pos != std::string::npos;
       pos = cfg.find(joined_yaw, pos + 6))
    cfg.replace(pos, joined_yaw.size(), "-1.570");
  out = parse_part_config(cfg, &rpk, RpkIndex::pack_from_full(type));
  out.resource = type;
  out.native_kind = subtype;
  return true;
}

// Reads files only, so map loading builds several types on the job pool.
NativeCollisionData build_native_collision(const RpkIndex& rpk, const PartConfig& parsed) {
  NativeCollisionData data;
  auto mesh = std::make_shared<ScxGeometry>();
  mesh->position_scale = 1.f;
  mesh->info.ok = true;
  bool bounds = false;
  const auto append = [&](const ScxGeometry& source, const PartMatrix& frame) {
    const auto first = static_cast<uint32_t>(mesh->positions.size() / 3);
    for (size_t i = 0; i + 2 < source.positions.size(); i += 3) {
      for (int axis = 0; axis < 3; ++axis) {
        const float value = frame[12 + axis] + source.position_scale *
            (frame[axis] * source.positions[i] +
             frame[4 + axis] * source.positions[i + 1] +
             frame[8 + axis] * source.positions[i + 2]);
        mesh->positions.push_back(value);
        if (!bounds) {
          mesh->info.bbox_min[axis] = mesh->info.bbox_max[axis] = value;
        } else {
          mesh->info.bbox_min[axis] = std::min(mesh->info.bbox_min[axis], value);
          mesh->info.bbox_max[axis] = std::max(mesh->info.bbox_max[axis], value);
        }
      }
      bounds = true;
    }
    for (uint32_t index : source.indices) mesh->indices.push_back(first + index);
  };
  PartMatrix identity_frame{};
  identity_frame[0] = identity_frame[5] = identity_frame[10] = identity_frame[15] = 1.f;
  bool positive_mass = false, zero_mass = false;
  for (const auto& body : parsed.bodies) {
    if (!body.collision) continue;
    if (body.mass > 0.f) {
      positive_mass = true;
      data.mass += body.mass;
      for (int axis = 0; axis < 3; ++axis)
        data.center[axis] += body.pose.position[axis] * body.mass;
    } else {
      zero_mass = true;
    }
  }
  if (data.mass > 0.f)
    for (auto& value : data.center) value /= data.mass;
  // Promote only an entirely positive-mass placement. A mixed placement keeps
  // every body static instead of moving one while baking another. A dynamic
  // type still builds its geometry: map loading uses its bounds to find the
  // dormant placement, and only static placements enter the ground caches.
  data.dynamic = positive_mass && !zero_mass;
  for (const auto& body : parsed.bodies) {
    if (!body.collision) continue;
    const auto frame = part_pose_matrix(body.pose);
    if (body.shape == PartBodyShape::Box) {
      data.primitives.push_back(body);
      append(collision::make_box_mesh(
                 frame, {body.box_half[0], body.box_half[1], body.box_half[2]}),
             identity_frame);
      continue;
    }
    if (body.shape == PartBodyShape::Sphere) {
      data.primitives.push_back(body);
      append(collision::make_sphere_mesh(frame, body.sphere_radius, 12, 6),
             identity_frame);
      continue;
    }
    if (body.shape != PartBodyShape::Mesh || body.collision_mesh.empty()) continue;
    std::string relative = body.collision_mesh;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const auto source = load_scx_geometry(
        (std::filesystem::path(rpk.game_root()) / relative).string());
    if (!source.info.ok || source.indices.empty()) continue;
    append(source, frame);
  }
  mesh->info.vertices = static_cast<int32_t>(mesh->positions.size() / 3);
  mesh->info.triangles = static_cast<int32_t>(mesh->indices.size() / 3);
  if (mesh->info.triangles) data.mesh = std::move(mesh);
  return data;
}

const NativeCollisionData& native_collision_data(
    const RpkIndex& rpk, uint32_t type, const PartConfig* parsed) {
  static const NativeCollisionData empty;
  if (!type) return empty;
  auto [cached, added] = g_native_collision.try_emplace(type);
  if (!added) return cached->second;
  PartConfig loaded;
  if (!parsed) {
    if (!ground_prop_config(rpk, type, loaded)) return cached->second;
    parsed = &loaded;
  }
  cached->second = build_native_collision(rpk, *parsed);
  return cached->second;
}

bool ground_prop_is_mesh(const RpkIndex& rpk, uint32_t resource) {
  const auto* entry = rpk.resolve(resource);
  return entry && (entry->typeof_ == 14 || entry->typeof_ == 5);
}

bool ground_prop_is_night(const RpkIndex& rpk, uint32_t resource) {
  const auto* entry = rpk.resolve(resource);
  if (!entry) return false;
  std::string alias = entry->alias;
  for (char& c : alias)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  constexpr std::string_view separators = "/\\ _-.";
  for (auto word = alias.find("night"); word != std::string::npos;
       word = alias.find("night", word + 5)) {
    if ((word == 0 || separators.find(alias[word - 1]) != std::string_view::npos) &&
        (word + 5 == alias.size() || separators.find(alias[word + 5]) != std::string_view::npos))
      return true;
  }
  return false;
}

bool prop_mesh_batchable(const ScxGeometry& mesh) {
  const bool v4 = mesh.info.version == 4;
  const bool v3 = mesh.info.version == 3;
  if (mesh.groups.empty() || mesh.ground_polygons || (!v4 && !v3)) return false;
  for (const auto& group : mesh.groups) {
    const bool blend = scx_surface(mesh.info.version, group).blend;
    // Blended sections are drawn after the opaque world, one by one.
    // Alpha-tested foliage, vertex-coloured walls and ColorMixTex facades
    // (fs_mesh mixes the instance colour under the texture) draw instanced.
    if (blend) return false;
  }
  return true;
}

void build_prop_batches(GroundPropCell& cell, const std::unordered_set<uint32_t>& batchable) {
  std::unordered_map<uint32_t, std::vector<uint32_t>> groups;
  for (uint32_t index = 0; index < cell.props.size(); ++index) {
    const auto id = cell.props[index].render_id;
    if (cell.props[index].signal || !batchable.contains(id)) continue;
    groups[id].push_back(index);
  }
  cell.batches.clear();
  // A single placement is batched too: collection merges the same mesh from
  // every visible cell into one instanced draw.
  for (auto& [id, indices] : groups) {
    for (const auto index : indices) cell.props[index].batched = true;
    cell.batches.push_back({id, std::move(indices)});
  }
}
}

SceneryLightList scene_instances_ground_lights(uint32_t render_id) {
  const auto found = g_ground_props.find(render_id);
  return found == g_ground_props.end() ? nullptr : found->second.lights;
}

void scene_instances_add_ground_props(const GroundMapData& map) {
  if (!g_resources || map.placements.empty() || g_ground_props.contains(map.render_id))
    return;
  part_physics_join();
  auto& props_map = g_ground_props[map.render_id];
  auto& cells = props_map.cells;
  std::unordered_map<uint32_t, PartConfig> object_types;
  std::unordered_map<uint32_t, std::shared_ptr<const PartConfig>> dormant_types;
  std::unordered_map<uint32_t, bool> visible_resources;
  const auto is_visible = [&](uint32_t resource) {
    if (!resource) return false;
    auto [visible, unseen] = visible_resources.try_emplace(resource, false);
    if (unseen) visible->second = ground_prop_is_mesh(*g_resources, resource);
    return visible->second;
  };
  size_t props = 0;
  std::unordered_set<uint32_t> used_resources;
  // Light children per object type, in the object's frame (#237). The RPK
  // entry's typeof (13) marks a light; the mesh filter below would drop them.
  std::unordered_map<uint32_t, std::vector<SceneryLight>> type_lights;
  std::unordered_map<uint32_t, std::optional<LightResource>> light_resources;
  {
    // A type's CFG and collision meshes only read packs and files. Prepare
    // the map's types on the job pool, then place them in map order.
    std::vector<uint32_t> types;
    for (const auto& placement : map.placements)
      if (object_types.try_emplace(placement.type).second) types.push_back(placement.type);
    std::vector<PartConfig> configs(types.size());
    std::vector<NativeCollisionData> collisions(types.size());
    std::vector<char> build(types.size());
    for (size_t i = 0; i < types.size(); ++i)
      build[i] = types[i] && !g_native_collision.contains(types[i]);
    jobs_parallel_for(types.size(), [&](size_t i) {
      ground_prop_config(*g_resources, types[i], configs[i]);
      if (build[i]) collisions[i] = build_native_collision(*g_resources, configs[i]);
    });
    for (size_t i = 0; i < types.size(); ++i) {
      if (build[i]) g_native_collision.emplace(types[i], std::move(collisions[i]));
      auto& renders = (object_types[types[i]] = std::move(configs[i])).renders;
      for (const auto& render : renders) {
        if (!render.variants.empty() || !is_light_resource(*g_resources, render.resource))
          continue;
        auto [cached, unseen] = light_resources.try_emplace(render.resource);
        if (unseen) cached->second = load_light_resource(*g_resources, render.resource);
        if (!cached->second) continue;
        const auto& resource = *cached->second;
        const auto frame = part_pose_matrix(render.pose);
        SceneryLight light;
        light.position = transform_point(frame, resource.position);
        const auto& d = resource.direction;
        light.direction = {frame[0] * d[0] + frame[4] * d[1] + frame[8] * d[2],
                           frame[1] * d[0] + frame[5] * d[1] + frame[9] * d[2],
                           frame[2] * d[0] + frame[6] * d[1] + frame[10] * d[2]};
        light.color = resource.diffuse;
        light.range = resource.range;
        light.attenuation = resource.attenuation;
        light.group = render.flags;
        light.resource = render.resource;
        light.lights_scene = light_resource_lights_scene(resource.type);
        light.spot = resource.type == LightResourceType::Spot ||
                     resource.type == LightResourceType::FakeSpot;
        type_lights[types[i]].push_back(light);
      }
      std::erase_if(renders, [&](const PartRender& render) {
        return render.variants.empty() ? !is_visible(render.resource)
            : std::none_of(render.variants.begin(), render.variants.end(), is_visible);
      });
      // A type with stock `lods` thresholds keeps its whole LOD chain (#252);
      // one with `lod` lines but no thresholds keeps only its highest LOD.
      if (!object_types[types[i]].lod_sizes.empty()) continue;
      const auto score = [](const PartRender& render) {
        return std::pair{render.lod_level + render.lod_last, render.lod_last};
      };
      const auto best = std::max_element(renders.begin(), renders.end(),
          [&](const PartRender& a, const PartRender& b) { return score(a) < score(b); });
      if (best != renders.end() && best->lod_level >= 0) {
        const auto best_index = static_cast<size_t>(best - renders.begin());
        for (size_t r = renders.size(); r-- > 0;)
          if (r != best_index && renders[r].lod_level >= 0)
            renders.erase(renders.begin() + r);
      }
    }
  }
  // Static collision meshes in placement order, baked on the job pool below.
  struct CollisionBake {
    const ScxGeometry* mesh;
    PartMatrix pose;
  };
  std::vector<CollisionBake> bakes;
  std::vector<SceneryLight> lights;
  for (const auto& placement : map.placements) {
    const auto type = object_types.find(placement.type);
    const auto pose = part_pose_matrix({placement.position, placement.rotation});
    // A dynamic placement's light stays at the authored pose when it is
    // knocked over; lights do not follow promoted bodies.
    if (const auto found = type_lights.find(placement.type); found != type_lights.end())
      for (auto light : found->second) {
        const auto& d = light.direction;
        light.direction = {pose[0] * d[0] + pose[4] * d[1] + pose[8] * d[2],
                           pose[1] * d[0] + pose[5] * d[1] + pose[9] * d[2],
                           pose[2] * d[0] + pose[6] * d[1] + pose[10] * d[2]};
        light.position = transform_point(pose, light.position);
        lights.push_back(light);
      }
    const auto& collision = native_collision_data(*g_resources, placement.type,
                                                  &type->second);
    const auto& p = placement.position;
    // The native object uses one seed for all render slots and palettes.
    const float sum = (p[0] + p[1]) + p[2];
    const auto fallback = std::isfinite(sum)
        ? static_cast<int32_t>(std::fmod(static_cast<double>(sum), 2147483648.)) : 0;
    const uint32_t seed = static_cast<uint32_t>(placement.seed >= 0 ? placement.seed : fallback)
                          & 0x7fffffffu;
    int32_t dormant_index = -1;
    if (collision.dynamic) {
      // Rendered as a plain instanced prop until a car comes close; see
      // scene_instances_wake_dynamics.
      auto& shared = dormant_types[placement.type];
      if (!shared) shared = std::make_shared<const PartConfig>(type->second);
      DormantProp entry;
      entry.config = shared;
      entry.pose = pose;
      entry.seed = static_cast<int32_t>(seed);
      if (collision.mesh) {
        for (int axis = 0; axis < 3; ++axis) {
          entry.local_lower[axis] = collision.mesh->info.bbox_min[axis];
          entry.local_upper[axis] = collision.mesh->info.bbox_max[axis];
        }
        dormant_bounds(entry);
        dormant_index = static_cast<int32_t>(props_map.dormant.size());
        dormant_cells(entry, [&](uint64_t cell) {
          props_map.dormant_grid[cell].push_back(static_cast<uint32_t>(dormant_index));
        });
        props_map.dormant.push_back(std::move(entry));
      }
    } else if (collision.mesh) {
      bakes.push_back({collision.mesh.get(), pose});
    }
    if (!collision.dynamic)
      for (const auto& body : collision.primitives) {
        const auto frame = part_matrix_multiply(pose, part_pose_matrix(body.pose));
        if (body.shape == PartBodyShape::Box)
          props_map.collision_shapes.add_box(
              frame, {body.box_half[0], body.box_half[1], body.box_half[2]});
        else if (body.shape == PartBodyShape::Sphere)
          props_map.collision_shapes.add_sphere(frame, body.sphere_radius);
      }
    if (type->second.renders.empty()) continue;
    int32_t lod_index = -1;
    for (const auto& render : type->second.renders) {
      const auto resource = render.variants.empty() ? render.resource
          : render.variants[seed % render.variants.size()];
      if (!is_visible(resource)) continue;
      const bool lod_render = render.lod_level >= 0 && !type->second.lod_sizes.empty();
      if (lod_render && lod_index < 0) {
        lod_index = static_cast<int32_t>(props_map.lods.size());
        PlacementLod entry;
        entry.center = {pose[12], pose[13], pose[14]};
        entry.sizes = type->second.lod_sizes;
        props_map.lods.push_back(std::move(entry));
      }
      const auto color = render.colors.empty() ? 0xffffffu : render.colors[seed % render.colors.size()];
      const auto transform = part_matrix_multiply(pose, part_pose_matrix(render.pose));
      auto& render_cell = cells[floor_cell(prop_cell(transform[12]), prop_cell(transform[14]))];
      render_cell.props.push_back({resource, transform, color});
      render_cell.props.back().dormant = dormant_index;
      render_cell.props.back().night_only = ground_prop_is_night(*g_resources, resource);
      if (lod_render) {
        render_cell.props.back().lod_min = static_cast<int8_t>(std::clamp(render.lod_level, 0, 127));
        render_cell.props.back().lod_max =
            static_cast<int8_t>(std::clamp(std::max(render.lod_last, render.lod_level), 0, 127));
        render_cell.props.back().lod_index = lod_index;
      }
      if(type->second.native_kind=="trlight"){
        render_cell.props.push_back({resource,pose,0xffffff});auto&glow=render_cell.props.back();glow.signal=true;glow.signal_position=placement.position;
      }
      used_resources.insert(resource);
      ++props;
    }
  }
  {
    const size_t first = props_map.collision_meshes.size();
    props_map.collision_meshes.resize(first + bakes.size());
    jobs_parallel_for(bakes.size(), [&](size_t i) {
      props_map.collision_meshes[first + i] = bake_static_mesh(*bakes[i].mesh, bakes[i].pose);
    });
  }
  props_map.resources.assign(used_resources.begin(), used_resources.end());
  if (!lights.empty()) {
    props_map.lights = std::make_shared<const std::vector<SceneryLight>>(std::move(lights));
    jobs_printf("[scene] ground 0x%x scenery lights=%zu\n", map.render_id,
                props_map.lights->size());
  }
  preload_meshes(props_map.resources);
  for (auto& [key, cell] : cells) {
    std::array<float, 3> lower, upper;
    lower.fill(std::numeric_limits<float>::infinity());
    upper.fill(-std::numeric_limits<float>::infinity());
    std::erase_if(cell.props, [](const GroundProp& prop) { return !geometry(prop.render_id); });
    for (uint32_t index = 0; index < cell.props.size(); ++index)
      if (cell.props[index].dormant >= 0)
        props_map.dormant[static_cast<size_t>(cell.props[index].dormant)].props.push_back(
            {&cell, index});
    for (auto& prop : cell.props) {
      const auto mesh = geometry(prop.render_id);
      std::array<float, 3> center{}, half{};
      for (int axis = 0; axis < 3; ++axis) {
        center[axis] = (mesh->info.bbox_min[axis] + mesh->info.bbox_max[axis]) * mesh->position_scale * .5f;
        half[axis] = (mesh->info.bbox_max[axis] - mesh->info.bbox_min[axis]) * mesh->position_scale * .5f;
      }
      prop.center = transform_point(prop.pose, center);
      prop.radius = std::hypot(half[0], half[1], half[2]);
      for (int axis = 0; axis < 3; ++axis) {
        lower[axis] = std::min(lower[axis], prop.center[axis] - prop.radius);
        upper[axis] = std::max(upper[axis], prop.center[axis] + prop.radius);
      }
    }
    // LOD radius of each placement: the sphere around its origin that holds
    // every one of its LOD renders, so all its levels switch together.
    for (const auto& prop : cell.props) {
      if (prop.lod_index < 0) continue;
      auto& entry = props_map.lods[static_cast<size_t>(prop.lod_index)];
      const float dx = prop.center[0] - entry.center[0], dy = prop.center[1] - entry.center[1],
                  dz = prop.center[2] - entry.center[2];
      entry.radius = std::max(entry.radius, std::sqrt(dx * dx + dy * dy + dz * dz) + prop.radius);
    }
    if (cell.props.empty()) continue;
    for (int axis = 0; axis < 3; ++axis) cell.center[axis] = (lower[axis] + upper[axis]) * .5f;
    cell.radius = .5f * std::hypot(upper[0] - lower[0], upper[1] - lower[1], upper[2] - lower[2]);
  }
  std::unordered_set<uint32_t> batchable;
  for (const auto id : props_map.resources) {
    const auto mesh = geometry(id);
    if (mesh && !ground_prop_is_night(*g_resources, id) && prop_mesh_batchable(*mesh))
      batchable.insert(id);
  }
  std::vector<GroundPropCell*> jobs;
  jobs.reserve(cells.size());
  for (auto& [key, cell] : cells)
    if (!cell.props.empty()) jobs.push_back(&cell);
  jobs_parallel_for(jobs.size(), [&](size_t job) { build_prop_batches(*jobs[job], batchable); });
  // Persistent batch layout (#260): a slot per batchable mesh of this map.
  {
    std::unordered_map<uint32_t, uint32_t> slot_of;
    for (auto& [key, cell] : cells)
      for (auto& batch : cell.batches) {
        const auto [found, added] =
            slot_of.try_emplace(batch.render_id, static_cast<uint32_t>(props_map.batch_ids.size()));
        if (added) {
          props_map.batch_ids.push_back(batch.render_id);
          props_map.batch_geometry.push_back(geometry(batch.render_id));
        }
        batch.slot = found->second;
      }
    std::unordered_map<uint32_t, uint32_t> night_slot_of;
    for (auto& [key, cell] : cells)
      for (auto& prop : cell.props) {
        if (!prop.night_only || prop.signal) continue;
        const auto [found, added] = night_slot_of.try_emplace(
            prop.render_id, static_cast<uint32_t>(props_map.night_ids.size()));
        if (added) {
          props_map.night_ids.push_back(prop.render_id);
          props_map.night_geometry.push_back(geometry(prop.render_id));
        }
        prop.night_slot = static_cast<int32_t>(found->second);
      }
    props_map.world_cache_valid = false;
  }
  std::erase_if(cells, [](const auto& entry) { return entry.second.props.empty(); });
  merge_static_meshes(props_map.collision_meshes);
  if (!props_map.collision_meshes.empty() || props_map.collision_shapes.size()) {
    ++g_floor_revision;
    g_floor_prepared = false;
  }
  std::printf("[scene] ground props 0x%x placements=%zu renders=%zu "
              "collisionMeshes=%zu collisionShapes=%zu\n",
              map.render_id, map.placements.size(), props,
              props_map.collision_meshes.size(), props_map.collision_shapes.size());
}

bool invert_columns(const float *m, float inv[9]) {
  const float a00 = m[0], a10 = m[1], a20 = m[2];
  const float a01 = m[4], a11 = m[5], a21 = m[6];
  const float a02 = m[8], a12 = m[9], a22 = m[10];
  const float det = a00 * (a11 * a22 - a21 * a12) -
                    a01 * (a10 * a22 - a20 * a12) +
                    a02 * (a10 * a21 - a20 * a11);
  if (std::fabs(det) < 1e-12f)
    return false;
  const float id = 1.f / det;
  inv[0] = (a11 * a22 - a21 * a12) * id;
  inv[1] = (a20 * a12 - a10 * a22) * id;
  inv[2] = (a10 * a21 - a20 * a11) * id;
  inv[3] = (a21 * a02 - a01 * a22) * id;
  inv[4] = (a00 * a22 - a20 * a02) * id;
  inv[5] = (a20 * a01 - a00 * a21) * id;
  inv[6] = (a01 * a12 - a11 * a02) * id;
  inv[7] = (a10 * a02 - a00 * a12) * id;
  inv[8] = (a00 * a11 - a10 * a01) * id;
  return true;
}

bool ray_triangle(const float o[3], const float d[3], const float a[3],
                  const float b[3], const float c[3], float &t) {
  const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                      d[0] * e2[1] - d[1] * e2[0]};
  const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
  if (std::fabs(det) < 1e-8f)
    return false;
  const float inv = 1.f / det;
  const float s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
  const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
  if (u < 0.f || u > 1.f)
    return false;
  const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
                      s[0] * e1[1] - s[1] * e1[0]};
  const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
  if (v < 0.f || u + v > 1.f)
    return false;
  t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
  return t > 1e-4f;
}

bool scene_instances_pointer_ray(int px, int py, int width, int height,
                                 float origin[3], float direction[3]) {
  if (width <= 0 || height <= 0) return false;
  float at[3], fov = 60.f, zn, zf;
  if (!gameref_world_camera(origin, at, &fov, &zn, &zf)) return false;
  float fx = at[0] - origin[0], fy = at[1] - origin[1], fz = at[2] - origin[2];
  const float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
  if (fl < 1e-4f) return false;
  fx /= fl; fy /= fl; fz /= fl;
  float rx = -fz, rz = fx;
  const float rl = std::sqrt(rx*rx + rz*rz);
  if (rl < 1e-4f) { rx = 1.f; rz = 0.f; }
  else { rx /= rl; rz /= rl; }
  const float tangent = std::tan(fov * 3.14159265f / 360.f);
  const float x = (2.f*px/width - 1.f) * tangent * width/height;
  const float y = (1.f - 2.f*py/height) * tangent;
  direction[0] = fx + rx*x - rz*fy*y;
  direction[1] = fy + (rz*fx - rx*fz)*y;
  direction[2] = fz + rz*x + rx*fy*y;
  const float dl = std::sqrt(direction[0]*direction[0] + direction[1]*direction[1] + direction[2]*direction[2]);
  if (dl < 1e-6f) return false;
  for (int i = 0; i < 3; ++i) direction[i] /= dl;
  return true;
}
void *scene_instances_pick(const float origin[3], const float direction[3], float* point) {
  ScenePickSurface surface;
  if (!scene_instances_pick_surface(origin, direction, &surface))
    return nullptr;
  if (point)
    std::copy(surface.world, surface.world + 3, point);
  return surface.key;
}

bool scene_instances_pick_surface(const float origin[3], const float direction[3],
                                  ScenePickSurface *surface) {
  if (!origin || !direction || !surface)
    return false;
  *surface = ScenePickSurface{};
  float screen_up[3] = {0.f, 1.f, 0.f}, eye[3], at[3];
  if (gameref_world_camera(eye, at, nullptr, nullptr, nullptr, screen_up)) {
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const float fl2 = f[0]*f[0] + f[1]*f[1] + f[2]*f[2];
    if (fl2 > 1e-8f) {
      const float d = (f[0]*screen_up[0] + f[1]*screen_up[1] + f[2]*screen_up[2]) / fl2;
      for (int k = 0; k < 3; ++k) screen_up[k] -= f[k] * d;
    }
  }
  float best = 1e9f;
  void *hit = nullptr;
  for (const SceneInstance &instance : scene_instances_collect_root(g_world)) {
    if (instance.sky || !instance.geometry || !instance.key)
      continue;
    float matrix[16];
    float inv[9];
    if (!scene_instances_script_matrix(instance.key, matrix) ||
        !invert_columns(matrix, inv))
      continue;
    const float scale = instance.geometry->position_scale;
    const ScxMeshInfo &info = instance.geometry->info;
    const float lx = origin[0] - matrix[12];
    const float ly = origin[1] - matrix[13];
    const float lz = origin[2] - matrix[14];
    const float o[3] = {inv[0] * lx + inv[3] * ly + inv[6] * lz,
                        inv[1] * lx + inv[4] * ly + inv[7] * lz,
                        inv[2] * lx + inv[5] * ly + inv[8] * lz};
    const float d[3] = {
        inv[0] * direction[0] + inv[3] * direction[1] + inv[6] * direction[2],
        inv[1] * direction[0] + inv[4] * direction[1] + inv[7] * direction[2],
        inv[2] * direction[0] + inv[5] * direction[1] + inv[8] * direction[2]};
    float tmin = -1e9f, tmax = 1e9f;
    bool miss = false;
    for (int axis = 0; axis < 3 && !miss; ++axis) {
      float lo = info.bbox_min[axis] * scale - 0.015f;
      float hi = info.bbox_max[axis] * scale + 0.015f;
      if (lo > hi)
        std::swap(lo, hi);
      if (std::fabs(d[axis]) < 1e-8f) {
        miss = o[axis] < lo || o[axis] > hi;
        continue;
      }
      float t1 = (lo - o[axis]) / d[axis];
      float t2 = (hi - o[axis]) / d[axis];
      if (t1 > t2)
        std::swap(t1, t2);
      tmin = std::max(tmin, t1);
      tmax = std::min(tmax, t2);
      miss = tmin > tmax;
    }
    // The chassis box contains the doors and the wheels. Rejecting on the
    // box entry kept every child behind the body. The surface decides.
    if (miss || tmax < 0.f || tmin >= best)
      continue;
    const auto &pos = instance.geometry->positions;
    const auto &idx = instance.geometry->indices;
    bool triangle = false;
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
      float v[3][3];
      bool bad = false;
      for (int k = 0; k < 3; ++k) {
        const size_t vi = static_cast<size_t>(idx[i + k]) * 3;
        if (vi + 2 >= pos.size()) {
          bad = true;
          break;
        }
        v[k][0] = pos[vi] * scale;
        v[k][1] = pos[vi + 1] * scale;
        v[k][2] = pos[vi + 2] * scale;
      }
      float t = 0.f;
      if (!bad && ray_triangle(o, d, v[0], v[1], v[2], t) && t < best) {
        best = t;
        hit = instance.key;
        triangle = true;
        // Face normal turned toward the viewer: winding is not a reliable
        // side once the render reflection and mirrored parts are involved.
        const float e1[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
        const float e2[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]};
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float side = n[0] * d[0] + n[1] * d[1] + n[2] * d[2] > 0.f ? -1.f : 1.f;
        surface->geometry = instance.geometry;
        surface->paint_slot = instance.paint_slot;
        surface->mirrored = matrix[0] * (matrix[5]*matrix[10] - matrix[6]*matrix[9])
                          - matrix[4] * (matrix[1]*matrix[10] - matrix[2]*matrix[9])
                          + matrix[8] * (matrix[1]*matrix[6] - matrix[2]*matrix[5]) < 0.f;
        for (int k = 0; k < 3; ++k) {
          surface->screen_up[k] = inv[k] * screen_up[0] + inv[3+k] * screen_up[1] + inv[6+k] * screen_up[2];
          surface->world_up[k] = screen_up[k];
          surface->local[k] = o[k] + d[k] * t;
          surface->normal[k] = nl > 0.f ? side * n[k] / nl : 0.f;
        }
        for (int k = 0; k < 3; ++k)
          surface->world_normal[k] = matrix[k] * surface->normal[0] +
                                     matrix[4 + k] * surface->normal[1] +
                                     matrix[8 + k] * surface->normal[2];
      }
    }
    if (!triangle && idx.empty() && tmin >= 0.f && tmin < best) {
      best = tmin;
      hit = instance.key;
      surface->geometry = nullptr;
      surface->paint_slot = -1;
    }
  }
  if (!hit)
    return false;
  surface->key = hit;
  for (int i = 0; i < 3; ++i) surface->world[i] = origin[i] + direction[i] * best;
  return true;
}

std::vector<ScenePaintTarget> scene_instances_paint_targets(const float world[3], float radius) {
  std::vector<ScenePaintTarget> out;
  if (!world)
    return out;
  for (const SceneInstance &instance : scene_instances_collect_root(g_world)) {
    if (instance.sky || !instance.geometry || !instance.key || instance.paint_slot < 0)
      continue;
    ScenePaintTarget target;
    if (!scene_instances_script_matrix(instance.key, target.matrix) ||
        !invert_columns(target.matrix, target.inverse))
      continue;
    const float *m = target.matrix, *inv = target.inverse;
    const float p[3] = {world[0] - m[12], world[1] - m[13], world[2] - m[14]};
    const float scale = instance.geometry->position_scale;
    const ScxMeshInfo &info = instance.geometry->info;
    bool near = true;
    for (int k = 0; k < 3 && near; ++k) {
      const float local = inv[k] * p[0] + inv[3 + k] * p[1] + inv[6 + k] * p[2];
      float lo = info.bbox_min[k] * scale, hi = info.bbox_max[k] * scale;
      if (lo > hi)
        std::swap(lo, hi);
      near = local >= lo - radius && local <= hi + radius;
    }
    if (!near)
      continue;
    target.key = instance.key;
    target.geometry = instance.geometry;
    target.paint_slot = instance.paint_slot;
    target.mirrored = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) +
                      m[8] * (m[1] * m[6] - m[2] * m[5]) < 0.f;
    out.push_back(std::move(target));
  }
  return out;
}

void scene_visit_gc_roots(const GcRootVisitor& visit) {
  visit(g_world); visit(g_sky);
}
} // namespace slrr
