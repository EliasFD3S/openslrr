#include "slrr/gameref.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/scene_instances.hpp"
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace slrr;
int main(int argc, char **argv) {
  int failures = 0;
  auto check = [&](bool ok, const char *name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
      ++failures;
  };
  auto near = [](float a, float b) { return std::fabs(a - b) < 0.0001f; };
  int root, parent, child, other;
  gameref_upsert(&root, GameRefKind::GroundRef);
  gameref_upsert(&parent);
  gameref_upsert(&child, GameRefKind::RenderRef);
  gameref_upsert(&other, GameRefKind::GroundRef);
  gameref_set_parent(&parent, &root);
  gameref_set_parent(&child, &parent);
  gameref_set_matrix(&parent, 1, 2, 3, 0, 0, 0);
  gameref_set_matrix(&child, 4, 5, 6, 0, 0, 0);
  float m[16];
  check(scene_instance_transform(&child, &root, m) && near(m[12], 5) &&
            near(m[13], 7) && near(m[14], -9),
        "parent translation and renderer Z convention");
  gameref_set_matrix(&parent, 0, 0, 0, 1.57079632679f, 0, 0);
  gameref_set_matrix(&child, 0, 0, 2, 0, 0, 0);
  check(scene_instance_transform(&child, &root, m) && near(m[12], 2) &&
            near(m[14], 0),
        "parent yaw rotates child translation");
  check(!scene_instance_transform(&child, &other, m),
        "foreign world is excluded");
  gameref_set_flags(&parent, kWorldTreeLeaf);
  check(!scene_instance_transform(&child, &root, m),
        "hidden parent hides descendants");
  gameref_clear_flags(&parent, kWorldTreeLeaf);
  gameref_set_parent(&parent, &child);
  check(!scene_instance_transform(&child, &root, m), "cycles are rejected");
  gameref_set_parent(&parent, &root);
  gameref_remove(&parent);
  check(!scene_instance_transform(&child, &root, m),
        "destroyed parent removes subtree from scene");
  gameref_set_parent(&child, &root);
  check(scene_instance_transform(&child, &root, m),
        "reparenting restores instance");
  gameref_set_flags(&child, kWorldTreeRoot);
  check(!scene_instance_transform(&child, &root, m),
        "nested world roots isolate inventory scenes");
  gameref_clear_flags(&child, kWorldTreeRoot);
  gameref_remove(&child);
  check(!scene_instance_transform(&child, &root, m),
        "destroyed instance disappears");

  ScxGeometry ramp;
  ramp.info.ok = true;
  ramp.position_scale = 1;
  ramp.positions = {0, -3, 0, 2, -2.6f, 0, 2, -2.6f, 2, 0, -3, 2};
  ramp.indices = {0, 1, 2, 0, 2, 3};
  scene_instances_add_static_mesh(ramp);
  SceneFloorHit support;
  check(scene_instances_sample_floor(1, 1, 0, nullptr, support) &&
            near(support.height, -2.8f) && support.normal[0] < -.19f &&
            support.normal[1] > .98f,
        "physics terrain returns the real negative-height triangle and its "
        "upward normal");
  check(!scene_instances_sample_floor(4, 4, 0, nullptr, support),
        "missing terrain does not become an implicit floor");
  check(!scene_instances_sample_floor(1, 1, -4, nullptr, support),
        "floor sampler rejects surfaces above the ray ceiling");
  scene_instances_enable_static_floor(false);
  check(!scene_instances_sample_floor(1, 1, 0, nullptr, support),
        "garage preload does not create phantom ground in another map");
  scene_instances_enable_static_floor(true);

  float receiver_frame[16]{};
  receiver_frame[0] = receiver_frame[5] = receiver_frame[10] =
      receiver_frame[15] = 1;
  receiver_frame[12] = 2;
  receiver_frame[14] = 3;
  SceneFloorHit receiver;
  receiver.height = -2;
  receiver.normal[0] = -.2f;
  receiver.normal[1] = 1;
  receiver.normal[2] = .1f;
  float projection[16];
  check(scene_shadow_projection(receiver_frame, receiver, projection),
        "vehicle shadow receiver accepts a sloping real floor");
  bool projected = true;
  for (const std::array<float, 3> point :
       {std::array<float, 3>{2, 1, -3}, std::array<float, 3>{3, 2, -4},
        std::array<float, 3>{1, 0, -2}}) {
    float p[3]{};
    for (int r = 0; r < 3; ++r)
      p[r] = projection[r] * point[0] + projection[4 + r] * point[1] +
             projection[8 + r] * point[2] + projection[12 + r];
    const float plane = -.2f * 2 - 2 + .1f * 3 + .012f;
    projected =
        projected && std::fabs(-.2f * p[0] + p[1] - .1f * p[2] - plane) < 1e-5;
  }
  check(projected, "shadow projection stays on the receiver with renderer Z "
                   "reflection and negative ground height");

  // RenderRef terrain takes the dealer path, rather than the boot preload.
  int terrain;
  gameref_upsert(&terrain, GameRefKind::RenderRef)->type_id = 42;
  gameref_set_parent(&terrain, &root);
  scene_instances_override_mesh(42, std::make_shared<ScxGeometry>(ramp));
  scene_instances_enable_static_floor(false);
  scene_instances_select(&root, nullptr);
  scene_instances_prepare_physics();
  const auto builds = scene_instances_floor_builds();
  check(scene_instances_sample_floor(1, 1, 0, nullptr, support) &&
            near(support.height, -2.8f),
        "indexed RenderRef terrain preserves surface height");
  for (int i = 0; i < 60; ++i) {
    scene_instances_prepare_physics();
    scene_instances_sample_floor(1, 1, 0, nullptr, support);
  }
  check(scene_instances_floor_builds() == builds,
        "unchanged dealer terrain is not rebuilt each frame or contact query");
  gameref_set_matrix(&terrain, 0, 1, 0, 0, 0, 0);
  scene_instances_prepare_physics();
  check(scene_instances_floor_builds() == builds + 1 &&
            scene_instances_sample_floor(1, 1, 0, nullptr, support) &&
            near(support.height, -1.8f),
        "moving terrain refreshes its world-space index");
  check(!scene_instances_sample_floor(1, 1, 0, &root, support),
        "cached terrain retains descendant exclusion");
  gameref_remove(&terrain);
  scene_instances_prepare_physics();
  check(!scene_instances_sample_floor(1, 1, 0, nullptr, support),
        "destroyed terrain leaves no stale contacts");
  scene_instances_enable_static_floor(true);

  // Optional original-data integration test; no assets bundled in the tests.
  if (argc > 1) {
    RpkIndex resources;
    resources.set_root(argv[1]);
    resources.open_all();
    scene_instances_set_resources(&resources);
    const auto *sky = resources.pack_by_name("maps/skydome.rpk");
    check(sky != nullptr, "original skydome RPK resolves");
    if (sky) {
      int sky1, sky2, ui;
      const auto rid = RpkIndex::make_full_id(sky->pack_id, 0x23);
      for (void *key :
           {static_cast<void *>(&sky1), static_cast<void *>(&sky2)}) {
        gameref_upsert(key, GameRefKind::RenderRef)->type_id = rid;
        gameref_set_parent(key, &root);
      }
      gameref_upsert(&ui, GameRefKind::RenderRef)->type_id = rid;
      gameref_set_parent(&ui, &other);
      scene_instances_select(&root, &sky1);
      auto first = scene_instances_collect();
      check(first.size() == 2,
            "only selected world's render instances collected");
      if (first.size() == 2) {
        check(first[0].geometry == first[1].geometry,
              "mesh shared across instances");
        check(first[0].sky != first[1].sky,
              "script-selected sky distinguished from geometry");
        check(!first[0].geometry->textures.empty(),
              "skydome textures resolved from RPK");
        bool exists = !first[0].geometry->textures.empty();
        for (const auto &texture : first[0].geometry->textures)
          exists = exists && std::filesystem::exists(texture);
        check(exists, "texture paths refer to existing assets, including PTX");
      }
      gameref_remove(&sky1);
      auto second = scene_instances_collect();
      check(second.size() == 1 && !second[0].sky,
            "destroy removes sky instance");
      gameref_get(&sky2)->type_id = RpkIndex::make_full_id(sky->pack_id, 0x1d);
      auto third = scene_instances_collect();
      check(third.size() == 1 && !first.empty() &&
                third[0].geometry != first[0].geometry,
            "resource replacement selects new geometry");
      int sky3;
      scene_instances_select(&root, &sky2);
      gameref_upsert(&sky3, GameRefKind::RenderRef)->type_id = rid;
      gameref_set_parent(&sky3, &root);
      scene_instances_select(&root, &sky3);
      auto replaced = scene_instances_collect();
      check(replaced.size() == 1 && replaced[0].key == &sky3 && replaced[0].sky,
            "superseded skydome does not accumulate as foreground geometry");
      scene_instances_select(nullptr, nullptr);
      check(scene_instances_collect().empty(),
            "leaving world clears visible instances");
    }
    if (const auto *frontend = resources.pack_by_name("frontend.rpk")) {
      int left_root, right_root, left_can, right_can;
      gameref_upsert(&left_root, GameRefKind::Dummy)->empty = false;
      gameref_upsert(&right_root, GameRefKind::Dummy)->empty = false;
      const auto can = RpkIndex::make_full_id(frontend->pack_id, 0x21);
      gameref_upsert(&left_can, GameRefKind::RenderRef)->type_id = can;
      gameref_upsert(&right_can, GameRefKind::RenderRef)->type_id = can;
      gameref_set_parent(&left_can, &left_root);
      gameref_set_parent(&right_can, &right_root);
      gameref_get(&left_can)->color = 0xf5f5f5;
      gameref_get(&right_can)->color = 0xdc191a;
      const auto left = scene_instances_collect_root(&left_root);
      const auto right = scene_instances_collect_root(&right_root);
      check(left.size() == 1 && right.size() == 1,
            "inventory roots isolate each paint can");
      if (left.size() == 1 && right.size() == 1) {
        check(left[0].geometry == right[0].geometry,
              "paint cans share geometry across viewports");
        check(left[0].color == 0xf5f5f5 && right[0].color == 0xdc191a,
              "shared paint mesh retains independent instance colors");
      }
      gameref_remove(&left_can);
      check(scene_instances_collect_root(&left_root).empty() &&
                scene_instances_collect_root(&right_root).size() == 1,
            "destroying a preview does not remove another panel's mesh");
      gameref_remove(&right_can);
      gameref_remove(&left_root);
      gameref_remove(&right_root);
    } else
      check(false, "frontend paint-can pack resolves");
    scene_instances_set_resources(nullptr);
  }
  gameref_clear();
  return failures ? 1 : 0;
}
