// Map prop LOD (#252): stock `lod`/`lods` parsing, level selection with
// hysteresis, per-placement LOD in collection, and Valo City statistics.
// Reads the local game assets only.
#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/part_config.hpp"
#include "slrr/scene_instances.hpp"
#include "standalone_test.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>

namespace slrr {
void spawn_ground_shell(Jvm &, const RpkIndex &, Object *);
}
using namespace slrr;

namespace {
SLRR_TEST(level_selection_and_hysteresis) {
  const std::vector<float> t{.01f, .025f};
  // Without a current level: the number of thresholds reached.
  SLRR_REQUIRE(scene_prop_lod_level(-1, .005f, t) == 0);
  SLRR_REQUIRE(scene_prop_lod_level(-1, .01f, t) == 1);
  SLRR_REQUIRE(scene_prop_lod_level(-1, .03f, t) == 2);
  // From a level, a threshold must be passed by 10 %.
  SLRR_REQUIRE(scene_prop_lod_level(2, .024f, t) == 2);
  SLRR_REQUIRE(scene_prop_lod_level(2, .022f, t) == 1);
  SLRR_REQUIRE(scene_prop_lod_level(1, .0095f, t) == 1);
  SLRR_REQUIRE(scene_prop_lod_level(1, .0089f, t) == 0);
  SLRR_REQUIRE(scene_prop_lod_level(0, .0108f, t) == 0);
  SLRR_REQUIRE(scene_prop_lod_level(0, .0111f, t) == 1);
  SLRR_REQUIRE(scene_prop_lod_level(0, .03f, t) == 2);
  SLRR_REQUIRE(scene_prop_lod_level(0, .03f, {}) == 0);
  SLRR_REQUIRE(scene_prop_lod_level(1, NAN, t) == 2);
}

struct Game {
  Fixture f;
  int32_t objects = 0;
  Game() {
    const auto root = fs::absolute("game");
    f.vm.set_game_root(root.string());
    f.packs.set_root(root.string());
    f.packs.open_all();
    const auto *pack = f.packs.pack_by_path("objects.rpk");
    SLRR_REQUIRE(pack);
    objects = pack->pack_id;
  }
  uint32_t object(uint32_t local) const { return RpkIndex::make_full_id(objects, local); }
  // A map object's ICFG text, parsed like the ground props do.
  PartConfig object_config(uint32_t local) const {
    const auto bytes = f.packs.read_payload(object(local));
    SLRR_REQUIRE(bytes.size() > 8 && std::memcmp(bytes.data(), "ICFG", 4) == 0);
    const auto separator = std::find(bytes.begin() + 8, bytes.end(), 0);
    SLRR_REQUIRE(separator != bytes.end());
    const std::string cfg(separator + 1, bytes.end());
    return parse_part_config(cfg, &f.packs, objects);
  }
};

SLRR_TEST(stock_lod_parsing) {
  Game g;
  const auto lamp = g.object_config(0x18e); // lamp_base
  std::printf("lamp_base: %zu renders, lods:", lamp.renders.size());
  for (const float v : lamp.lod_sizes) std::printf(" %g", v);
  std::printf(" errors=%zu\n", lamp.errors.size());
  SLRR_REQUIRE(lamp.lod_sizes == (std::vector<float>{.01f, .025f}));
  int hi = 0, lo = 0;
  for (const auto &render : lamp.renders) {
    if ((render.resource & 0xffff) == 0x118) {
      SLRR_REQUIRE(render.lod_level == 2 && render.lod_last == 2);
      ++hi;
    }
    if ((render.resource & 0xffff) == 0x13) {
      SLRR_REQUIRE(render.lod_level == 0 && render.lod_last == 1);
      ++lo;
    }
  }
  SLRR_REQUIRE(hi == 1 && lo == 1);
  const auto shop = g.object_config(0x42); // 06x5_shop
  SLRR_REQUIRE(shop.lod_sizes == (std::vector<float>{.001f, .0025f}));
  SLRR_REQUIRE(g.object_config(0x18c).lod_sizes.empty()); // kislampa
}

SLRR_TEST(placement_lod_in_collection) {
  Game g;
  Object root, shell;
  gameref_upsert(&root);
  auto *node = gameref_upsert(&shell, GameRefKind::RenderRef);
  node->type_id = static_cast<int32_t>(g.object(0x1ad));
  node->empty = false;
  gameref_set_parent(&shell, &root);
  scene_instances_set_resources(&g.f.packs);
  scene_instances_select(&root, nullptr, false);
  // One fa03a tree at the origin: hi 0xd8 (lod 2 2), lo 0x5d5 (lod 0 1),
  // lods 2 0.01 0.025.
  GroundMapData map;
  map.render_id = static_cast<uint32_t>(node->type_id);
  GroundMapData::Placement tree;
  tree.type = g.object(0x182);
  tree.seed = 0;
  map.placements.push_back(tree);
  scene_instances_add_ground_props(map);
  SLRR_REQUIRE(scene_instances_lod_placements() == 1);
  // hi = objects 0xd8, lo = objects 0x5d5 (told apart by mesh path below).
  const auto drawn = [&](float distance, bool update, bool force = false) {
    ScenePropLodView view;
    view.eye = {0.f, 2.f, distance};
    view.update = update;
    view.valid = true;
    view.force_highest = force;
    scene_instances_set_lod_view(view);
    // Single props report their resource id; batches carry their mesh path.
    std::vector<SceneBatch> batches;
    scene_instances_collect_batches(nullptr, batches);
    std::set<uint32_t> types;
    for (const auto &instance : scene_instances_collect(nullptr))
      if (instance.key != &shell && instance.type_id) types.insert(static_cast<uint32_t>(instance.type_id));
    std::vector<std::string> paths;
    for (const auto &batch : batches)
      if (!batch.instances.empty()) paths.push_back(batch.geometry->info.path);
    return std::pair{types, paths};
  };
  // Find which batch path belongs to which LOD from the two extremes.
  const auto near_set = drawn(3.f, true);
  const auto far_set = drawn(3000.f, true);
  std::printf("tree near: %zu instances %zu batches (%s); far: %zu instances %zu batches (%s)\n",
              near_set.first.size(), near_set.second.size(),
              near_set.second.empty() ? "-" : near_set.second.front().c_str(), far_set.first.size(),
              far_set.second.size(), far_set.second.empty() ? "-" : far_set.second.front().c_str());
  const auto one = [](const std::pair<std::set<uint32_t>, std::vector<std::string>> &s) {
    return s.first.size() + s.second.size() == 1;
  };
  SLRR_REQUIRE(one(near_set) && one(far_set));
  const auto label = [](const std::pair<std::set<uint32_t>, std::vector<std::string>> &s) {
    return s.second.empty() ? std::to_string(*s.first.begin()) : s.second.front();
  };
  const std::string hi_label = label(near_set), lo_label = label(far_set);
  SLRR_REQUIRE(hi_label != lo_label);

  // Walk away and back: exactly one LOD at every step, switching once each
  // way, farther on the way out than on the way back (hysteresis).
  float out_switch = 0.f, in_switch = 0.f;
  std::string previous = hi_label;
  drawn(3.f, true);
  for (float d = 3.f; d <= 1500.f; d *= 1.01f) {
    const auto s = drawn(d, true);
    SLRR_REQUIRE(one(s));
    const auto now = label(s);
    if (now != previous) {
      SLRR_REQUIRE(out_switch == 0.f && now == lo_label);
      out_switch = d;
    }
    previous = now;
  }
  for (float d = 1500.f; d >= 3.f; d /= 1.01f) {
    const auto s = drawn(d, true);
    SLRR_REQUIRE(one(s));
    const auto now = label(s);
    if (now != previous) {
      SLRR_REQUIRE(in_switch == 0.f && now == hi_label);
      in_switch = d;
    }
    previous = now;
  }
  std::printf("tree LOD switch: out at %.1f m, back at %.1f m\n", out_switch, in_switch);
  SLRR_REQUIRE(out_switch > in_switch && in_switch > 0.f);
  SLRR_REQUIRE(out_switch / in_switch > 1.15f && out_switch / in_switch < 1.3f);
  // Non-updating passes reuse the stored level; force_highest shows hi.
  drawn(3000.f, true);
  SLRR_REQUIRE(label(drawn(3.f, false)) == lo_label);
  SLRR_REQUIRE(label(drawn(3000.f, false, true)) == hi_label);
  scene_instances_set_lod_view({});
  SLRR_REQUIRE(label(drawn(3000.f, false, false)) == lo_label);
  scene_instances_set_lod_view({});
  gameref_remove(&shell);
  gameref_remove(&root);
  scene_instances_set_resources(nullptr);
}

SLRR_TEST(valo_city_lod_statistics) {
  Game g;
  scene_instances_set_resources(&g.f.packs);
  const auto pack = g.f.packs.open_lib("maps/city.rpk");
  const auto ground_id = RpkIndex::make_full_id(pack, 1);
  Object *root = g.f.vm.new_object("java.util.resource.GroundRef");
  gameref_upsert(root, GameRefKind::GroundRef)->type_id = static_cast<int32_t>(ground_id);
  spawn_ground_shell(g.f.vm, g.f.packs, root);
  scene_instances_select(root, nullptr, false);
  // Eye at the first ground shell origin, 2 m up: whole-city prop totals.
  float frame[16];
  SLRR_REQUIRE(scene_instances_ground_frame(root, frame));
  const auto totals = [&](bool force) {
    ScenePropLodView view;
    view.eye = {frame[12], frame[13] + 2.f, frame[14]};
    view.update = view.valid = true;
    view.force_highest = force;
    scene_instances_set_lod_view(view);
    uint64_t triangles = 0, placements = 0;
    std::vector<SceneBatch> batches;
    scene_instances_collect_batches(nullptr, batches);
    for (const auto &batch : batches) {
      triangles += uint64_t(batch.geometry->info.triangles) * batch.instances.size();
      placements += batch.instances.size();
    }
    for (const auto &instance : scene_instances_collect(nullptr))
      if (instance.geometry && !instance.geometry->ground_polygons) {
        triangles += uint64_t(instance.geometry->info.triangles);
        ++placements;
      }
    return std::pair{triangles, placements};
  };
  const auto highest = totals(true), lod = totals(false);
  std::printf("Valo City: %zu placements with a LOD chain; whole-city prop triangles %llu -> %llu "
              "(drawn records %llu -> %llu)\n",
              scene_instances_lod_placements(), (unsigned long long)highest.first,
              (unsigned long long)lod.first, (unsigned long long)highest.second,
              (unsigned long long)lod.second);
  SLRR_REQUIRE(scene_instances_lod_placements() > 0 && lod.first < highest.first);
  {
    // #257: main-view collection time (instances + batches, LOD on) with a
    // street-level 60-degree frustum looking along +x (D3D depth 0..1).
    // Planes in render space (script Z flipped), inward normals, through the eye.
    const std::array<float, 3> eye{frame[12], frame[13] + 2.f, -frame[14]};
    const float h = std::atan(16.f / 9.f * std::tan(30.f * 3.14159265f / 180.f));
    const float v = 30.f * 3.14159265f / 180.f;
    const std::array<std::array<float, 3>, 4> sides{{{std::sin(h), 0.f, -std::cos(h)},
                                                     {std::sin(h), 0.f, std::cos(h)},
                                                     {std::sin(v), -std::cos(v), 0.f},
                                                     {std::sin(v), std::cos(v), 0.f}}};
    SceneFrustum frustum;
    for (size_t i = 0; i < 4; ++i)
      frustum[i] = {sides[i][0], sides[i][1], sides[i][2],
                    -(sides[i][0] * eye[0] + sides[i][1] * eye[1] + sides[i][2] * eye[2])};
    frustum[4] = {1.f, 0.f, 0.f, -(eye[0] + .05f)};
    frustum[5] = {-1.f, 0.f, 0.f, eye[0] + 5000.f};
    const auto time_collection = [&](bool force_highest) {
      ScenePropLodView view_lod;
      view_lod.eye = {frame[12], frame[13] + 2.f, frame[14]};
      view_lod.valid = true;
      view_lod.force_highest = force_highest;
      size_t records = 0;
      constexpr int runs = 60;
      double total = 0, checksum = 0;
      // As in the renderer: a frame cache around each frame and one batch
      // vector kept across frames (LibBgfxBackend::prop_batches_).
      std::vector<SceneBatch> batches;
      for (int i = 0; i < runs; ++i) {
        view_lod.update = true;  // one main-view collection per frame
        scene_instances_frame_cache(true);
        scene_instances_set_lod_view(view_lod);
        const auto start = std::chrono::steady_clock::now();
        // Renderer order (#260): batches, then instances.
        scene_instances_collect_batches(&frustum, batches);
        const auto instances = scene_instances_collect(&frustum);
        total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        scene_instances_frame_cache(false);
        records = instances.size();
        checksum = 0;
        for (const auto &instance : instances) checksum += instance.transform[12] + instance.transform[14];
        for (const auto &batch : batches) {
          records += batch.instances.size();
          for (const auto &placement : batch.instances)
            checksum += placement.transform[12] + 2.0 * placement.transform[14] + placement.transform[0];
        }
      }
      std::printf("  collection checksum %.6f\n", checksum);
      return std::pair{total / runs, records};
    };
    for (int round = 0; round < 2; ++round) {
      const auto highest_only = time_collection(true), with_lod = time_collection(false);
      std::printf("Valo City main-view collection: highest LOD only %.3f ms (%zu records), "
                  "LOD on %.3f ms (%zu records)\n", highest_only.first, highest_only.second,
                  with_lod.first, with_lod.second);
    }
  }
  // Far size cull as the renderer applies it at 2560x1440 (60 degree FOV):
  // projected box = radius * cot(30 deg) * 1440 / distance, everything kept
  // within 40 m. Distance stands in for view depth (all directions).
  struct Cull { std::array<float, 3> eye; float px; };
  static Cull cull;
  cull.eye = {frame[12], frame[13] + 2.f, frame[14]};
  const auto filter = [](const float center[3], float radius, void *) {
    const float d = std::hypot(center[0] - cull.eye[0], center[1] - cull.eye[1], center[2] - cull.eye[2]);
    if (d - radius < 40.f) return true;
    return radius * 1.7320508f * 1440.f / d >= cull.px;
  };
  for (const float px : {2.f, 4.f}) {
    cull.px = px;
    scene_instances_set_size_filter(filter, nullptr);
    const auto culled = totals(false);
    scene_instances_set_size_filter(nullptr, nullptr);
    std::printf("Valo City with LOD and far cull %.0f px: triangles %llu, drawn records %llu\n", px,
                (unsigned long long)culled.first, (unsigned long long)culled.second);
  }
  scene_instances_set_lod_view({});
  for (auto *child : std::vector<void *>(gameref_get(root)->children)) gameref_remove(child);
  gameref_remove(root);
  scene_instances_set_resources(nullptr);
}
} // namespace

int main() {
  const standalone::Test tests[] = {level_selection_and_hysteresis, stock_lod_parsing,
                                    placement_lod_in_collection, valo_city_lod_statistics};
  return standalone::run("prop_lod", tests);
}
