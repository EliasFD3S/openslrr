#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/race_runtime.hpp"
#include "slrr/scene_instances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

using namespace slrr;

namespace {
constexpr float kPad = 20000.f;
constexpr float kPi = 3.14159265358979323846f;
// The public runtime accumulator accepts 60 Hz host updates, each containing
// two synchronized 120 Hz vehicle substeps. Publication stays host-boundary.
constexpr double kStep = 1. / 60.;
constexpr const char *kBaiern = "cars/racers/Baiern.rpk";
constexpr const char *kEnula = "cars/racers/Enula.rpk";
constexpr const char *kWagon = "cars/traffic/Wagon.rpk";
constexpr const char *kTeg = "cars/fake_racers/Teg.rpk";

struct Sample {
  PartPhysicsState physics;
  PartMatrix matrix{};
  std::array<float, 3> position{}, velocity{}, angular{};
};

Sample sample(Object *car, bool require_active = true) {
  Sample out;
  const auto *node = gameref_get(car);
  SLRR_REQUIRE(node && part_physics_state(car, out.physics));
  SLRR_REQUIRE((!require_active || out.physics.active) && out.physics.mass > 100);
  SLRR_REQUIRE(scene_instances_script_matrix(car, out.matrix.data()));
  std::copy(out.matrix.begin() + 12, out.matrix.begin() + 15,
            out.position.begin());
  std::copy(std::begin(node->vel), std::end(node->vel), out.velocity.begin());
  std::copy(std::begin(node->angular_vel), std::end(node->angular_vel),
            out.angular.begin());
  for (float value : out.matrix) SLRR_REQUIRE(std::isfinite(value));
  for (float value : out.velocity) SLRR_REQUIRE(std::isfinite(value));
  for (float value : out.angular) SLRR_REQUIRE(std::isfinite(value));
  SLRR_REQUIRE(std::isfinite(out.physics.speed_kph) &&
               std::isfinite(out.physics.height) &&
               std::isfinite(out.physics.vertical_speed) &&
               std::isfinite(out.physics.yaw_rate));
  return out;
}

struct ContactFixture : Fixture {
  Object world, map, floor, other_world, other_map, controller;
  std::vector<Object *> cars;
  uint32_t city = 0;

  explicit ContactFixture(bool generic_map = false) {
    const auto game = fs::absolute("game");
    SLRR_REQUIRE(fs::is_directory(game));
    // Match test_race_vehicle's original TUFA bootstrap/factory. Only classes
    // are copied: all VM file natives still write below Fixture::root, never
    // into the supplied installation. RPK/CFG/SCX reads use the actual game.
    for (const auto &entry : fs::recursive_directory_iterator(game))
      if (entry.is_regular_file() && entry.path().extension() == ".class") {
        const auto out = root / fs::relative(entry.path(), game);
        fs::create_directories(out.parent_path());
        fs::copy_file(entry.path(), out);
      }
    packs.set_root(game.string());
    packs.open_all();
    rpk_set_active(&packs);
    SLRR_REQUIRE(vm.bootstrap_classpath());
    gameref_clear();
    register_lang_natives(vm, packs, sections);
    register_gameref_natives(vm, packs, sections);
    register_resref_natives(vm, packs, sections);
    register_renderref_natives(vm, packs, sections);
    register_physicsref_natives(vm);
    register_part_natives(vm, packs);
    register_race_natives(vm, packs, sections);

    auto *w = gameref_upsert(&world, GameRefKind::Dummy);
    w->empty = false;
    w->flags = kWorldTreeRoot;
    map.class_name = generic_map ? "java.lang.GameType"
                                 : "java.util.resource.GroundRef";
    auto *m = gameref_upsert(&map, generic_map ? GameRefKind::GameType
                                            : GameRefKind::GroundRef);
    m->empty = false;
    city = RpkIndex::make_full_id(packs.open_lib("maps/city.rpk"), 1);
    m->type_id = generic_map ? 0 : city;
    gameref_set_parent(&map, &world);
    auto *other = gameref_upsert(&other_world, GameRefKind::Dummy);
    other->empty = false;
    other->flags = kWorldTreeRoot;
    other = gameref_upsert(&other_map, GameRefKind::GameType);
    other->empty = false;
    gameref_set_parent(&other_map, &other_world);
    controller.class_name = "java.io.Controller";
    gameref_upsert(&controller);
    scene_instances_set_resources(&packs);
    scene_instances_select(&world, nullptr, false);
    scene_instances_enable_static_floor(false);
    // A real map's streets, slopes and props are deliberately not the test
    // floor. This explicit pad is far outside its structures, with top Y=0.
    physicsref_create_box(&floor, &map, 800, 2, 800, "contact regression pad");
    gameref_set_matrix(&floor, kPad, -1, kPad, 0, 0, 0);
    scene_instances_prepare_physics();
    SceneFloorHit hit;
    SLRR_REQUIRE(scene_instances_sample_floor(kPad, kPad, 4, nullptr, hit));
    SLRR_REQUIRE(std::fabs(hit.height) < .001f);
    damage(false);
  }

  ~ContactFixture() {
    if (auto *controlled = part_controlled_chassis())
      part_command(&controller, "leave " +
                    std::to_string(gameref_make_live(controlled)));
    for (auto *car : cars) part_destroy(car);
    scene_instances_select(nullptr, nullptr, false);
    scene_instances_set_resources(nullptr);
    scene_instances_enable_static_floor(false);
    gameref_clear();
    rpk_set_active(nullptr);
  }

  void damage(bool enabled) {
    vm.set_static("java.util.Config", "deformation",
                  Value::make_float(enabled ? 1.6f : 0.f));
    vm.set_static("java.util.Config", "internal_damage",
                  Value::make_float(enabled ? 1.f : 0.f));
    vm.set_static("java.util.Config", "player_damage_multiplier",
                  Value::make_float(enabled ? .8f : 0.f));
    host_game_options_load(vm);
    for (auto *car : cars)
      SLRR_REQUIRE(part_command(car, enabled ? "damage_multiplier 1"
                                            : "damage_multiplier 0"));
  }

  uint32_t car_type(const char *name) {
    const auto pack = packs.open_lib(name);
    const auto *data = packs.pack_by_id(pack);
    SLRR_REQUIRE(data);
    for (const auto &entry : data->entries) {
      const auto type = RpkIndex::make_full_id(pack, entry.type_id);
      const auto cfg = load_part_config(packs, type);
      if (cfg.valid && cfg.native_kind == "car" && cfg.wheels.size() == 4)
        return type;
    }
    throw std::runtime_error(std::string("missing real four-wheel car: ") + name);
  }

  Object *car(const char *name, bool mechanical = false) {
    const auto type = car_type(name);
    Object *result = nullptr;
    if (mechanical) {
      auto *vehicle = vehicle_spawn_stock(vm, packs, &map, type);
      SLRR_REQUIRE(vehicle);
      result = part_chassis(vehicle);
    } else {
      result = part_create(vm, packs, &map, type, "native contact car", {}, true);
    }
    SLRR_REQUIRE(result);
    cars.push_back(result);
    SLRR_REQUIRE(part_command(result, "damage_multiplier 0"));
    const auto tree = part_installed_tree(result);
    const auto cfg = load_part_config(packs, type);
    if (mechanical) {
      const auto ratio = result->fields.find("ratio");
      SLRR_REQUIRE(ratio != result->fields.end() &&
                   ratio->second.tag == ValueTag::Obj && ratio->second.v.o &&
                   ratio->second.v.o->slots.size() >= 8);
      SLRR_REQUIRE(tree.size() > 20);
    } else {
      const auto ratio = result->fields.find("ratio");
      SLRR_REQUIRE(ratio == result->fields.end() ||
                   (ratio->second.tag == ValueTag::Obj && !ratio->second.v.o));
      if (std::string(name) == kBaiern) {
        // Native Baiern has no Java drivetrain. Its stock wheels are mounted
        // CFG parts, unlike Wagon/Teg's inline wheel render declarations.
        SLRR_REQUIRE(std::none_of(cfg.renders.begin(), cfg.renders.end(),
                                 [](const PartRender &r) { return r.wheel >= 0; }));
        size_t mounted = 0;
        for (const auto &part : tree)
          if (part.parent == result && part.parent_slot >= 101 &&
              part.parent_slot <= 104) ++mounted;
        SLRR_REQUIRE(mounted == 4 && tree.size() > 4);
      } else {
        SLRR_REQUIRE(std::any_of(cfg.renders.begin(), cfg.renders.end(),
                                [](const PartRender &r) { return r.wheel >= 0; }));
      }
    }
    return result;
  }

  void place(Object *car, float x, float z, float yaw = -kPi / 2,
             float height = 0) {
    gameref_set_matrix(car, kPad + x, height, kPad + z, yaw, 0, 0);
    const float zero[] = {0, 0, 0};
    // setState itself consumes the tyre-contact placement and parks once.
    part_apply_state(car, zero, zero);
  }

  void velocity(Object *car, float x, float z = 0) {
    float px = 0, py = 0, pz = 0;
    SLRR_REQUIRE(part_car_script_pos(car, &px, &py, &pz));
    const auto *node = gameref_get(car);
    SLRR_REQUIRE(node);
    const std::array<float, 3> angles{node->ypr[0], node->ypr[1], node->ypr[2]};
    // A published chassis origin is not the script's tyre-contact getPos.
    // Restore that input before setState, otherwise every speed change lifts
    // the car by another ride height and stops this being a floor-contact test.
    gameref_set_matrix(car, px, py, pz, angles[0], angles[1], angles[2]);
    const float linear[] = {x, 0, z}, angular[] = {0, 0, 0};
    part_apply_state(car, linear, angular);
  }

  void park(Object *car) {
    float x = 0, y = 0, z = 0;
    SLRR_REQUIRE(part_car_script_pos(car, &x, &y, &z));
    const auto *node = gameref_get(car);
    SLRR_REQUIRE(node);
    const std::array<float, 3> angles{node->ypr[0], node->ypr[1], node->ypr[2]};
    gameref_set_matrix(car, x, y, z, angles[0], angles[1], angles[2]);
    part_park_chassis(car);
  }

  void advance(unsigned host_updates = 1) {
    for (unsigned i = 0; i < host_updates; ++i) part_physics_advance(kStep);
  }

  void control(Object *car) {
    SLRR_REQUIRE(part_command(&controller, "controllable " +
                              std::to_string(gameref_make_live(car))));
    SLRR_REQUIRE(part_controlled_chassis() == car);
  }

  Object *brain(Object *car) {
    auto *result = vm.new_object("java.lang.GameType");
    SLRR_REQUIRE(result);
    gameref_upsert(result);
    SLRR_REQUIRE(race_brain_command(packs, result, "controllable " +
                                    std::to_string(gameref_make_live(car))));
    // No route/throttle is needed to exercise a moving rigid body. The brain
    // owns its normal zero-input drive; setState supplies reproducible speed.
    return result;
  }

  void destroy(Object *car) {
    SLRR_REQUIRE(part_destroy(car));
    std::erase(cars, car);
  }
};

struct ImpactPair {
  Sample a, b;
  double impulse_a = 0, impulse_b = 0;
};

ImpactPair first_x_impact(ContactFixture &f, Object *a, Object *b,
                          unsigned limit = 120, bool passive_target = false) {
  auto before_a = sample(a), before_b = sample(b, !passive_target);
  for (unsigned tick = 0; tick < limit; ++tick) {
    f.advance();
    auto after_a = sample(a), after_b = sample(b, !passive_target);
    // Released cars share both 120 Hz substeps of each host update. A truly
    // parked body does not integrate until contact wakes it (possibly midway
    // through this update), but participates with finite mass throughout.
    SLRR_REQUIRE(after_a.physics.ticks == before_a.physics.ticks + 2);
    if (before_b.physics.active)
      SLRR_REQUIRE(after_b.physics.ticks == before_b.physics.ticks + 2);
    else
      SLRR_REQUIRE(after_b.physics.ticks >= before_b.physics.ticks &&
                   after_b.physics.ticks <= before_b.physics.ticks + 2);
    const double da = after_a.velocity[0] - before_a.velocity[0];
    const double db = after_b.velocity[0] - before_b.velocity[0];
    if (da < -.5 && db > .5) {
      const double ja = after_a.physics.mass * da;
      const double jb = after_b.physics.mass * db;
      // Tyres/floor are external impulses. At the first large contact step
      // they are small compared with the equal/opposite two-body impulse.
      SLRR_REQUIRE(std::fabs(ja + jb) < .25 * (std::fabs(ja) + std::fabs(jb)));
      std::printf("PAIR tick=%u masses=%.1f/%.1f dV=%.3f/%.3f J=%.1f/%.1f\n",
                  tick + 1, after_a.physics.mass, after_b.physics.mass, da, db,
                  ja, jb);
      return {after_a, after_b, ja, jb};
    }
    before_a = after_a;
    before_b = after_b;
  }
  throw std::runtime_error("both real chassis did not exchange contact impulses");
}

void native_baiern_wagon_head_on_in_generic_map() {
  ContactFixture f(true);
  auto *a = f.car(kBaiern), *b = f.car(kWagon);
  f.place(a, -7, 0);
  f.place(b, 7, 0, kPi / 2);
  f.advance(48);
  f.velocity(a, 22);
  f.velocity(b, -22);
  SLRR_REQUIRE(part_controlled_chassis() == nullptr);
  const auto hit = first_x_impact(f, a, b);
  SLRR_REQUIRE(hit.a.velocity[0] < 20 && hit.b.velocity[0] > -20);
  for (unsigned i = 0; i < 18; ++i) {
    f.advance();
    SLRR_REQUIRE(sample(a).position[0] < sample(b).position[0] + .25f);
  }
  std::puts("PASS native mounted-wheel Baiern and inline Wagon exchange head-on "
            "impulses without a player in a generic GameType map");
}

void native_mechanical_high_speed_head_on_does_not_tunnel() {
  ContactFixture f;
  auto *a = f.car(kBaiern), *b = f.car(kEnula, true);
  f.place(a, -8, 0);
  f.place(b, 8, 0, kPi / 2);
  f.advance(48);
  f.brain(a);
  f.control(b);
  f.velocity(a, 90);
  f.velocity(b, -90);
  first_x_impact(f, a, b, 30);
  SLRR_REQUIRE(part_controlled_chassis() == b);
  for (unsigned i = 0; i < 12; ++i) {
    f.advance();
    SLRR_REQUIRE(sample(a).position[0] < sample(b).position[0] + .25f);
  }
  SLRR_REQUIRE(part_controlled_chassis() == b);
  std::puts("PASS native AI/full mechanical player high-relative-speed head-on "
            "exchanges impulses without tunnelling; player retains control");
}

void mechanical_baiern_rear_end_and_offset_side_wake_parked_cars() {
  struct RestoreFramerate {
    FramerateMode previous = host_framerate_mode();
    ~RestoreFramerate() { host_set_framerate_mode(previous); }
  } restore; // Declared first so the fixture cleans up before mode restoration.
  ContactFixture f;
  host_set_framerate_mode(FramerateMode::Adaptive);
  const auto render_near_physics = [](Object *car, const Sample &physics) {
    PartMatrix drawn;
    SLRR_REQUIRE(scene_instances_render_matrix(car, drawn.data()));
    for (float value : drawn) SLRR_REQUIRE(std::isfinite(value));
    SLRR_REQUIRE(std::hypot(double(drawn[12] - physics.position[0]),
                            double(drawn[13] - physics.position[1]),
                            double(drawn[14] - physics.position[2])) < 2);
  };
  auto *rear = f.car(kBaiern, true), *parked = f.car(kWagon);
  auto *side = f.car(kTeg), *cross = f.car(kEnula, true);
  f.place(rear, -8, -20);
  f.place(parked, 0, -20);
  f.place(side, -7, 21.3f);
  f.place(cross, 0, 20, 0);
  f.advance(48);
  f.park(parked);
  f.park(cross);
  SLRR_REQUIRE(!sample(parked, false).physics.active);
  SLRR_REQUIRE(!sample(cross, false).physics.active);
  f.velocity(rear, 20);
  const auto rear_hit = first_x_impact(f, rear, parked, 120, true);
  SLRR_REQUIRE(rear_hit.b.physics.active && rear_hit.b.velocity[0] > .5f);
  // A passive car woken in the first substep must seed interpolation from its
  // actual parked pose, not the zero-initialized world origin 28 km away.
  render_near_physics(parked, rear_hit.b);
  f.velocity(side, 20);
  const auto side_hit = first_x_impact(f, side, cross, 120, true);
  SLRR_REQUIRE(side_hit.b.physics.active && side_hit.b.velocity[0] > .5f);
  render_near_physics(cross, side_hit.b);
  std::printf("SIDE yawrate=%.6f/%.6f published-angular-y=%.6f/%.6f "
              "z=%.3f/%.3f\n", side_hit.a.physics.yaw_rate,
              side_hit.b.physics.yaw_rate, side_hit.a.angular[1],
              side_hit.b.angular[1], side_hit.a.position[2] - kPad,
              side_hit.b.position[2] - kPad);
  // Both probes are rad/s. The map has identity axes, so the published
  // GameRef angular velocity must agree with the solver's yaw-rate sample.
  SLRR_REQUIRE(std::fabs(side_hit.a.angular[1]) +
                   std::fabs(side_hit.b.angular[1]) > .02f);
  SLRR_REQUIRE(std::fabs(side_hit.a.physics.yaw_rate - side_hit.a.angular[1]) < .0001);
  SLRR_REQUIRE(std::fabs(side_hit.b.physics.yaw_rate - side_hit.b.angular[1]) < .0001);
  f.advance(24);
  SLRR_REQUIRE(sample(parked).position[0] > kPad + .05f);
  SLRR_REQUIRE(sample(cross).position[0] > kPad + .05f);
  std::puts("PASS mechanical Baiern rear-end and inline Teg offset-side "
            "contacts wake and move passive unowned parked assemblies");
}

void parked_setmatrix_and_dirty_mass_do_not_restore_a_stale_body() {
  ContactFixture f(true);
  auto *car = f.car(kBaiern);
  f.place(car, 0, 0);
  f.advance(48);
  const auto previous = sample(car);
  gameref_set_matrix(car, kPad + 70, 0, kPad + 60, 0, 0, 0);
  part_park_chassis(car);
  const auto placed = sample(car, false);
  SLRR_REQUIRE(!placed.physics.active);
  SLRR_REQUIRE(placed.position[0] > previous.position[0] + 60);
  for (unsigned i = 0; i < 2; ++i) {
    f.advance();
    const auto idle = sample(car, false);
    SLRR_REQUIRE(!idle.physics.active);
    for (unsigned axis = 0; axis < 3; ++axis)
      SLRR_REQUIRE(std::fabs(idle.position[axis] - placed.position[axis]) < .005f);
  }
  // A passive participant must also rebuild its mass after an assembly edit,
  // without waking merely because a body-bearing stock panel was removed.
  Object *panel = nullptr;
  float panel_mass = 0;
  uint32_t panel_resource = 0;
  for (const auto &part : part_installed_tree(car)) {
    const auto cfg = load_part_config(f.packs, part.resource);
    if (part.parent == car && cfg.mass > 1) {
      panel = part.object;
      panel_mass = cfg.mass;
      panel_resource = part.resource;
      break;
    }
  }
  SLRR_REQUIRE(panel);
  const auto mass_before = sample(car, false).physics.mass;
  Object inventory;
  auto *storage = gameref_upsert(&inventory, GameRefKind::GameType);
  storage->empty = false;
  gameref_set_parent(&inventory, &f.other_map);
  // Use the actual removal command, which dirties the chassis, rather than
  // the lower-level destruction/cleanup helper that does not request refresh.
  SLRR_REQUIRE(part_command(panel, "remove 0 " +
                            std::to_string(gameref_make_live(&inventory))));
  f.advance(2);
  const auto edited = sample(car, false);
  std::printf("PARKED panel=%x cfgmass=%.3f mass=%.3f->%.3f active=%d "
              "pose=(%.3f,%.3f,%.3f)\n", panel_resource, panel_mass,
              mass_before, edited.physics.mass, edited.physics.active,
              edited.position[0] - kPad, edited.position[1],
              edited.position[2] - kPad);
  SLRR_REQUIRE(!edited.physics.active && edited.physics.mass < mass_before - .1);
  for (unsigned axis = 0; axis < 3; ++axis)
    SLRR_REQUIRE(std::fabs(edited.position[axis] - placed.position[axis]) < .005f);
  f.destroy(panel);
  gameref_remove(&inventory);
  std::puts("PASS configured parked setMatrix keeps its new placement and "
            "dirty assembly mass refreshes without restoring an old solver pose");
}

void inactive_hidden_and_overpass_chassis_are_not_peers() {
  ContactFixture f(true);
  auto *moving = f.car(kWagon), *baseline = f.car(kWagon);
  auto *inactive = f.car(kTeg), *hidden = f.car(kTeg);
  auto *overpass = f.car(kTeg);
  Object hidden_group, deck;
  auto *group = gameref_upsert(&hidden_group, GameRefKind::GameType);
  group->empty = false;
  gameref_set_parent(&hidden_group, &f.map);
  f.place(moving, -8, 0);
  f.place(baseline, -8, 40);
  f.place(inactive, 0, 0);
  f.place(hidden, 0, 0);
  f.place(overpass, 0, 0, 0, 8);
  physicsref_create_box(&deck, &f.map, 30, 1, 20, "overpass");
  gameref_set_matrix(&deck, kPad, 7.5f, kPad, 0, 0, 0);
  // Exclude the two coincident ghost cars before any shared substep, so their
  // deliberate overlap cannot first separate them into a different lane.
  gameref_set_parent(inactive, &f.other_map);
  gameref_set_parent(hidden, &hidden_group);
  gameref_get(&hidden_group)->streamed_hidden = true;
  f.advance(48);
  const auto inactive_pose = gameref_get(inactive)->pos[0];
  const auto hidden_pose = gameref_get(hidden)->pos[0];
  PartPhysicsState inactive_before, hidden_before;
  SLRR_REQUIRE(part_physics_state(inactive, inactive_before));
  SLRR_REQUIRE(part_physics_state(hidden, hidden_before));
  f.velocity(moving, 22);
  f.velocity(baseline, 22);
  for (unsigned i = 0; i < 90; ++i) {
    f.advance();
    const auto a = sample(moving), b = sample(baseline), top = sample(overpass);
    SLRR_REQUIRE(std::fabs(a.position[0] - b.position[0]) < .03f);
    SLRR_REQUIRE(std::fabs(a.velocity[0] - b.velocity[0]) < .03f);
    SLRR_REQUIRE(top.position[1] > 7.5f);
    SLRR_REQUIRE(std::fabs(top.velocity[0]) < .05f);
  }
  PartPhysicsState inactive_after, hidden_after;
  SLRR_REQUIRE(part_physics_state(inactive, inactive_after));
  SLRR_REQUIRE(part_physics_state(hidden, hidden_after));
  SLRR_REQUIRE(inactive_after.ticks == inactive_before.ticks);
  SLRR_REQUIRE(hidden_after.ticks == hidden_before.ticks);
  SLRR_REQUIRE(gameref_get(inactive)->pos[0] == inactive_pose);
  SLRR_REQUIRE(gameref_get(hidden)->pos[0] == hidden_pose);
  SLRR_REQUIRE(sample(moving).position[0] > kPad + 3);
  gameref_remove(&deck);
  gameref_remove(&hidden_group);
  std::puts("PASS other-world and hidden-ancestor cars do not tick/contact; "
            "an overpass car does not block the identical unobstructed trajectory");
}

void ai_hits_free_physicsref_without_nearby_player() {
  for (bool player_far : {false, true}) {
    ContactFixture f;
    auto *ai = f.car(player_far ? kEnula : kBaiern, player_far);
    auto *brain = f.brain(ai);
    (void)brain;
    f.place(ai, -8, 0);
    if (player_far) {
      auto *player = f.car(kWagon);
      f.place(player, 150, 100);
      f.control(player);
    } else SLRR_REQUIRE(part_controlled_chassis() == nullptr);
    Object free;
    physicsref_create_box(&free, &f.map, 1.6f, 1.6f, 1.6f, "free contact target");
    gameref_set_matrix(&free, kPad, .81f, kPad, 0, 0, 0);
    f.registry.invoke("java.util.resource.PhysicsRef", "setStatic", "(I)V",
                      &free, {Value::make_int(0)});
    f.advance(48);
    SLRR_REQUIRE(gameref_get(&free)->physics_simulated);
    const float start = gameref_get(&free)->pos[0];
    f.velocity(ai, 22);
    bool exchanged = false;
    float largest_car_drop = 0, largest_free_speed = 0;
    auto before = sample(ai);
    for (unsigned i = 0; i < 90; ++i) {
      f.advance();
      auto after = sample(ai);
      const auto *body = gameref_get(&free);
      SLRR_REQUIRE(body && body->physics_simulated);
      for (float value : body->pos) SLRR_REQUIRE(std::isfinite(value));
      for (float value : body->vel) SLRR_REQUIRE(std::isfinite(value));
      largest_car_drop = std::max(largest_car_drop,
                                   before.velocity[0] - after.velocity[0]);
      largest_free_speed = std::max(largest_free_speed, body->vel[0]);
      if (after.velocity[0] < before.velocity[0] - .3f && body->vel[0] > .3f)
        exchanged = true;
      before = after;
    }
    const auto *final_free = gameref_get(&free);
    std::printf("FREE player-far=%d car-drop=%.4f free-max-vx=%.4f "
                "free-dx=%.4f free-y=%.4f car-x=%.4f car-vx=%.4f exchanged=%d\n",
                player_far, largest_car_drop, largest_free_speed,
                final_free->pos[0] - start, final_free->pos[1],
                before.position[0] - kPad, before.velocity[0], exchanged);
    SLRR_REQUIRE(exchanged && final_free->pos[0] > start + .15f);
    gameref_remove(&free);
  }
  std::puts("PASS native/mechanical brain-owned cars exchange impulses with "
            "free PhysicsRef bodies with player absent or far away");
}

// Read the same embedded native object CFG used by map placement loading.
// Select a real, entirely positive-mass, compact prop, not a synthetic car or
// an immovable wall disguised as a vehicle.
uint32_t compact_city_prop(ContactFixture &f, const GroundMapData &map,
                           std::shared_ptr<const ScxGeometry> &geometry) {
  std::set<uint32_t> visited;
  for (const auto &placement : map.placements) {
    if (!visited.insert(placement.type).second) continue;
    const auto bytes = f.packs.read_payload(placement.type);
    if (bytes.size() < 10 || std::memcmp(bytes.data(), "ICFG", 4)) continue;
    const auto separator = std::find(bytes.begin() + 8, bytes.end(), 0);
    if (separator == bytes.end()) continue;
    const std::string kind(bytes.begin() + 8, separator);
    if (kind != "object") continue;
    const std::string text(separator + 1, bytes.end());
    const auto cfg = parse_part_config(text, &f.packs,
                                      RpkIndex::pack_from_full(placement.type));
    bool positive = false, all_positive = true;
    for (const auto &body : cfg.bodies) {
      if (!body.collision) continue;
      positive = positive || body.mass > 0;
      all_positive = all_positive && body.mass > 0;
    }
    float mass = 0, center[3]{};
    if (!positive || !all_positive ||
        !scene_instances_native_body(placement.type, geometry, mass, center) ||
        !geometry || mass < 30 || mass > 3000) continue;
    const auto &lo = geometry->info.bbox_min, &hi = geometry->info.bbox_max;
    if (hi[0] - lo[0] > .3f && hi[0] - lo[0] < 2.5f &&
        hi[1] - lo[1] > .5f && hi[1] - lo[1] < 2.5f &&
        hi[2] - lo[2] > .3f && hi[2] - lo[2] < 2.5f) {
      std::printf("PROP real type=%x mass=%.1f bounds=%.2f/%.2f/%.2f\n",
                  placement.type, mass, hi[0] - lo[0], hi[1] - lo[1],
                  hi[2] - lo[2]);
      return placement.type;
    }
  }
  return 0;
}

void ai_wakes_and_hits_promoted_map_prop_without_nearby_player() {
  for (bool player_far : {false, true}) {
    ContactFixture f;
    auto source = scene_instances_ground_map(f.packs, f.city);
    std::shared_ptr<const ScxGeometry> geometry;
    uint32_t prop = compact_city_prop(f, source, geometry);
    for (auto exterior : source.exterior_maps) {
      if (prop) break;
      auto next = scene_instances_ground_map(f.packs, exterior);
      prop = compact_city_prop(f, next, geometry);
    }
    SLRR_REQUIRE(prop && geometry && source.render_id);
    GroundMapData isolated;
    isolated.render_id = source.render_id;
    GroundMapData::Placement placement;
    placement.type = prop;
    placement.seed = 0;
    const auto &lo = geometry->info.bbox_min, &hi = geometry->info.bbox_max;
    placement.position = {kPad - (lo[0] + hi[0]) * .5f, .01f - lo[1],
                          kPad - (lo[2] + hi[2]) * .5f};
    isolated.placements.push_back(placement);
    // Keep the real map shell identity/frame but re-place only one real prop
    // on our flat pad. This traverses dormant wake -> promotion -> loose body.
    scene_instances_add_ground_props(isolated);
    Object shell;
    auto *s = gameref_upsert(&shell, GameRefKind::RenderRef);
    s->empty = false;
    gameref_set_type(*s, static_cast<int32_t>(source.render_id));
    gameref_set_parent(&shell, &f.map);
    f.map.fields["_ground_shell"] = Value::make_obj(&shell);
    auto *ai = f.car(player_far ? kWagon : kBaiern);
    f.brain(ai);
    f.place(ai, -8, 0);
    if (player_far) {
      auto *player = f.car(kTeg);
      f.place(player, 150, 100);
      f.control(player);
    } else SLRR_REQUIRE(part_controlled_chassis() == nullptr);
    f.advance(48);
    f.velocity(ai, 22);
    Object *dynamic = nullptr;
    PartMatrix initial{};
    bool moved = false, reacted = false;
    auto before = sample(ai);
    for (unsigned i = 0; i < 90; ++i) {
      f.advance();
      auto after = sample(ai);
      if (!dynamic)
        for (auto *node : gameref_render_nodes())
          if (node->dynamic_native && node->parent == &f.map) {
            dynamic = static_cast<Object *>(node->key);
            SLRR_REQUIRE(scene_instances_script_matrix(dynamic, initial.data()));
            break;
          }
      if (dynamic) {
        PartMatrix current;
        SLRR_REQUIRE(scene_instances_script_matrix(dynamic, current.data()));
        for (float value : current) SLRR_REQUIRE(std::isfinite(value));
        moved = moved || current[12] > initial[12] + .1f;
      }
      reacted = reacted || after.velocity[0] < before.velocity[0] - .1f;
      before = after;
    }
    SLRR_REQUIRE(dynamic && moved && reacted);
    gameref_remove(&shell);
  }
  std::puts("PASS AI proximity promotes a real city prop and both bodies react "
            "without selecting the absent/distant player as contact carrier");
}

void damage_on_pair_affects_both_and_detached_parts_keep_finite_motion() {
  ContactFixture f;
  auto *a = f.car(kBaiern, true), *b = f.car(kEnula, true);
  f.place(a, -8, 0);
  f.place(b, 8, 0, kPi / 2);
  f.advance(48);
  const auto original_a = part_installed_tree(a);
  const auto original_b = part_installed_tree(b);
  SLRR_REQUIRE(!original_a.empty() && !original_b.empty());
  const float condition_a = original_a.front().tear;
  const float condition_b = original_b.front().tear;
  std::vector<InstalledPart> original = original_a;
  original.insert(original.end(), original_b.begin(), original_b.end());
  f.damage(true);
  f.velocity(a, 45);
  f.velocity(b, -45);
  bool damaged_a = false, damaged_b = false, loose_moved = false;
  std::map<Object *, PartMatrix> previous_loose;
  for (unsigned i = 0; i < 90; ++i) {
    f.advance();
    sample(a);
    sample(b);
    const auto now_a = part_installed_tree(a);
    const auto now_b = part_installed_tree(b);
    SLRR_REQUIRE(!now_a.empty() && !now_b.empty());
    damaged_a = damaged_a || now_a.front().tear < condition_a - .001f;
    damaged_b = damaged_b || now_b.front().tear < condition_b - .001f;
    for (const auto &part : original) {
      if (part.object == a || part.object == b) continue;
      const auto tree = part_installed_tree(part.object);
      if (tree.empty() || tree.front().parent) continue;
      const auto *node = gameref_get(part.object);
      SLRR_REQUIRE(node && node->parent == &f.map);
      PartMatrix frame;
      SLRR_REQUIRE(scene_instances_script_matrix(part.object, frame.data()));
      for (float value : frame) SLRR_REQUIRE(std::isfinite(value));
      const auto [previous, first] = previous_loose.try_emplace(part.object, frame);
      if (!first) {
        const double distance = std::hypot(
            double(frame[12] - previous->second[12]),
            double(frame[13] - previous->second[13]),
            double(frame[14] - previous->second[14]));
        SLRR_REQUIRE(std::isfinite(distance) && distance < 20);
        loose_moved = loose_moved || distance > .005;
        previous->second = frame;
      }
    }
  }
  std::printf("DAMAGE both=%d/%d detached=%zu moving=%d\n", damaged_a,
              damaged_b, previous_loose.size(), loose_moved);
  SLRR_REQUIRE(damaged_a && damaged_b);
  SLRR_REQUIRE(!previous_loose.empty() && loose_moved);
  for (const auto &[part, frame] : previous_loose) f.destroy(part);
  std::puts("PASS damage-enabled real mechanical pair damages both chassis; "
            "detached original parts retain finite moving world frames");
}

void controller_handoff_deletion_and_zero_time_pause_keep_shared_ownership() {
  ContactFixture f(true);
  auto *a = f.car(kBaiern, true), *b = f.car(kTeg);
  auto *brain = f.brain(a);
  f.place(a, -8, 0);
  f.place(b, 0, 0);
  f.advance(48);
  f.control(a);
  SLRR_REQUIRE(race_brain_command(f.packs, brain, "leave"));
  race_brain_forget(brain);
  RaceDriveInput input;
  SLRR_REQUIRE(!race_drive_input(a, {kPad - 8, 0, kPad}, -kPi / 2,
                                 0, float(kStep), input));
  f.control(b);
  SLRR_REQUIRE(part_command(&f.controller, "leave " +
                            std::to_string(gameref_make_live(b))));
  SLRR_REQUIRE(part_controlled_chassis() == nullptr);
  f.velocity(a, 20);
  first_x_impact(f, a, b);

  const auto healthy_ticks = sample(b).physics.ticks;
  auto *faulted_node = gameref_get(a);
  SLRR_REQUIRE(faulted_node);
  faulted_node->local_matrix[0] = std::numeric_limits<float>::quiet_NaN();
  faulted_node->ypr[0] = std::numeric_limits<float>::quiet_NaN();
  faulted_node->matrix_override = true;
  f.advance();
  PartPhysicsState faulted;
  SLRR_REQUIRE(part_physics_state(a, faulted) && !faulted.active);
  for (float value : gameref_get(a)->local_matrix)
    SLRR_REQUIRE(std::isfinite(value));
  SLRR_REQUIRE(sample(b).physics.ticks == healthy_ticks + 2);
  // A valid setMatrix/setState retry must restore pair eligibility, not only
  // ordinary ticking: the old fault flag must not leave a permanent ghost.
  f.place(a, -8, 0);
  f.place(b, 0, 0);
  f.advance(24);
  f.velocity(a, 20);
  const auto retried = first_x_impact(f, a, b);
  SLRR_REQUIRE(retried.a.physics.active && retried.b.velocity[0] > .5f);
  std::puts("RETRY valid placement restores finite shared pair impulses");

  Object free;
  physicsref_create_box(&free, &f.map, 1, 1, 1, "pause witness");
  gameref_set_matrix(&free, kPad + 50, 3, kPad, 0, 0, 0);
  gameref_set_physics_simulated(&free, true);
  f.advance();
  const auto before_a = sample(a), before_b = sample(b);
  const auto free_before = *gameref_get(&free);
  for (unsigned i = 0; i < 60; ++i) part_physics_advance(0);
  const auto paused_a = sample(a), paused_b = sample(b);
  SLRR_REQUIRE(paused_a.physics.ticks == before_a.physics.ticks);
  SLRR_REQUIRE(paused_b.physics.ticks == before_b.physics.ticks);
  SLRR_REQUIRE(paused_a.matrix == before_a.matrix &&
               paused_b.matrix == before_b.matrix);
  SLRR_REQUIRE(paused_a.velocity == before_a.velocity &&
               paused_b.velocity == before_b.velocity);
  const auto *free_after = gameref_get(&free);
  SLRR_REQUIRE(std::equal(std::begin(free_before.pos), std::end(free_before.pos),
                          std::begin(free_after->pos)));
  SLRR_REQUIRE(std::equal(std::begin(free_before.vel), std::end(free_before.vel),
                          std::begin(free_after->vel)));
  f.control(a);
  const auto dead_id = gameref_make_live(a);
  f.destroy(a);
  SLRR_REQUIRE(!gameref_find_live(dead_id) && part_controlled_chassis() == nullptr);
  f.advance(12);
  SLRR_REQUIRE(sample(b).physics.ticks == paused_b.physics.ticks + 24);
  SLRR_REQUIRE(gameref_get(&free)->pos[1] < free_before.pos[1]);
  for (const auto &vehicle : part_vehicle_positions())
    SLRR_REQUIRE(vehicle.chassis != a);
  gameref_remove(&free);
  std::puts("PASS handoff/leave and valid fault retry retain driverless powered "
            "contacts; zero-time pause is inert and deletion removes the peer");
}
} // namespace

int main() {
  const standalone::Test tests[] = {
      native_baiern_wagon_head_on_in_generic_map,
      native_mechanical_high_speed_head_on_does_not_tunnel,
      mechanical_baiern_rear_end_and_offset_side_wake_parked_cars,
      parked_setmatrix_and_dirty_mass_do_not_restore_a_stale_body,
      inactive_hidden_and_overpass_chassis_are_not_peers,
      ai_hits_free_physicsref_without_nearby_player,
      ai_wakes_and_hits_promoted_map_prop_without_nearby_player,
      damage_on_pair_affects_both_and_detached_parts_keep_finite_motion,
      controller_handoff_deletion_and_zero_time_pause_keep_shared_ownership,
  };
  return standalone::run("vehicle_contacts_runtime", tests);
}
