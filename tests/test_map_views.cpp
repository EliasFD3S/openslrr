#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/scene_instances.hpp"
using namespace slrr;

void map_camera_does_not_replace_scene() {
  Fixture f;
  gameref_clear();
  Object world, local, scene_camera, map_camera, scene_view, map_view;
  auto* root = gameref_upsert(&world, GameRefKind::Dummy);
  root->empty = false;
  root->flags = kWorldTreeRoot;
  auto* local_root = gameref_upsert(&local, GameRefKind::Dummy);
  local_root->empty = false;
  local_root->flags = kWorldTreeRoot;
  // Independent worlds can have an owning parent: they still remain isolated.
  gameref_set_parent(&local, &world);
  auto* scene = gameref_upsert(&scene_camera, GameRefKind::Camera);
  scene->empty = false;
  gameref_set_parent(&scene_camera, &world);
  auto* map = gameref_upsert(&map_camera, GameRefKind::Camera);
  map->empty = false;
  gameref_set_parent(&map_camera, &local);
  auto* scene_vp = gameref_upsert(&scene_view, GameRefKind::Viewport);
  auto* map_vp = gameref_upsert(&map_view, GameRefKind::Viewport);
  scene_vp->empty = map_vp->empty = false;
  scene_vp->vp_active = map_vp->vp_active = true;
  scene_vp->vp_pri = 10;
  map_vp->vp_pri = 12;
  map_vp->vp_w = .21f; map_vp->vp_h = .23f;
  scene_instances_select(&world, nullptr, false);
  gameref_activate_camera(*scene, *scene_vp, 1);
  gameref_activate_camera(*map, *map_vp, 1);
  SLRR_REQUIRE(fullscreen_script_camera() == scene);
  SLRR_REQUIRE(gameref_active_cameras().size() == 2);
  // Real Navigator.ViewMap layout: full-screen local world, resized scene view.
  map_vp->vp_w = map_vp->vp_h = 1.f; map_vp->vp_pri = 5;
  scene_vp->vp_x = scene_vp->vp_y = .64f;
  scene_vp->vp_w = scene_vp->vp_h = .35f;
  gameref_activate_camera(*map, *map_vp, 1);
  SLRR_REQUIRE(fullscreen_script_camera() == scene);
  float transform[16];
  SLRR_REQUIRE(scene_instance_transform(scene->key, &world, transform));
  SLRR_REQUIRE(!scene_instance_transform(map->key, &world, transform));
  SLRR_REQUIRE(scene_instance_transform(map->key, &local, transform));
  // Closing/reopening and viewport lifetime must not steal or retain a camera.
  map_vp->vp_active = false;
  scene_vp->vp_w = scene_vp->vp_h = 1.f;
  SLRR_REQUIRE(fullscreen_script_camera() == scene);
  SLRR_REQUIRE(gameref_active_cameras().size() == 1);
  scene_vp->vp_active = false;
  SLRR_REQUIRE(fullscreen_script_camera() == nullptr);
  scene_instances_select(nullptr, nullptr, false);
  gameref_clear();
  std::puts("PASS local GPS/fullscreen map stays separate from resized scene camera; close and viewport lifetime");
}
int main() {
  const standalone::Test tests[] = {map_camera_does_not_replace_scene};
  return standalone::run("map_views", tests);
}
