#include "slrr/scene_instances.hpp"
#include "slrr/gameref.hpp"
#include "slrr/rpk_index.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
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
const RpkIndex* g_resources = nullptr;
void* g_world = nullptr;
void* g_sky = nullptr;
uint32_t g_environment = 0;
std::unordered_map<uint32_t, std::shared_ptr<const ScxGeometry>> g_meshes;
std::unordered_map<uint32_t, std::shared_ptr<const ScxGeometry>> g_overrides;

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
  auto over = g_overrides.find(id);
  if (over != g_overrides.end()) return over->second;
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

uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
float rdf(const uint8_t* p) {
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
  const std::vector<uint8_t>* file = nullptr;
  std::string params;
  std::vector<PolySection> poly;
};

void take_params(GroundWalk& w, const uint8_t* data, const uint8_t* end) {
  if (!w.params.empty() || !data) return;
  const uint8_t* s = data;
  while (s < end) {
    const uint8_t* line = s;
    while (s < end && *s != '\n' && *s != '\r' && *s != 0) ++s;
    std::string text(reinterpret_cast<const char*>(line),
                     static_cast<size_t>(s - line));
    while (s < end && (*s == '\n' || *s == '\r')) ++s;
    size_t a = 0;
    while (a < text.size() && (text[a] == ' ' || text[a] == '\t')) ++a;
    if (text.size() < a + 6) continue;
    std::string key = text.substr(a, 6);
    for (char& c : key)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key != "params") continue;
    if (a + 6 < text.size() && text[a + 6] != ' ' && text[a + 6] != '\t')
      continue;
    size_t v = a + 6;
    while (v < text.size() && (text[v] == ' ' || text[v] == '\t')) ++v;
    size_t e = v;
    while (e < text.size() && text[e] != ' ' && text[e] != '\t' &&
           text[e] != ',')
      ++e;
    if (e > v) w.params = text.substr(v, e - v);
  }
}

void walk_ground_node(GroundWalk& w, uint32_t offset, uint32_t size,
                      int depth) {
  const std::vector<uint8_t>& file = *w.file;
  if (size < 8 || offset > file.size() || size > file.size() - offset) return;
  const uint8_t* base = file.data() + offset;
  const uint8_t* end = base + size;
  const uint32_t n0 = rd32(base);
  if (n0 > (size - 8) / 4) return;
  const uint8_t* p = base + 4 + 4 * static_cast<size_t>(n0);
  if (end - p < 4) return;
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
    const uint8_t* data = p + 8;
    if (tag == 0 && csize == 0) {
      if (end - p < 12) break;
      take_params(w, p + 12, end);
      break;
    }
    if (csize > static_cast<size_t>(end - data)) break;
    if (tag == 0x00445352u) {
      take_params(w, data, data + csize);
    } else if (tag == 0x594C4F50u && csize >= 8) {
      const uint32_t nsec = rd32(data + 4);
      const uint8_t* q = data + 8;
      const uint8_t* cend = data + csize;
      for (uint32_t s = 0; s < nsec; ++s) {
        if (cend - q < 12) break;
        const uint32_t mat = rd32(q);
        const uint32_t nv = rd32(q + 4);
        const uint32_t nt = rd32(q + 8);
        q += 12;
        if (nv > 200000u || nt > 200000u) break;
        const uint64_t need =
            static_cast<uint64_t>(nv) * 56u + static_cast<uint64_t>(nt) * 12u;
        if (need > static_cast<uint64_t>(cend - q)) break;
        PolySection sec;
        sec.material = mat;
        sec.pos.resize(static_cast<size_t>(nv) * 3u);
        sec.nrm.resize(sec.pos.size());
        sec.uv0.resize(static_cast<size_t>(nv) * 2u);
        sec.uv1.resize(sec.uv0.size());
        for (uint32_t vi = 0; vi < nv; ++vi) {
          const uint8_t* v = q + static_cast<size_t>(vi) * 56u;
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
          if (sec.indices[ii] >= nv) oob = true;
        }
        q += static_cast<size_t>(nt) * 12u;
        if (!oob && nv && nt) w.poly.push_back(std::move(sec));
      }
    }
    p = data + csize;
  }
  if (depth >= 64) return;
  for (const Kid& k : kids)
    if (k.restype == 1) walk_ground_node(w, k.off, k.sz, depth + 1);
}

struct GroundMat {
  uint16_t tex = 0xffff;
  int16_t lm = -1;
  uint32_t flags = 0;
};

std::vector<GroundMat> read_ground_materials(const std::filesystem::path& path) {
  std::vector<GroundMat> mats;
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return mats;
  const auto sz = static_cast<size_t>(in.tellg());
  if (sz < 16) return mats;
  in.seekg(0);
  std::vector<uint8_t> buf(sz);
  in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
  if (!in || std::memcmp(buf.data(), "INVO", 4) != 0 || rd32(buf.data() + 4) != 3)
    return {};
  size_t p = 8;
  while (p + 4 <= buf.size() && mats.size() < 4096) {
    const uint32_t hsz = rd32(buf.data() + p);
    if (hsz == 0) break;
    if (hsz < 46 || p + hsz + 8 > buf.size()) break;
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
}  // namespace

void scene_instances_set_resources(const RpkIndex* resources) {
  g_resources = resources;
  g_world = g_sky = nullptr;
  g_meshes.clear();
  g_overrides.clear();
  g_environment = 0;
}
void scene_instances_set_envmap(uint32_t resource) { g_environment=resource; }
std::string scene_instances_envmap() {
  return g_resources ? g_resources->texture_path_for_id(g_environment) : std::string{};
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
    float local[16];
    if(n->matrix_override)std::copy(n->local_matrix,n->local_matrix+16,local);
    else local_matrix(*n, local);
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
    instance.vehicle_material = n->vehicle_material;
    if(n->paint_source) {
      auto normalize=[](std::string path){
        std::replace(path.begin(),path.end(),'\\','/');
        for(char& c:path)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return path;
      };
      const auto from=normalize(g_resources->texture_path_for_id(n->paint_source));
      for(size_t i=0; !from.empty() && i<instance.geometry->textures.size(); ++i) {
        if(normalize(instance.geometry->textures[i])==from) {instance.paint_slot=static_cast<int>(i);break;}
      }
      if(n->paint_texture)instance.paint_texture=g_resources->texture_path_for_id(n->paint_texture);
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
  if (id && geo) g_overrides[id] = std::move(geo);
}

bool scene_instances_ground_map(const RpkIndex& rpk, uint32_t ground_id,
                                uint32_t* render_id,
                                std::shared_ptr<const ScxGeometry>* poly) {
  if (!render_id || !poly) return false;
  *render_id = 0;
  poly->reset();
  const RpkEntry* entry = rpk.resolve(ground_id);
  if (!entry || entry->typeof_ != 1 || entry->size < 8) return false;
  const int32_t pack = RpkIndex::pack_from_full(ground_id);
  const std::string disk = rpk.disk_path_for_pack(pack);
  std::ifstream in(disk, std::ios::binary | std::ios::ate);
  if (!in) return false;
  const auto sz = static_cast<std::streamoff>(in.tellg());
  if (sz <= 0) return false;
  in.seekg(0);
  std::vector<uint8_t> file(static_cast<size_t>(sz));
  in.read(reinterpret_cast<char*>(file.data()), sz);
  if (!in) return false;

  GroundWalk walk;
  walk.file = &file;
  walk_ground_node(walk, entry->offset, entry->size, 0);
  if (walk.params.size() < 3) return false;
  char* end = nullptr;
  const unsigned long local = std::strtoul(walk.params.c_str(), &end, 0);
  if (!end || end == walk.params.c_str() || local == 0) return false;
  *render_id = rpk.resolve_embedded_id(pack, static_cast<uint32_t>(local));
  if (walk.poly.empty()) return true;

  const RpkIndex::ModelRef model =
      rpk.model_for_resource(RpkIndex::pack_from_full(*render_id),
                             *render_id & 0xffffu);
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
    for (auto& texture : geo->textures) {
      std::replace(texture.begin(), texture.end(), '\\', '/');
      if (!texture.empty() && std::filesystem::path(texture).is_relative())
        texture = (std::filesystem::path(rpk.game_root()) / texture).string();
    }
  }
  for (const PolySection& sec : walk.poly) {
    const bool known = sec.material < mats.size();
    ScxGroup group;
    group.index_start = static_cast<uint32_t>(geo->indices.size());
    group.index_count = static_cast<uint32_t>(sec.indices.size());
    group.texture_index = known ? mats[sec.material].tex : 0xffff;
    if (known && mats[sec.material].lm >= 0) {
      group.lightmap_index = static_cast<uint16_t>(mats[sec.material].lm);
      group.lightmap_map.channel = 1;
    }
    if (known &&
        ((mats[sec.material].flags & 1u) ||
         (mats[sec.material].flags & 0x20000000u)))
      group.material_flags = 1;
    const uint32_t base = static_cast<uint32_t>(geo->positions.size() / 3);
    geo->positions.insert(geo->positions.end(), sec.pos.begin(), sec.pos.end());
    geo->normals.insert(geo->normals.end(), sec.nrm.begin(), sec.nrm.end());
    geo->uvs.insert(geo->uvs.end(), sec.uv0.begin(), sec.uv0.end());
    geo->uv2.insert(geo->uv2.end(), sec.uv1.begin(), sec.uv1.end());
    for (uint32_t index : sec.indices) geo->indices.push_back(base + index);
    geo->groups.push_back(std::move(group));
    geo->info.vertices += static_cast<int32_t>(sec.pos.size() / 3);
    geo->info.triangles += static_cast<int32_t>(sec.indices.size() / 3);
  }
  geo->info.groups = static_cast<int32_t>(geo->groups.size());
  if (geo->indices.empty()) return true;
  std::printf("[scene] ground poly 0x%x tris=%d materials=%zu\n", *render_id,
              geo->info.triangles, mats.size());
  *poly = std::move(geo);
  return true;
}
}  // namespace slrr
