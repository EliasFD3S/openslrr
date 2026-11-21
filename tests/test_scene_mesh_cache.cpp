#include "../src/render/bgfx_lib_backend.hpp"
#include "../src/render/scene_mesh_data.hpp"
#include "save_io_fixture.hpp"
using namespace slrr;

static ScxGeometry fixture() {
  ScxGeometry g;
  g.info.ok = true;
  g.info.version = 4;
  g.info.groups = 3;
  g.position_scale = 1;
  g.ground_polygons = true;
  g.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  g.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
  g.uvs = {0, 0, 1, 0, 0, 1};
  g.uv2 = g.uvs;
  g.uv3 = g.uvs;
  g.vertex_colors = {0xffffffff, 0xff00ff00, 0xffff0000};
  g.skin_weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
  g.skin_bones.resize(12);
  g.indices = {0, 1, 2, 2, 1, 0, 1, 2, 0};
  for (uint32_t i = 0; i < 3; ++i) {
    ScxGroup group;
    group.index_start = i * 3;
    group.index_count = 3;
    group.texture_index = i == 1 ? 1 : 0;
    g.groups.push_back(group);
  }
  return g;
}
static void metadata_and_materials() {
  auto g = fixture();
  const auto original = g.indices;
  auto plain = prepare_scene_mesh(g, false);
  SLRR_REQUIRE(plain.metadata.index_count == 9 &&
               plain.metadata.vertex_count == 3);
  SLRR_REQUIRE(&plain.indices(g) == &g.indices && plain.merged_indices.empty());
  auto merged = prepare_scene_mesh(g, true);
  SLRR_REQUIRE(merged.metadata.groups.size() == 2);
  SLRR_REQUIRE(merged.indices(g) ==
               std::vector<uint32_t>({0, 1, 2, 1, 2, 0, 2, 1, 0}));
  SLRR_REQUIRE(merged.metadata.groups[0].index_count == 6 &&
               merged.metadata.groups[1].index_start == 6);
  SLRR_REQUIRE(g.indices == original && g.groups.size() == 3);
  for (auto version : {3u, 4u}) {
    g.info.version = version;
    g.groups[0].material_flags = version == 3 ? 0x101 : 0x40000000;
    auto blend = prepare_scene_mesh(g, true);
    SLRR_REQUIRE(blend.merged_indices.empty() &&
                 blend.metadata.groups.size() == 3);
    g.groups[0].material_flags = 0;
  }
  g.groups[1].index_count = 100;
  SLRR_REQUIRE(prepare_scene_mesh(g, true).merged_indices.empty());
}
static void shadow_material_ranges() {
  SceneMeshData implicit;implicit.index_count=3;
  const auto implicit_ranges=shadow_draw_groups(implicit);
  SLRR_REQUIRE(implicit_ranges.size()==1 && implicit_ranges[0].index_count==3);
  ScxGeometry g;g.info.version=4;
  for(unsigned i=0;i<8;++i) {
    ScxGroup group;group.index_start=i*3;group.index_count=3;group.texture_index=uint16_t(i);
    if(i==2) group.material_flags=0x40000000u;
    if(i>=4){group.material_flags=0x20000000u;group.texture_index=2;group.alpha_reference=i<6?128:64;}
    if(i==7) group.base_map.channel=1;
    g.groups.push_back(group);
  }
  const auto ranges=shadow_draw_groups(SceneMeshData(g));
  SLRR_REQUIRE(ranges.size()==5);
  SLRR_REQUIRE(ranges[0].index_start==0 && ranges[0].index_count==6);
  SLRR_REQUIRE(ranges[1].index_start==9 && ranges[1].index_count==3);
  SLRR_REQUIRE(ranges[2].index_start==12 && ranges[2].index_count==6 && ranges[2].alpha_reference==128);
  SLRR_REQUIRE(ranges[3].alpha_reference==64 && ranges[4].base_map.channel==1);
  SLRR_REQUIRE(g.groups.size()==8 && g.groups[0].index_count==3);
}
static size_t payload_bytes(const ScxGeometry &g) {
  return (g.positions.size() + g.normals.size() + g.uvs.size() + g.uv2.size() +
          g.uv3.size() + g.skin_weights.size()) *
             sizeof(float) +
         (g.indices.size() + g.vertex_colors.size()) * sizeof(uint32_t) +
         g.skin_bones.size() * sizeof(uint16_t);
}
static void real_asset_metadata() {
  Fixture f;
  f.packs.set_root(fs::absolute("game").string());
  f.packs.open_all();
  scene_instances_set_resources(&f.packs);
  size_t saved = 0;
  const auto city = f.packs.open_lib("maps/city.rpk");
  const auto map =
      scene_instances_ground_map(f.packs, RpkIndex::make_full_id(city, 1));
  auto source = map.poly;
  SLRR_REQUIRE(source && source->info.ok && !source->indices.empty());
  auto prepared = prepare_scene_mesh(*source, true);
  SLRR_REQUIRE(prepared.metadata.vertex_count == source->positions.size() / 3);
  saved += payload_bytes(*source);
  for (const char *path :
       {"game/cars/racers/Baiern_data/meshes/chassis.scx",
        "game/objects/meshes/area_205_egyedi/area_205_egyedi.scx"}) {
    auto mesh = load_scx_geometry(path);
    SLRR_REQUIRE(mesh.info.ok);
    const auto *positions = mesh.positions.data();
    const auto indices = mesh.indices;
    auto upload = prepare_scene_mesh(mesh, true);
    SLRR_REQUIRE(mesh.positions.data() == positions && mesh.indices == indices);
    SLRR_REQUIRE(upload.metadata.index_count == upload.indices(mesh).size());
    saved += payload_bytes(mesh);
  }
  std::printf(
      "real assets: %zu bytes of duplicate vertex/index/skin arrays avoided\n",
      saved);
  SLRR_REQUIRE(saved > 1024 * 1024);
  scene_instances_set_resources(nullptr);
}
namespace slrr {
struct SceneMeshCacheRegression {
  static void gpu(LibBgfxBackend &b) {
    auto source = std::make_shared<ScxGeometry>(fixture());
    const auto *vertices = source->positions.data();
    auto settle = []() {
      for (int i = 0; i < 8; ++i)
        bgfx::frame();
    };
    settle();
    const auto base_vb = bgfx::getStats()->numVertexBuffers;
    const auto base_ib = bgfx::getStats()->numIndexBuffers;
    b.cache_scene_mesh(source, nullptr, true);
    auto &part = b.instance_meshes_.at(source.get());
    SLRR_REQUIRE(part.source.get() == source.get() &&
                 source->positions.data() == vertices);
    SLRR_REQUIRE(part.geo.groups.size() == 2 && part.geo.index_count == 9);
    b.upload_skin(part);
    settle();
    SLRR_REQUIRE(bgfx::isValid(part.vbh) && bgfx::isValid(part.skin_vbh) &&
                 bgfx::isValid(part.ibh));
    SLRR_REQUIRE(bgfx::getStats()->numVertexBuffers == base_vb + 2 &&
                 bgfx::getStats()->numIndexBuffers == base_ib + 1);
    for (int i = 0; i < 16; ++i) {
      source->positions[0] = float(i);
      source->normals[0] = .25f;
      source->uv2[0] = .5f;
      source->skin_weights[0] = .75f;
      source->indices = {0, 2, 1, 1, 2, 0};
      source->groups.clear();
      ++source->revision;
      b.cache_scene_mesh(source, nullptr, true);
      b.upload_skin(part);
      settle();
      SLRR_REQUIRE(part.revision == source->revision &&
                   part.geo.index_count == 6 && part.geo.groups.empty() &&
                   part.source.get() == source.get());
      SLRR_REQUIRE(bgfx::getStats()->numVertexBuffers == base_vb + 2 &&
                   bgfx::getStats()->numIndexBuffers == base_ib + 1);
    }
    // Replaced deformed sources retain their already-loaded texture bindings.
    Object owner;
    auto *node = gameref_upsert(&owner, GameRefKind::RenderRef);
    auto old_source = std::make_shared<ScxGeometry>(fixture());
    node->deformed_geometry = old_source;
    b.cache_scene_mesh(old_source, &owner, false);
    b.instance_meshes_.at(old_source.get()).tex = {b.white_tex_};
    b.deformed_meshes_[&owner] = old_source.get();
    auto replacement = std::make_shared<ScxGeometry>(*old_source);
    replacement->positions[0] = 2;
    node->deformed_geometry = replacement;
    b.cache_scene_mesh(replacement, &owner, false);
    SLRR_REQUIRE(b.instance_meshes_.at(old_source.get()).tex.empty());
    SLRR_REQUIRE(b.instance_meshes_.at(replacement.get()).tex.size() == 1);
    SLRR_REQUIRE(b.instance_meshes_.at(replacement.get()).source.get() ==
                 replacement.get());
    b.clear_scene();
    gameref_remove(&owner);
    settle();
    SLRR_REQUIRE(source.use_count() == 1);
    SLRR_REQUIRE(bgfx::getStats()->numVertexBuffers == base_vb &&
                 bgfx::getStats()->numIndexBuffers == base_ib);
  }
};
} // namespace slrr
int main() {
  LibBgfxBackend backend;
  try {
    metadata_and_materials();
    shadow_material_ranges();
    real_asset_metadata();
    SLRR_REQUIRE(backend.open(640, 480, "Shared mesh cache regression"));
    ShowWindow(static_cast<HWND>(backend.native_hwnd()), SW_HIDE);
    SceneMeshCacheRegression::gpu(backend);
    backend.close();
    std::puts("PASS source/index ownership, material ordering/blends, skin + "
              "16 revisions, GPU cleanup");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "scene_mesh_cache: %s\n", e.what());
    if (backend.ready())
      backend.close();
    return 1;
  }
}
