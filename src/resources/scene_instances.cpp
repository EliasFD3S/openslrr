#include "slrr/gc_roots.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/gameref.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/vehicle_dynamics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace slrr {
namespace {
const RpkIndex *g_resources = nullptr;
void *g_world = nullptr;
void *g_sky = nullptr;
bool g_preview_lamps = false;
uint32_t g_environment = 0;
struct ShadowInfo {
  uint32_t flags = 0;
  SceneShadowDecal decal;
};
std::unordered_map<uint32_t, ShadowInfo> g_shadow_info;
std::unordered_map<uint32_t, std::shared_ptr<const ScxGeometry>> g_meshes;
std::unordered_map<uint32_t, std::shared_ptr<const ScxGeometry>> g_overrides;
struct StaticMesh {
  std::vector<float> positions;
  std::vector<uint32_t> indices;
  float scale = 0.01f;
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
  std::vector<uint32_t> large_triangles;
};
std::vector<StaticMesh> g_static;
bool g_static_enabled = true;
uint64_t floor_cell(int32_t x, int32_t z) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
         static_cast<uint32_t>(z);
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
void index_floor(StaticMesh &mesh) {
  // Static garage triangles are indexed once in script space. Physics asks
  // for several support samples per 240 Hz tick; rescanning the whole shell
  // for every wheel/corner is unnecessary.
  for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
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
    const int32_t x0 = static_cast<int32_t>(std::floor(min_x / 2)),
                  x1 = static_cast<int32_t>(std::floor(max_x / 2));
    const int32_t z0 = static_cast<int32_t>(std::floor(min_z / 2)),
                  z1 = static_cast<int32_t>(std::floor(max_z / 2));
    if ((int64_t(x1) - x0 + 1) * (int64_t(z1) - z0 + 1) > 4096) {
      mesh.large_triangles.insert(mesh.large_triangles.end(),
                                  mesh.indices.begin() + i,
                                  mesh.indices.begin() + i + 3);
      continue;
    }
    for (int32_t x = x0; x <= x1; ++x)
      for (int32_t z = z0; z <= z1; ++z) {
        auto &cell = mesh.cells[floor_cell(x, z)];
        cell.insert(cell.end(), mesh.indices.begin() + i,
                    mesh.indices.begin() + i + 3);
      }
  }
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

std::shared_ptr<const ScxGeometry> geometry(uint32_t id) {
  auto over = g_overrides.find(id);
  if (over != g_overrides.end())
    return over->second;
  auto found = g_meshes.find(id);
  if (found != g_meshes.end())
    return found->second;
  std::shared_ptr<const ScxGeometry> result;
  const int pack = RpkIndex::pack_from_full(id);
  auto model = g_resources->model_for_resource(pack, id & 0xffffu);
  if (model.ok) {
    std::string source = model.source;
    std::replace(source.begin(), source.end(), '\\', '/');
    std::filesystem::path path =
        std::filesystem::path(g_resources->game_root()) / source;
    std::string ext = path.extension().string();
    for (char &c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".scy") {
      auto scx = path;
      scx.replace_extension(".scx");
      if (std::filesystem::exists(scx))
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
                (std::filesystem::path(g_resources->game_root()) / texture)
                    .string();
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
  std::vector<float> pos, nrm, uv0, uv1;
  std::vector<uint32_t> indices;
};

struct GroundWalk {
  const std::vector<uint8_t> *file = nullptr;
  std::string params;
  std::vector<PolySection> poly;
};

void take_params(GroundWalk &w, const uint8_t *data, const uint8_t *end) {
  if (!w.params.empty() || !data)
    return;
  const uint8_t *s = data;
  while (s < end) {
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
    if (e > v)
      w.params = text.substr(v, e - v);
  }
}

void walk_ground_node(GroundWalk &w, uint32_t offset, uint32_t size,
                      int depth) {
  const std::vector<uint8_t> &file = *w.file;
  if (size < 8 || offset > file.size() || size > file.size() - offset)
    return;
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
    kids.push_back(Kid{restype, foff, sz});
  }
  while (ok && p + 8 <= end) {
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
      take_params(w, data, data + csize);
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
        sec.uv0.resize(static_cast<size_t>(nv) * 2u);
        sec.uv1.resize(sec.uv0.size());
        for (uint32_t vi = 0; vi < nv; ++vi) {
          const uint8_t *v = q + static_cast<size_t>(vi) * 56u;
          sec.pos[vi * 3] = rdf(v);
          sec.pos[vi * 3 + 1] = rdf(v + 4);
          sec.pos[vi * 3 + 2] = rdf(v + 8);
          sec.nrm[vi * 3] = rdf(v + 12);
          sec.nrm[vi * 3 + 1] = rdf(v + 16);
          sec.nrm[vi * 3 + 2] = rdf(v + 20);
          sec.uv0[vi * 2] = rdf(v + 32);
          sec.uv0[vi * 2 + 1] = rdf(v + 36);
          sec.uv1[vi * 2] = rdf(v + 40);
          sec.uv1[vi * 2 + 1] = rdf(v + 44);
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
      walk_ground_node(w, k.off, k.sz, depth + 1);
}

struct GroundMat {
  uint16_t tex = 0xffff;
  int16_t lm = -1;
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
    mats.push_back(m);
    p += static_cast<size_t>(hsz) + 8;
  }
  return mats;
}
} // namespace

void scene_instances_set_resources(const RpkIndex *resources) {
  g_resources = resources;
  g_world = g_sky = nullptr;
  g_preview_lamps = false;
  g_meshes.clear();
  g_overrides.clear();
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
std::string scene_instances_texture(uint32_t resource) {
  return g_resources ? g_resources->texture_path_for_id(resource) : std::string{};
}
std::string scene_instances_envmap() {
  return g_resources ? g_resources->texture_path_for_id(g_environment)
                     : std::string{};
}
void scene_instances_select(void *world, void *skydome, bool preview_lamps) {
  if (g_world != world) ++g_floor_revision;
  g_world = world;
  g_sky = skydome;
  g_preview_lamps = preview_lamps;
  g_floor_prepared = false;
  if (auto *sky = gameref_get(skydome))
    sky->sky_render = true;
}

bool chain_matrix(void *key, void *root, float *matrix, bool reflect) {
  if (!key || !root || !matrix)
    return false;
  identity(matrix);
  std::unordered_set<void *> seen;
  for (void *current = key; current;) {
    if (!seen.insert(current).second || seen.size() > 128)
      return false;
    auto *n = gameref_get(current);
    if (!n || n->empty || (n->flags & kWorldTreeLeaf))
      return false;
    // Inventory items are independent worlds even though their roots have
    // an owning inventory parent. Never leak their meshes into the garage.
    if (current != root && (n->flags & kWorldTreeRoot))
      return false;
    float local[16];
    if (n->matrix_override)
      std::copy(n->local_matrix, n->local_matrix + 16, local);
    else
      local_matrix(*n, local);
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
  return chain_matrix(key, root, matrix, true);
}
bool scene_instances_script_matrix(void *key, float *matrix) {
  return g_world && chain_matrix(key, g_world, matrix, false);
}

namespace {
void collect_render_nodes(GameRefState* node, std::vector<GameRefState*>& out,
                          unsigned depth = 0) {
  if (!node || node->empty || (node->flags & kWorldTreeLeaf) ||
      (depth && (node->flags & kWorldTreeRoot)) || depth >= 128)
    return;
  if (node->kind == GameRefKind::RenderRef && node->type_id)
    out.push_back(node);
  for (auto* child : node->children)
    collect_render_nodes(gameref_get(child), out, depth + 1);
}

std::vector<GameRefState*> render_nodes_in_world(void* root) {
  std::vector<GameRefState*> nodes;
  collect_render_nodes(gameref_get(root), nodes);
  return nodes;
}

void floor_triangles(const float *m, const std::vector<float> &pos,
                     const std::vector<uint32_t> &idx, float scale, float x,
                     float z, float below, float &best, bool &hit,
                     float *normal) {
  if (!m || idx.size() < 3 || pos.size() < 9)
    return;
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    float v[3][3];
    bool ok = true;
    float min_x = 0, max_x = 0, min_z = 0, max_z = 0;
    for (int k = 0; k < 3; ++k) {
      const uint32_t vi = idx[t + static_cast<size_t>(k)];
      if (static_cast<size_t>(vi) * 3 + 2 >= pos.size()) {
        ok = false;
        break;
      }
      const float vx = pos[vi * 3] * scale;
      const float vy = pos[vi * 3 + 1] * scale;
      const float vz = pos[vi * 3 + 2] * scale;
      v[k][0] = m[0] * vx + m[4] * vy + m[8] * vz + m[12];
      v[k][1] = m[1] * vx + m[5] * vy + m[9] * vz + m[13];
      v[k][2] = m[2] * vx + m[6] * vy + m[10] * vz + m[14];
      if (k == 0) {
        min_x = max_x = v[0][0];
        min_z = max_z = v[0][2];
      } else {
        min_x = std::min(min_x, v[k][0]);
        max_x = std::max(max_x, v[k][0]);
        min_z = std::min(min_z, v[k][2]);
        max_z = std::max(max_z, v[k][2]);
      }
    }
    if (!ok || x < min_x || x > max_x || z < min_z || z > max_z)
      continue;
    const float x0 = v[0][0], z0 = v[0][2], x1 = v[1][0], z1 = v[1][2];
    const float x2 = v[2][0], z2 = v[2][2];
    const float denom = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2);
    if (std::fabs(denom) < 1e-8f)
      continue;
    const float a = ((z1 - z2) * (x - x2) + (x2 - x1) * (z - z2)) / denom;
    const float b = ((z2 - z0) * (x - x2) + (x0 - x2) * (z - z2)) / denom;
    const float c = 1.f - a - b;
    if (a < -1e-4f || b < -1e-4f || c < -1e-4f)
      continue;
    const float y = a * v[0][1] + b * v[1][1] + c * v[2][1];
    if (y > below + 0.05f)
      continue;
    if (!hit || y > best) {
      best = y;
      hit = true;
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
  std::unordered_set<void *> active;
  if (g_world)
    for (auto *n : render_nodes_in_world(g_world)) {
      if (!n || n->empty || n->sky_render || n->vehicle_material)
        continue;
      std::array<float, 16> m;
      if (!chain_matrix(n->key, g_world, m.data(), false))
        continue;
      auto geo = geometry(static_cast<uint32_t>(n->type_id));
      if (!geo)
        continue;
      active.insert(n->key);
      auto &cached = g_floor_meshes[n->key];
      if (cached.source != geo || cached.matrix != m) {
        cached.source = geo;
        cached.matrix = m;
        cached.mesh = StaticMesh{};
        auto &mesh = cached.mesh;
        mesh.scale = 1;
        mesh.indices = geo->indices;
        mesh.positions.resize(geo->positions.size());
        for (size_t i = 0; i + 2 < geo->positions.size(); i += 3) {
          float x = geo->positions[i] * geo->position_scale,
                y = geo->positions[i + 1] * geo->position_scale,
                z = geo->positions[i + 2] * geo->position_scale;
          mesh.positions[i] = m[0] * x + m[4] * y + m[8] * z + m[12];
          mesh.positions[i + 1] = m[1] * x + m[5] * y + m[9] * z + m[13];
          mesh.positions[i + 2] = m[2] * x + m[6] * y + m[10] * z + m[14];
        }
        index_floor(mesh);
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
  for (auto it = g_floor_meshes.begin(); it != g_floor_meshes.end();)
    if (!active.count(it->first)) {
      ++g_floor_revision;
      it = g_floor_meshes.erase(it);
    } else
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

void sweep_triangles(const StaticMesh &mesh, const std::vector<uint32_t> &indices,
                     Vec3 start, Vec3 delta, double radius, SceneSphereHit &hit) {
  const Vec3 end = start + delta;
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    Vec3 v[3];
    bool valid = true;
    for (int j = 0; j < 3; ++j) {
      const size_t offset = static_cast<size_t>(indices[i + j]) * 3;
      if (offset + 2 >= mesh.positions.size()) {
        valid = false;
        break;
      }
      const float *p = mesh.positions.data() + offset;
      v[j] = Vec3{p[0], p[1], p[2]} * mesh.scale;
    }
    if (!valid || std::min({v[0].y, v[1].y, v[2].y}) > std::max(start.y, end.y) + radius ||
        std::max({v[0].y, v[1].y, v[2].y}) < std::min(start.y, end.y) - radius)
      continue;
    const Vec3 n = (v[1] - v[0]).cross(v[2] - v[0]).normalized();
    if (n.dot(n) < 0.5) continue;

    double t = 0.0;
    // Conservative advancement against the face, edges and vertices.
    for (int step = 0; step < 12 && t < hit.fraction; ++step) {
      const Vec3 p = start + delta * t;
      const double height = (p - v[0]).dot(n);
      const Vec3 projected = p - n * height;
      Vec3 closest = projected;
      double edge_distance2 = 0.0;
      bool outside = false;
      for (int j = 0; j < 3; ++j) {
        const Vec3 a = v[j], b = v[(j + 1) % 3];
        if ((b - a).cross(projected - a).dot(n) >= 0.0) continue;
        const Vec3 on_edge = closest_on_edge(projected, a, b);
        const Vec3 offset = on_edge - projected;
        const double distance2 = offset.dot(offset);
        if (!outside || distance2 < edge_distance2) {
          closest = on_edge;
          edge_distance2 = distance2;
        }
        outside = true;
      }
      const Vec3 offset = p - closest;
      const double distance = offset.length();
      const Vec3 normal = distance > 1e-8 ? offset / distance
                                         : (delta.dot(n) > 0.0 ? -n : n);
      const double closing = -delta.dot(normal);
      if (closing <= 1e-8) break;
      if (distance <= radius + 1e-5) {
        hit.fraction = static_cast<float>(t);
        hit.normal[0] = static_cast<float>(normal.x);
        hit.normal[1] = static_cast<float>(normal.y);
        hit.normal[2] = static_cast<float>(normal.z);
        break;
      }
      t += (distance - radius) / closing;
    }
  }
}
} // namespace

bool scene_instances_sweep_sphere(const float start[3], const float delta[3],
                                  float radius, SceneSphereHit &hit) {
  const Vec3 from{start[0], start[1], start[2]};
  const Vec3 movement{delta[0], delta[1], delta[2]};
  if (!from.finite() || !movement.finite() || movement.dot(movement) < 1e-12)
    return false;
  if (!g_floor_prepared) scene_instances_prepare_physics();
  hit = {};
  const Vec3 to = from + movement;
  const int32_t x0 = static_cast<int32_t>(std::floor((std::min(from.x, to.x) - radius) / 2));
  const int32_t x1 = static_cast<int32_t>(std::floor((std::max(from.x, to.x) + radius) / 2));
  const int32_t z0 = static_cast<int32_t>(std::floor((std::min(from.z, to.z) - radius) / 2));
  const int32_t z1 = static_cast<int32_t>(std::floor((std::max(from.z, to.z) + radius) / 2));
  const auto sweep_mesh = [&](const StaticMesh &mesh) {
    for (int32_t x = x0; x <= x1; ++x)
      for (int32_t z = z0; z <= z1; ++z) {
        const auto cell = mesh.cells.find(floor_cell(x, z));
        if (cell != mesh.cells.end())
          sweep_triangles(mesh, cell->second, from, movement, radius, hit);
      }
    sweep_triangles(mesh, mesh.large_triangles, from, movement, radius, hit);
  };
  if (g_static_enabled)
    for (const auto &mesh : g_static) sweep_mesh(mesh);
  for (const auto &entry : g_floor_meshes) sweep_mesh(entry.second.mesh);
  return hit.fraction < 1.f;
}

bool scene_instances_sample_floor(float x, float z, float below, void *ignore,
                                  SceneFloorHit &sample) {
  float best = 0.f;
  bool hit = false;
  // Preloaded garage shells are drawn at identity with unflipped script XZ.
  float eye[16];
  identity(eye);
  if (g_static_enabled)
    for (const auto &mesh : g_static) {
      const auto cell =
          mesh.cells.find(floor_cell(static_cast<int32_t>(std::floor(x / 2)),
                                     static_cast<int32_t>(std::floor(z / 2))));
      if (cell != mesh.cells.end())
        floor_triangles(eye, mesh.positions, cell->second, mesh.scale, x, z,
                        below, best, hit, sample.normal);
      floor_triangles(eye, mesh.positions, mesh.large_triangles, mesh.scale, x,
                      z, below, best, hit, sample.normal);
    }
  if (!g_floor_prepared)
    scene_instances_prepare_physics();
  for (const auto &entry : g_floor_meshes) {
    const auto &cached = entry.second;
    if (ignore && std::find(cached.ancestors.begin(), cached.ancestors.end(),
                            ignore) != cached.ancestors.end())
      continue;
    const auto &mesh = cached.mesh;
    auto cell =
        mesh.cells.find(floor_cell(static_cast<int32_t>(std::floor(x / 2)),
                                   static_cast<int32_t>(std::floor(z / 2))));
    if (cell != mesh.cells.end())
      floor_triangles(eye, mesh.positions, cell->second, mesh.scale, x, z,
                      below, best, hit, sample.normal);
    floor_triangles(eye, mesh.positions, mesh.large_triangles, mesh.scale, x, z,
                    below, best, hit, sample.normal);
  }
  if (!hit)
    return false;
  sample.height = best;
  return true;
}

bool scene_instances_supports(void *root,
                              std::vector<std::array<float, 3>> &points) {
  points.clear();
  if (!g_world || !root)
    return false;
  for (auto *n : render_nodes_in_world(root)) {
    if (!n || n->empty || n->sky_render)
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

SceneSun scene_instances_light(void* root) {
  if(!root)root=g_world;
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
  return selected;
}
std::vector<SceneInstance> scene_instances_collect_root(void *root) {
  std::vector<SceneInstance> out;
  if (!g_resources || !root)
    return out;
  const auto lighting=scene_instances_light(root);
  for (auto *n : render_nodes_in_world(root)) {
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
    instance.geometry = geometry(static_cast<uint32_t>(n->type_id));
    if (!instance.geometry)
      continue;
    instance.key = n->key;
    instance.type_id = n->type_id;
    instance.sky = n->key == g_sky;
    instance.color = n->color;
    instance.vehicle_material = n->vehicle_material;
    instance.light_flags = n->light_flags;
    if (n->paint_source) {
      // The mesh list prefers a .dds next to the source, while the CFG id
      // still names the .png. Compare the game-relative path without the
      // extension so the paint color replaces that one slot.
      auto key = [](std::string path) {
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
      };
      const auto from = key(g_resources->texture_path_for_id(n->paint_source));
      for (size_t i = 0;
           !from.empty() && i < instance.geometry->textures.size(); ++i) {
        if (key(instance.geometry->textures[i]) == from) {
          instance.paint_slot = static_cast<int>(i);
          break;
        }
      }
      if (n->paint_texture)
        instance.paint_texture =
            g_resources->texture_path_for_id(n->paint_texture);
    }
    out.push_back(std::move(instance));
  }
  return out;
}
std::vector<SceneInstance> scene_instances_collect() {
  return scene_instances_collect_root(g_world);
}

void scene_instances_override_mesh(uint32_t id,
                                   std::shared_ptr<const ScxGeometry> geo) {
  if (id && geo)
    g_overrides[id] = std::move(geo);
}
void scene_instances_enable_static_floor(bool enabled) {
  if (g_static_enabled != enabled) ++g_floor_revision;
  g_static_enabled = enabled;
}
void scene_instances_add_static_mesh(const ScxGeometry &geo) {
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

bool scene_instances_ground_map(const RpkIndex &rpk, uint32_t ground_id,
                                uint32_t *render_id,
                                std::shared_ptr<const ScxGeometry> *poly) {
  if (!render_id || !poly)
    return false;
  *render_id = 0;
  poly->reset();
  const RpkEntry *entry = rpk.resolve(ground_id);
  if (!entry || entry->typeof_ != 1 || entry->size < 8)
    return false;
  const int32_t pack = RpkIndex::pack_from_full(ground_id);
  const std::string disk = rpk.disk_path_for_pack(pack);
  std::ifstream in(disk, std::ios::binary | std::ios::ate);
  if (!in)
    return false;
  const auto sz = static_cast<std::streamoff>(in.tellg());
  if (sz <= 0)
    return false;
  in.seekg(0);
  std::vector<uint8_t> file(static_cast<size_t>(sz));
  in.read(reinterpret_cast<char *>(file.data()), sz);
  if (!in)
    return false;

  GroundWalk walk;
  walk.file = &file;
  walk_ground_node(walk, entry->offset, entry->size, 0);
  if (walk.params.size() < 3)
    return false;
  char *end = nullptr;
  const unsigned long local = std::strtoul(walk.params.c_str(), &end, 0);
  if (!end || end == walk.params.c_str() || local == 0)
    return false;
  *render_id = rpk.resolve_embedded_id(pack, static_cast<uint32_t>(local));
  if (walk.poly.empty())
    return true;

  const RpkIndex::ModelRef model = rpk.model_for_resource(
      RpkIndex::pack_from_full(*render_id), *render_id & 0xffffu);
  std::filesystem::path table;
  if (model.ok && !model.source.empty()) {
    std::string source = model.source;
    std::replace(source.begin(), source.end(), '\\', '/');
    table = std::filesystem::path(rpk.game_root()) / source;
  }
  const std::vector<GroundMat> mats = read_ground_materials(table);

  auto geo = std::make_shared<ScxGeometry>();
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
  for (const PolySection &sec : walk.poly) {
    const bool known = sec.material < mats.size();
    ScxGroup group;
    group.index_start = static_cast<uint32_t>(geo->indices.size());
    group.index_count = static_cast<uint32_t>(sec.indices.size());
    group.texture_index = known ? mats[sec.material].tex : 0xffff;
    if (known && mats[sec.material].lm >= 0) {
      group.lightmap_index = static_cast<uint16_t>(mats[sec.material].lm);
      group.lightmap_map.channel = 1;
    }
    if (known && ((mats[sec.material].flags & 1u) ||
                  (mats[sec.material].flags & 0x20000000u)))
      group.material_flags = 1;
    const uint32_t base = static_cast<uint32_t>(geo->positions.size() / 3);
    geo->positions.insert(geo->positions.end(), sec.pos.begin(), sec.pos.end());
    geo->normals.insert(geo->normals.end(), sec.nrm.begin(), sec.nrm.end());
    geo->uvs.insert(geo->uvs.end(), sec.uv0.begin(), sec.uv0.end());
    geo->uv2.insert(geo->uv2.end(), sec.uv1.begin(), sec.uv1.end());
    for (uint32_t index : sec.indices)
      geo->indices.push_back(base + index);
    geo->groups.push_back(std::move(group));
    geo->info.vertices += static_cast<int32_t>(sec.pos.size() / 3);
    geo->info.triangles += static_cast<int32_t>(sec.indices.size() / 3);
  }
  geo->info.groups = static_cast<int32_t>(geo->groups.size());
  if (geo->indices.empty())
    return true;
  std::printf("[scene] ground poly 0x%x tris=%d materials=%zu\n", *render_id,
              geo->info.triangles, mats.size());
  *poly = std::move(geo);
  return true;
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
  if (!origin || !direction)
    return nullptr;
  float best = 1e9f;
  void *hit = nullptr;
  for (const SceneInstance &instance : scene_instances_collect()) {
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
      }
    }
    if (!triangle && idx.empty() && tmin >= 0.f && tmin < best) {
      best = tmin;
      hit = instance.key;
    }
  }
  if (hit && point)
    for (int i = 0; i < 3; ++i) point[i] = origin[i] + direction[i] * best;
  return hit;
}

void scene_visit_gc_roots(const GcRootVisitor& visit) {
  visit(g_world); visit(g_sky);
}
} // namespace slrr
