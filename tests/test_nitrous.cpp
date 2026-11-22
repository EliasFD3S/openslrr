// #303/#304: real stock kit -> native fuel/drive -> authored exhaust flames.
// Installed assets are read-only; TUFA classes and saves use Fixture::root.
#include "save_io_fixture.hpp"
#include "slrr/dyno_model.hpp"
#include "slrr/gameref.hpp"
#include "slrr/host_input.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/race_runtime.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/vehicle_dynamics.hpp"
#include <algorithm>
#include <cmath>

namespace slrr {
void spawn_ground_shell(Jvm &, const RpkIndex &, Object *);
}
using namespace slrr;
namespace {
constexpr double kTick = 1. / 60., kRpm = 9.54929658551372;
float number(Object *object, const char *name) {
  SLRR_REQUIRE(object && object->fields.contains(name));
  const auto value = object->fields.at(name);
  SLRR_REQUIRE(value.tag == ValueTag::Int || value.tag == ValueTag::Float);
  return value.tag == ValueTag::Float ? value.v.f : float(value.v.i);
}
Object *object_field(Object *object, const char *name) {
  const auto it = object->fields.find(name);
  return it != object->fields.end() && it->second.tag == ValueTag::Obj ? it->second.v.o : nullptr;
}
std::vector<CarLight> flames(bool lit_only) {
  auto lights = part_car_lights(lit_only);
  std::erase_if(lights, [](const CarLight &light) { return light.kind != CarLightKind::Nitro; });
  return lights;
}
size_t check_authored_flame_meshes(Object *car) {
  size_t checked = 0;
  for (const auto &part : part_installed_tree(car)) {
    const auto *parent = gameref_get(part.object);
    if (!parent) continue;
    for (auto *child : parent->children) {
      const auto *node = gameref_get(child);
      if (!node || !(node->light_flags & 256u)) continue;
      const auto stock = scene_instances_geometry(uint32_t(node->type_id));
      SLRR_REQUIRE(stock && !stock->positions.empty());
      const auto drawn = node->deformed_geometry && node->deformed_type == node->type_id
                             ? node->deformed_geometry : stock;
      SLRR_REQUIRE(drawn->positions == stock->positions && drawn->normals == stock->normals);
      SLRR_REQUIRE(drawn->indices == stock->indices && drawn->uvs == stock->uvs);
      SLRR_REQUIRE(drawn->position_scale == stock->position_scale);
      ++checked;
    }
  }
  return checked;
}
struct NitroFixture : Fixture {
  Object world, ground, controller, player, hud, modal;
  Object *car = nullptr, *bottle = nullptr, *rail = nullptr;
  std::vector<Object *> loose;
  uint32_t baiern = 0;
  NitroFixture() {
    const auto game = fs::absolute("game");
    SLRR_REQUIRE(fs::is_directory(game));
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
    osd_clear_widgets();
    host_input() = {};
    register_lang_natives(vm, packs, sections);
    register_gameref_natives(vm, packs, sections);
    register_resref_natives(vm, packs, sections);
    register_renderref_natives(vm, packs, sections);
    register_part_natives(vm, packs);
    register_race_natives(vm, packs, sections);
    gameref_upsert(&world, GameRefKind::Dummy)->flags = kWorldTreeRoot;
    ground.class_name = "java.util.resource.GroundRef";
    gameref_upsert(&ground, GameRefKind::GroundRef)->type_id =
        RpkIndex::make_full_id(packs.open_lib("maps/city.rpk"), 1);
    gameref_set_parent(&ground, &world);
    controller.class_name = "java.io.Controller";
    gameref_upsert(&controller);
    scene_instances_set_resources(&packs);
    scene_instances_select(&world, nullptr, false);
    spawn_ground_shell(vm, packs, &ground);
    scene_instances_enable_static_floor(false);
    baiern = packs.open_lib("cars/racers/Baiern.rpk");
    auto *vehicle = vehicle_spawn_stock(vm, packs, &ground, RpkIndex::make_full_id(baiern, 6));
    SLRR_REQUIRE(vehicle && (car = part_chassis(vehicle)));
    auto roads = race_ground_roads(packs, &ground);
    SLRR_REQUIRE(roads && !roads->edges().empty());
    const auto &edge = roads->edges().front();
    SLRR_REQUIRE(edge.points.size() >= 2);
    const auto start = edge.points[0], next = edge.points[1];
    const float yaw = std::atan2(-(next[0] - start[0]), -(next[2] - start[2]));
    gameref_set_matrix(car, start[0], start[1], start[2], yaw, 0, 0);
    part_park_chassis(car);
    SLRR_REQUIRE(part_command(car, "damage_multiplier 0"));
    control(true);
    // Manual neutral and an open clutch isolate engine RPM from road contacts.
    SLRR_REQUIRE(part_command(car, "transmission 2"));
    host_input().drive_clutch = 1;
    SLRR_REQUIRE(part_command(car, "start"));
    player.fields["_hostVehicleHud"] = Value::make_int(1);
    player.fields["osd"] = Value::make_obj(&hud);
    vm.set_static("java.game.GameLogic", "player", Value::make_obj(&player));
    vm.set_static("java.game.GameLogic", "time", Value::make_float(12 * 3600.f));
  }
  ~NitroFixture() {
    host_input() = {};
    osd_clear_widgets();
    vm.set_static("java.game.GameLogic", "player", Value::make_obj(nullptr));
    if (car) {
      control(false);
      part_destroy(car);
    }
    for (auto *part : loose) part_destroy(part);
    scene_instances_select(nullptr, nullptr, false);
    scene_instances_set_resources(nullptr);
    scene_instances_enable_static_floor(false);
    gameref_clear();
    rpk_set_active(nullptr);
  }
  void control(bool enabled) {
    SLRR_REQUIRE(part_command(&controller, std::string(enabled ? "controllable " : "leave ") +
                                          std::to_string(gameref_make_live(car))));
    SLRR_REQUIRE(part_controlled_chassis() == (enabled ? car : nullptr));
  }
  PartPhysicsState state() {
    PartPhysicsState state;
    SLRR_REQUIRE(part_physics_state(car, state));
    SLRR_REQUIRE(std::isfinite(state.nitro_remaining) && std::isfinite(state.nitro_capacity));
    return state;
  }
  PartPhysicsState advance(unsigned ticks = 1) {
    for (unsigned i = 0; i < ticks; ++i) part_physics_advance(kTick);
    return state();
  }
  void refresh() {
    vm.invoke_by_name(car->class_name, "forceUpdate", {Value::make_obj(car)}, false);
  }
  void expect_hud(float ratio) {
    VehicleHudState hud_state;
    SLRR_REQUIRE(part_vehicle_hud(hud_state));
    SLRR_REQUIRE(std::fabs(hud_state.nitro - ratio) < 1e-6f);
  }
  void install_bottle() {
    SLRR_REQUIRE(!bottle);
    bottle = part_create(vm, packs, &ground,
        RpkIndex::make_full_id(packs.open_lib("parts.rpk"), 0x1c1), "12lb regression bottle");
    SLRR_REQUIRE(bottle && part_install(bottle, car));
    refresh();
    SLRR_REQUIRE(number(bottle, "capacity") > 5 && number(bottle, "capacity") < 6);
  }
  void install_rail() {
    SLRR_REQUIRE(!rail);
    rail = part_create(vm, packs, &ground,
        RpkIndex::make_full_id(packs.open_lib("parts/engines/Baiern_Emer.rpk"), 0x51),
        "stock regression NOS rail");
    SLRR_REQUIRE(rail && part_install(rail, car));
    refresh();
    SLRR_REQUIRE(number(car, "consumption_nitro") > 0);
  }
  Object *dyno() {
    for (const auto &part : part_installed_tree(car))
      if (part.parent == car)
        if (auto *dyno = object_field(part.object, "dynodata")) return dyno;
    throw std::runtime_error("stock Baiern engine has no dynodata");
  }
  void start_engine() {
    host_input().drive_nitro = false;
    host_input().drive_throttle = .2f;
    for (unsigned i = 0; i < 240; ++i) {
      const auto current = advance();
      if (current.rpm > 1600 && current.rpm < number(car, "RPM_limit") * .7) return;
    }
    throw std::runtime_error("stock Baiern did not start below limiter");
  }
};

void stock_nitrous_lifecycle() {
  NitroFixture f;
  // Pristine stock car: neither a rail nor a bottle. A held key cannot turn
  // the dry engine into a wet one, or manufacture fuel/flames/HUD capacity.
  SLRR_REQUIRE(number(f.car, "tank_nitro") == 0);
  SLRR_REQUIRE(number(f.car, "consumption_nitro") == 0);
  f.start_engine();
  host_input().drive_nitro = true;
  auto state = f.advance(4);
  SLRR_REQUIRE(state.nitro_capacity == 0 && state.nitro_remaining == 0 && !state.nitro_active);
  SLRR_REQUIRE(flames(true).empty());
  f.expect_hud(0);

  // Both real stock mufflers carry authored flag-256 SCX flame meshes.
  SLRR_REQUIRE(check_authored_flame_meshes(f.car) == 2);
  const auto outlets = flames(false);
  SLRR_REQUIRE(outlets.size() == 2);
  SLRR_REQUIRE(outlets[0].part == outlets[1].part && outlets[0].car == f.car);
  SLRR_REQUIRE(outlets[1].car == f.car && outlets[0].resource == outlets[1].resource);
  SLRR_REQUIRE(std::hypot(outlets[0].position[0] - outlets[1].position[0],
                          outlets[0].position[2] - outlets[1].position[2]) > .8f);
  const auto authored = load_light_resource(f.packs, outlets[0].resource);
  SLRR_REQUIRE(authored && authored->has_flare);
  SLRR_REQUIRE(!light_resource_lights_scene(authored->type));
  for (const auto &outlet : outlets) {
    SLRR_REQUIRE(outlet.light.type == authored->type && outlet.light.has_flare);
    SLRR_REQUIRE(outlet.light.diffuse == authored->diffuse && outlet.range == authored->range);
    SLRR_REQUIRE(!part_lamp_lit(outlet.part, 256u, true)); // never preview NOS
  }

  // Installing a real 12lb bottle changes capacity, not the native running
  // aggregate. Even reload cannot activate NOS without an injector rail.
  f.install_bottle();
  state = f.state();
  const double capacity = number(f.bottle, "capacity");
  SLRR_REQUIRE(state.nitro_capacity == capacity && state.nitro_remaining == 0);
  SLRR_REQUIRE(part_command(f.car, "reload"));
  state = f.advance(4);
  SLRR_REQUIRE(state.nitro_remaining == capacity && !state.nitro_active && !state.nitro_flame);
  SLRR_REQUIRE(flames(true).empty());
  f.expect_hud(0);

  // Canister.actual is original script save data, NOT the live native tank.
  // Exercise the stock virtual save/load chain in the isolated save root.
  host_input().drive_nitro = false;
  f.bottle->fields["actual"] = Value::make_float(2.25f);
  f.file.class_name = "java.io.File";
  SLRR_REQUIRE(f.open("save/nitrous-canister", 1) == 1);
  f.vm.invoke_by_name(f.bottle->class_name, "save",
                      {Value::make_obj(f.bottle), Value::make_obj(&f.file)}, false);
  f.close();
  SLRR_REQUIRE(fs::file_size(f.root / "save/nitrous-canister") > 0);
  f.bottle->fields["actual"] = Value::make_float(0);
  SLRR_REQUIRE(f.open("save/nitrous-canister", 0) == 1);
  SLRR_REQUIRE(uint32_t(f.read_resource()) ==
      RpkIndex::make_full_id(f.packs.open_lib("parts.rpk"), 0x1c1));
  f.read_int(); // Part.save header version; instance load starts at its body.
  f.vm.invoke_by_name(f.bottle->class_name, "load",
                      {Value::make_obj(f.bottle), Value::make_obj(&f.file)}, false);
  f.close();
  SLRR_REQUIRE(number(f.bottle, "actual") == 2.25f);
  SLRR_REQUIRE(f.state().nitro_remaining == capacity);

  f.install_rail();
  f.expect_hud(1);
  f.refresh();
  SLRR_REQUIRE(f.state().nitro_remaining == capacity);
  // Restart at a known moderate RPM before the exact two-substep witness.
  SLRR_REQUIRE(part_command(f.car, "reset"));
  SLRR_REQUIRE(f.state().nitro_remaining == capacity);
  SLRR_REQUIRE(part_command(f.car, "start"));
  f.start_engine();
  host_input().drive_nitro = .1f;
  const double below_threshold = f.state().nitro_remaining;
  state = f.advance();
  SLRR_REQUIRE(!state.nitro_active && state.nitro_remaining == below_threshold);
  const auto before = f.state();
  const double rate = number(f.car, "consumption_nitro");
  SLRR_REQUIRE(rate > 0 && rate < 3); // real stock injector, not a mock dyno
  SLRR_REQUIRE(before.gear == 0 && before.rpm < number(f.car, "RPM_limit") * .7);

  // Predict only the uncoupled engine's middle RPM using the public solver.
  // Fuel integration is independently checked as RPM*.0002*input*limiter*dt*rate
  // for EACH 120-Hz step, not final RPM*host dt, and not curve-call count.
  physics::VehicleDynamics predictor;
  predictor.body.configure(1000, {-1, -.5, -2}, {1, .5, 2}, {});
  predictor.engine_running = true;
  predictor.engine_omega = before.rpm / kRpm;
  auto &drive = predictor.drive;
  drive.engaged = true;
  drive.automatic_shift = drive.auto_clutch = false;
  drive.clutch = 0;
  drive.throttle = host_input().drive_throttle;
  drive.idle = std::max(400.f, number(f.car, "engine_rpm_idle"));
  drive.limit = std::max(drive.idle + 100., double(number(f.car, "RPM_limit")));
  drive.inertia = .28 + .01 * std::max(0.f, number(f.car, "engine_inertia"));
  drive.torque = [&](double rpm) { return dyno_sample(f.dyno(), float(rpm), 0, false); };
  drive.nitro_torque = [&](double rpm) { return dyno_sample(f.dyno(), float(rpm), 1, false); };
  drive.nitro = .25;
  const physics::Terrain air = [](double, double, double, physics::GroundHit &) { return false; };
  double expected = 0;
  for (int substep = 0; substep < 2; ++substep) {
    const double limiter = predictor.engine_limit_fraction();
    SLRR_REQUIRE(limiter == 1);
    expected += predictor.engine_omega * kRpm * .0002 * .25 * limiter * rate / 120.;
    SLRR_REQUIRE(predictor.advance(1. / 120., air) == 1);
  }
  host_input().drive_nitro = .25f;
  state = f.advance();
  const double consumed = before.nitro_remaining - state.nitro_remaining;
  std::printf("NITRO rate %.9f RPM %.3f -> %.3f consumed %.12f expected %.12f ticks %llu\n",
              rate, before.rpm, state.rpm, consumed, expected,
              static_cast<unsigned long long>(state.ticks - before.ticks));
  SLRR_REQUIRE(state.ticks == before.ticks + 2 && state.nitro_active);
  SLRR_REQUIRE(std::fabs(state.rpm - predictor.engine_omega * kRpm) < 1e-5);
  SLRR_REQUIRE(expected > 0 && std::fabs(consumed - expected) < 1e-9);
  SLRR_REQUIRE(number(f.bottle, "actual") == 2.25f);
  f.expect_hud(float(state.nitro_remaining / capacity));

  host_input().drive_nitro = 1.f;
  bool saw_flame = false, saw_dark = false;
  for (unsigned i = 0; i < 120 && !(saw_flame && saw_dark); ++i) {
    const double previous = state.nitro_remaining;
    state = f.advance();
    SLRR_REQUIRE(state.nitro_active && state.nitro_remaining <= previous);
    const auto lit = flames(true);
    SLRR_REQUIRE(lit.size() == (state.nitro_flame ? 2u : 0u));
    SLRR_REQUIRE(part_lamp_lit(outlets[0].part, 256u, false) == state.nitro_flame);
    saw_flame |= state.nitro_flame;
    saw_dark |= !state.nitro_flame;
    if (state.nitro_flame) {
      // Camera/shadow/reflection collections and pause must not re-roll the
      // flame gate or subtract another portion of the tank.
      SLRR_REQUIRE(check_authored_flame_meshes(f.car) == 2);
      const auto frozen = state;
      for (int view = 0; view < 8; ++view) {
        const auto meshes = scene_instances_collect_root(f.car);
        SLRR_REQUIRE(!meshes.empty() && flames(true).size() == 2);
        VehicleHudState hud;
        SLRR_REQUIRE(part_vehicle_hud(hud));
        part_physics_advance(0);
        const auto sampled = f.state();
        SLRR_REQUIRE(sampled.ticks == frozen.ticks && sampled.nitro_flame == frozen.nitro_flame);
        SLRR_REQUIRE(sampled.nitro_remaining == frozen.nitro_remaining);
      }
      Object other_world;
      gameref_upsert(&other_world, GameRefKind::Dummy)->flags = kWorldTreeRoot;
      scene_instances_select(&other_world, nullptr, false);
      SLRR_REQUIRE(flames(true).empty()); // old section cannot leak an optical glow before another tick
      SLRR_REQUIRE(!part_lamp_lit(outlets[0].part, 256u, false));
      scene_instances_select(&f.world, nullptr, false);
      SLRR_REQUIRE(flames(true).size() == 2);
      gameref_remove(&other_world);
    }
  }
  SLRR_REQUIRE(saw_flame && saw_dark);

  host_input().drive_nitro = false;
  const double released = state.nitro_remaining;
  state = f.advance(4);
  SLRR_REQUIRE(!state.nitro_active && !state.nitro_flame && state.nitro_remaining == released);
  SLRR_REQUIRE(flames(true).empty());
  f.refresh();
  SLRR_REQUIRE(f.state().nitro_remaining == released); // forceUpdate != reload
  for (const char *command : {"stop", "suspend"}) {
    SLRR_REQUIRE(part_command(f.car, command));
    SLRR_REQUIRE(!f.state().nitro_active && !f.state().nitro_flame);
    SLRR_REQUIRE(f.state().nitro_remaining == released);
    SLRR_REQUIRE(part_command(f.car, "start"));
  }

  // Modal input and another/nonexistent controller cannot spend this tank.
  f.modal.fields["flags"] = Value::make_int(1);
  OsdWidget plate;
  plate.owner = &f.modal;
  osd_add_widget(plate);
  SLRR_REQUIRE(osd_has_modal());
  host_input().drive_nitro = true;
  state = f.advance(4);
  SLRR_REQUIRE(!state.nitro_active && !state.nitro_flame && state.nitro_remaining == released);
  osd_clear_widgets();
  f.control(false);
  state = f.advance(4);
  SLRR_REQUIRE(!state.nitro_active && state.nitro_remaining == released);
  f.control(true);
  SLRR_REQUIRE(part_command(f.car, "transmission 2"));
  SLRR_REQUIRE(part_command(f.car, "reset"));
  SLRR_REQUIRE(f.state().nitro_remaining == released && !f.state().nitro_flame);
  SLRR_REQUIRE(part_command(f.car, "start"));
  f.start_engine();

  // Bound the depletion test without absurd engine forces or changing the
  // authored kit/dyno: only the native consumption scalar is accelerated.
  f.car->fields["consumption_nitro"] = Value::make_float(100);
  host_input().drive_nitro = true;
  for (unsigned i = 0; i < 600 && f.state().nitro_remaining > 0; ++i) f.advance();
  state = f.advance(2); // the next step must switch back to the dry bank
  SLRR_REQUIRE(state.nitro_remaining == 0 && !state.nitro_active && !state.nitro_flame);
  SLRR_REQUIRE(flames(true).empty());
  f.expect_hud(0);
  f.refresh(); // restores the stock rail's real rate, never fuel
  SLRR_REQUIRE(f.state().nitro_remaining == 0 && number(f.car, "consumption_nitro") == rate);
  SLRR_REQUIRE(part_command(f.car, "reset"));
  SLRR_REQUIRE(f.state().nitro_remaining == 0);
  SLRR_REQUIRE(part_command(f.car, "reload"));
  SLRR_REQUIRE(f.state().nitro_remaining == capacity);
  f.expect_hud(1);

  SLRR_REQUIRE(part_destroy(f.rail));
  f.rail = nullptr;
  f.refresh();
  SLRR_REQUIRE(number(f.car, "consumption_nitro") == 0);
  SLRR_REQUIRE(f.state().nitro_remaining == capacity);
  f.expect_hud(0);
  SLRR_REQUIRE(part_command(f.car, "start"));
  f.start_engine();
  host_input().drive_nitro = true;
  state = f.advance(4);
  SLRR_REQUIRE(!state.nitro_active && !state.nitro_flame && state.nitro_remaining == capacity);

  // Removing capacity clamps to zero. Reinstalling even a new bottle does
  // not refill; a rail without a bottle cannot activate either.
  SLRR_REQUIRE(part_destroy(f.bottle));
  f.bottle = nullptr;
  f.refresh();
  SLRR_REQUIRE(f.state().nitro_capacity == 0 && f.state().nitro_remaining == 0);
  f.install_rail();
  SLRR_REQUIRE(part_command(f.car, "reload"));
  state = f.advance(4);
  SLRR_REQUIRE(!state.nitro_active && state.nitro_remaining == 0);
  f.install_bottle();
  SLRR_REQUIRE(f.state().nitro_capacity == capacity && f.state().nitro_remaining == 0);
  SLRR_REQUIRE(part_command(f.car, "reload"));
  SLRR_REQUIRE(f.state().nitro_remaining == capacity);

  // Tear off the exhaust: its two independently placed flames disappear.
  SLRR_REQUIRE(part_tear_off(outlets[0].part));
  f.loose.push_back(outlets[0].part);
  SLRR_REQUIRE(flames(false).empty() && !part_lamp_lit(outlets[0].part, 256u, false));
  f.control(false);
  Object *destroyed = f.car;
  SLRR_REQUIRE(part_destroy(f.car));
  f.car = nullptr;
  SLRR_REQUIRE(!part_physics_state(destroyed, state));
  SLRR_REQUIRE(part_controlled_chassis() == nullptr && part_car_lights(false).empty());
  std::puts("PASS stock NOS install/input/rate/depletion/reload/HUD/save/teardown");
}

} // namespace
int main() {
  const standalone::Test tests[] = {stock_nitrous_lifecycle};
  return standalone::run("nitrous", tests);
}
