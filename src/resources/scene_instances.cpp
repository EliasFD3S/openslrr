#include "slrr/scene_instances.hpp"
#include "slrr/gameref.hpp"
#include "slrr/rpk_index.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace slrr {
namespace {
const RpkIndex* g_resources = nullptr;
void* g_world = nullptr;
void* g_sky = nullptr;
std::unordered_map<uint32_t, std::shared_ptr<const ScxGeometry>> g_meshes;

void identity(float* m) {
  std::fill(m, m + 16, 0.f);
  m[0] = m[5] = m[10] = m[15] = 1.f;
}
void multiply(float* out, const float* a, const float* b) {
  float result[16]{};
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row)
      for (int k = 0; k < 4; ++k)
        result[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
  std::copy(result, result + 16, out);
}
void local_matrix(const GameRefState& n, float* m) {
  float yaw[16], pitch[16], roll[16];
  identity(yaw); identity(pitch); identity(roll);
  const float cy = std::cos(n.ypr[0]), sy = std::sin(n.ypr[0]);
  const float cp = std::cos(n.ypr[1]), sp = std::sin(n.ypr[1]);
  const float cr = std::cos(n.ypr[2]), sr = std::sin(n.ypr[2]);
  yaw[0] = cy; yaw[2] = -sy; yaw[8] = sy; yaw[10] = cy;
  pitch[5] = cp; pitch[6] = sp; pitch[9] = -sp; pitch[10] = cp;
  roll[0] = cr; roll[1] = sr; roll[4] = -sr; roll[5] = cr;
  multiply(m, yaw, pitch); multiply(m, m, roll);
  m[12] = n.pos[0]; m[13] = n.pos[1]; m[14] = n.pos[2];
}

std::shared_ptr<const ScxGeometry> geometry(uint32_t id) {
  auto found = g_meshes.find(id);
  if (found != g_meshes.end()) return found->second;
  std::shared_ptr<const ScxGeometry> result;
  const int pack = RpkIndex::pack_from_full(id);
  auto model = g_resources->model_for_resource(pack, id & 0xffffu);
  if (model.ok) {
    std::string source = model.source;
    std::replace(source.begin(), source.end(), '\\', '/');
    std::filesystem::path path = std::filesystem::path(g_resources->game_root()) / source;
    std::string ext = path.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".scy") {
      auto scx = path; scx.replace_extension(".scx");
      if (std::filesystem::exists(scx)) path = scx;
    }
    ext = path.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".scx" || ext == ".scy") {
      auto mesh = std::make_shared<ScxGeometry>(load_scx_geometry(path.string()));
      if (mesh->info.ok && !mesh->indices.empty()) {
        for (auto& texture : model.textures) {
          std::replace(texture.begin(), texture.end(), '\\', '/');
          if (!texture.empty() && std::filesystem::path(texture).is_relative())
            texture = (std::filesystem::path(g_resources->game_root()) / texture).string();
        }
        mesh->textures = std::move(model.textures);
        result = std::move(mesh);
        std::printf("[scene] resource=0x%x source=%s vertices=%d\n", id,
                    path.string().c_str(), result->info.vertices);
      }
    }
  }
  g_meshes.emplace(id, result);
  return result;
}
}  // namespace

void scene_instances_set_resources(const RpkIndex* resources) {
  g_resources = resources;
  g_world = g_sky = nullptr;
  g_meshes.clear();
}
void scene_instances_select(void* world, void* skydome) {
  g_world = world; g_sky = skydome;
  if (auto* sky = gameref_get(skydome)) sky->sky_render = true;
}

bool scene_instance_transform(void* key, void* root, float* matrix) {
  if (!key || !root || !matrix) return false;
  identity(matrix);
  std::unordered_set<void*> seen;
  for (void* current = key; current;) {
    if (!seen.insert(current).second || seen.size() > 128) return false;
    auto* n = gameref_get(current);
    if (!n || n->empty || (n->flags & kWorldTreeLeaf)) return false;
    // Inventory items are independent worlds even though their roots have
    // an owning inventory parent. Never leak their meshes into the garage.
    if(current != root && (n->flags & kWorldTreeRoot)) return false;
    float local[16]; local_matrix(*n, local);
    multiply(matrix, local, matrix);
    if (current == root) {
      // Vertices are uploaded as S*v (S reflects Z). Instance matrix is S*M*S.
      for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
          if ((col == 2) != (row == 2)) matrix[col * 4 + row] *= -1.f;
      return true;
    }
    current = n->parent;
  }
  return false;
}

std::vector<SceneInstance> scene_instances_collect_root(void* root) {
  std::vector<SceneInstance> out;
  if (!g_resources || !root) return out;
  for (auto* n : gameref_render_nodes()) {
    // Source scripts replace Scene.skydome; a superseded handle may remain
    // alive until script cleanup. Never render it as ordinary foreground.
    if (n->sky_render && n->key != g_sky) continue;
    SceneInstance instance;
    if (!scene_instance_transform(n->key, root, instance.transform.data())) continue;
    instance.geometry = geometry(static_cast<uint32_t>(n->type_id));
    if (!instance.geometry) continue;
    instance.key = n->key;
    instance.sky = n->key == g_sky;
    instance.color = n->color;
    out.push_back(std::move(instance));
  }
  return out;
}
std::vector<SceneInstance> scene_instances_collect() {
  return scene_instances_collect_root(g_world);
}
}  // namespace slrr
