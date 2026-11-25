// #307: stock TUFA bars configure only the chassis mounts authored in CFG.
// Assets are read-only; copied bytecode and save files stay in Fixture::root.
#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/host_input.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/race_runtime.hpp"
#include "slrr/scene_instances.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace slrr;
namespace {
constexpr double kTick = 1. / 60.;
constexpr float kPad = 20000.f;
constexpr const char *kSwaybar = "java.game.parts.rgearpart.Swaybar";
struct Axle {
  double stiffness = 0, damping = 0;
};
constexpr Axle kStock{25000, 2500};
struct CarType {
  const char *pack, *script;
};
constexpr CarType kDuhen{"cars/racers/Duhen.rpk", "Duhen_SunStrip_1_5_DVC"};
constexpr CarType kEinvagen{"cars/racers/Einvagen.rpk", "Einvagen_110_GT"};
constexpr CarType kBaiern{"cars/racers/Baiern.rpk", "Baiern_CoupeSport"};
struct BarType {
  uint32_t local;
  const char *script;
};
constexpr std::array<BarType, 4> kBars{{
    {0x41ff, "swaybar_SunStrip_15_front"},
    {0x017c, "swaybar_SunStrip_15_rear"},
    {0x0219, "swaybar_Baiern_CS_front"},
    {0x021a, "swaybar_Baiern_CS_rear"},
}};

float number(Object *object, const char *name) {
  SLRR_REQUIRE(object && object->fields.contains(name));
  const auto value = object->fields.at(name);
  SLRR_REQUIRE(value.tag == ValueTag::Int || value.tag == ValueTag::Float);
  return value.tag == ValueTag::Float ? value.v.f : float(value.v.i);
}
InstalledPart installed(Object *part) {
  const auto tree = part_installed_tree(part);
  SLRR_REQUIRE(!tree.empty() && tree.front().object == part);
  return tree.front();
}
Object *on_slot(Object *car, int slot) {
  for (const auto &part : part_installed_tree(car))
    if (part.parent == car && part.parent_slot == slot) return part.object;
  return nullptr;
}
const PartSlot *slot(const PartConfig &config, int id) {
  const auto found = std::find_if(config.slots.begin(), config.slots.end(),
                                 [=](const PartSlot &s) { return s.id == id; });
  return found == config.slots.end() ? nullptr : &*found;
}

struct SwaybarFixture : Fixture {
  Object world, map, floor, controller, modal;
  struct Vehicle { Object *wrapper, *chassis; };
  std::vector<Vehicle> cars;
  std::vector<Object *> parts;

  // `patched` installs the shipped patch (classes and CFGs) over a private
  // hard-linked view of the game, as a player installing it would. Patched
  // files replace their link, so the installed game is never written.
  bool patched = false;
  fs::path overlay;
  static fs::path installed_game() {
    const auto game = fs::absolute(fs::is_directory("game") ? "game" : "data/game");
    SLRR_REQUIRE(fs::is_directory(game));
    return game;
  }
  fs::path patched_game() {
    const auto game = installed_game(), patch = fs::absolute("patch");
    SLRR_REQUIRE(fs::is_directory(patch));
    overlay = root.parent_path() / (root.filename().string() + "-game");
    for (const auto &entry : fs::recursive_directory_iterator(game))
      if (entry.is_regular_file()) {
        const auto out = overlay / fs::relative(entry.path(), game);
        fs::create_directories(out.parent_path());
        std::error_code error;
        fs::create_hard_link(entry.path(), out, error);
        if (error) fs::copy_file(entry.path(), out);
      }
    for (const auto &entry : fs::recursive_directory_iterator(patch))
      if (entry.is_regular_file() && entry.path().parent_path().filename() != "src") {
        const auto out = overlay / fs::relative(entry.path(), patch);
        fs::create_directories(out.parent_path());
        fs::remove(out); // unlink first: never write through to the game
        fs::copy_file(entry.path(), out);
      }
    return overlay;
  }
  explicit SwaybarFixture(bool with_patch = false) : patched(with_patch) {
    const auto game = patched ? patched_game() : installed_game();
    // Same stock bootstrap as test_nitrous: native file writes still use the
    // temporary VM root; RPK/CFG/SCX reads use the private installed game.
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
    register_physicsref_natives(vm);
    register_part_natives(vm, packs);
    register_race_natives(vm, packs, sections);
    auto *w = gameref_upsert(&world, GameRefKind::Dummy);
    w->empty = false;
    w->flags = kWorldTreeRoot;
    map.class_name = "java.lang.GameType";
    gameref_upsert(&map, GameRefKind::GameType)->empty = false;
    gameref_set_parent(&map, &world);
    controller.class_name = "java.io.Controller";
    gameref_upsert(&controller);
    scene_instances_set_resources(&packs);
    scene_instances_select(&world, nullptr, false);
    scene_instances_enable_static_floor(false);
    // Explicit support, as in test_vehicle_contacts_runtime: no implicit Y=0
    // floor, city traffic, controller, or fabricated drivetrain is required.
    physicsref_create_box(&floor, &map, 200, 2, 200, "swaybar regression pad");
    gameref_set_matrix(&floor, kPad, -1, kPad, 0, 0, 0);
    scene_instances_prepare_physics();
    SceneFloorHit hit;
    SLRR_REQUIRE(scene_instances_sample_floor(kPad, kPad, 4, nullptr, hit));
    SLRR_REQUIRE(std::fabs(hit.height) < .001f);
  }
  ~SwaybarFixture() {
    if (auto *car = part_controlled_chassis())
      part_command(&controller, "leave " + std::to_string(gameref_make_live(car)));
    host_input() = {};
    osd_clear_widgets();
    for (const auto &car : cars) part_destroy(car.chassis);
    for (auto *part : parts) part_destroy(part); // installed/destroyed ones are already gone
    scene_instances_select(nullptr, nullptr, false);
    scene_instances_set_resources(nullptr);
    scene_instances_enable_static_floor(false);
    gameref_clear();
    rpk_set_active(nullptr);
    packs.set_root(root.string());
    std::error_code error;
    if (!overlay.empty()) fs::remove_all(overlay, error); // links only
  }
  uint32_t resource(const char *pack, uint32_t local) {
    const auto id = RpkIndex::make_full_id(packs.open_lib(pack), local);
    SLRR_REQUIRE(packs.resolve(id));
    return id;
  }
  Object *spawn(CarType type) {
    const auto id = resource(type.pack, 6);
    const auto cfg = load_part_config(packs, id);
    SLRR_REQUIRE(cfg.valid && cfg.native_kind == "car" && cfg.wheels.size() == 4);
    auto *vehicle = vehicle_spawn_stock(vm, packs, &map, id);
    SLRR_REQUIRE(vehicle);
    auto *car = part_chassis(vehicle);
    SLRR_REQUIRE(car && car->class_name.ends_with(type.script));
    SLRR_REQUIRE(installed(car).resource == id && part_installed_tree(car).size() > 20);
    cars.push_back({vehicle, car});
    SLRR_REQUIRE(part_command(car, "damage_multiplier 0"));
    gameref_set_matrix(car, kPad + float(cars.size() - 1) * 20, 0, kPad, 0, 0, 0);
    part_park_chassis(car);
    // Stock scripts keep their bar lists commented out: catalog bars must NOT
    // appear automatically. The patched car scripts restore those lists.
    if (patched) return car;
    for (const auto &part : part_installed_tree(car))
      SLRR_REQUIRE(!vm.tree_instance_of(part.object, kSwaybar));
    expect(car);
    return car;
  }
  Object *bar(size_t index = 0) {
    const auto type = kBars.at(index);
    const auto id = resource("parts.rpk", type.local);
    auto *part = part_create(vm, packs, &map, id, "stock regression swaybar");
    SLRR_REQUIRE(part);
    parts.push_back(part);
    SLRR_REQUIRE(part->class_name.ends_with(type.script));
    SLRR_REQUIRE(vm.tree_instance_of(part, kSwaybar));
    SLRR_REQUIRE(number(part, "force") == kStock.stiffness);
    SLRR_REQUIRE(number(part, "damping") == kStock.damping);
    const auto cfg = load_part_config(packs, id);
    const auto *mate = slot(cfg, 1);
    SLRR_REQUIRE(cfg.valid && mate && mate->compatible.size() == 1);
    SLRR_REQUIRE(mate->compatible[0].resource == resource("parts.rpk", 0x4000));
    SLRR_REQUIRE(mate->compatible[0].slot == 1);
    return part;
  }
  PartPhysicsState state(Object *car) {
    PartPhysicsState out;
    SLRR_REQUIRE(part_physics_state(car, out));
    for (double value : out.anti_roll_stiffness) SLRR_REQUIRE(std::isfinite(value));
    for (double value : out.anti_roll_damping) SLRR_REQUIRE(std::isfinite(value));
    SLRR_REQUIRE(std::isfinite(out.height) && std::isfinite(out.rpm));
    return out;
  }
  void expect(Object *car, Axle front = {}, Axle rear = {}) {
    const auto out = state(car);
    const std::array<Axle, 2> expected{front, rear};
    for (size_t axle = 0; axle < expected.size(); ++axle) {
      SLRR_REQUIRE(std::fabs(out.anti_roll_stiffness[axle] - expected[axle].stiffness) < 1e-7);
      SLRR_REQUIRE(std::fabs(out.anti_roll_damping[axle] - expected[axle].damping) < 1e-7);
    }
  }
  void refresh(Object *car) {
    vm.invoke_by_name(car->class_name, "forceUpdate", {Value::make_obj(car)}, false);
  }
  void fit(Object *part, Object *car, int target) {
    SLRR_REQUIRE(part_install(part, car, 1, target));
    const auto actual = installed(part);
    SLRR_REQUIRE(actual.parent == car && actual.parent_slot == target && actual.own_slot == 1);
    SLRR_REQUIRE(on_slot(car, target) == part);
    const auto mate = vm.invoke_by_name(part->class_name, "slotIDOnSlot",
        {Value::make_obj(part), Value::make_int(1)}, false);
    SLRR_REQUIRE(mate.tag == ValueTag::Int && mate.v.i == target);
  }
  void remove(Object *part) {
    SLRR_REQUIRE(part_command(part, "remove 0 " + std::to_string(gameref_make_live(&map))));
    const auto actual = installed(part);
    SLRR_REQUIRE(!actual.parent && actual.parent_slot == 0 && actual.own_slot == 0);
    SLRR_REQUIRE(gameref_get(part) && gameref_get(part)->parent == &map);
  }
  void coefficients(Object *part, float force, float damping) {
    part->fields["force"] = Value::make_float(force);
    part->fields["damping"] = Value::make_float(damping);
  }
  void condition(Object *part, float wear, float tear) {
    registry.invoke("java.game.parts.Part", "setWear", "(F)F", part, {Value::make_float(wear)});
    registry.invoke("java.game.parts.Part", "setTear", "(F)F", part, {Value::make_float(tear)});
    SLRR_REQUIRE(installed(part).wear == wear && installed(part).tear == tear);
  }
  void coast(Object *car) {
    // Real stock engine remains unstarted. setState releases the whole body;
    // no engine fields, torque callback, ratio array or engine mock is added.
    const float velocity[] = {0, 0, -6}, angular[] = {0, 0, 0};
    part_apply_state(car, velocity, angular);
    SLRR_REQUIRE(state(car).active);
  }
  PartPhysicsState advance(Object *car) {
    const auto before = state(car);
    part_physics_advance(kTick);
    const auto after = state(car);
    SLRR_REQUIRE(after.active && after.ticks == before.ticks + 2);
    return after;
  }
  void control(Object *car, bool enabled) {
    SLRR_REQUIRE(part_command(&controller, std::string(enabled ? "controllable " : "leave ") +
                                          std::to_string(gameref_make_live(car))));
    SLRR_REQUIRE(part_controlled_chassis() == (enabled ? car : nullptr));
  }
};

void stock_mounts_and_generic_compatibility() {
  SwaybarFixture f;
  auto *duhen = f.spawn(kDuhen), *einvagen = f.spawn(kEinvagen), *baiern = f.spawn(kBaiern);
  for (auto *car : {duhen, einvagen, baiern}) {
    const auto cfg = load_part_config(f.packs, installed(car).resource);
    for (int id : {701, 702}) {
      const bool authored = car == duhen || (car == einvagen && id == 701);
      const auto *mount = slot(cfg, id);
      SLRR_REQUIRE(bool(mount) == authored);
      SLRR_REQUIRE(!on_slot(car, id));
      if (mount) {
        SLRR_REQUIRE(mount->attach.size() == 1);
        SLRR_REQUIRE(mount->attach[0].resource == f.resource("parts.rpk", 0x4000));
        SLRR_REQUIRE(mount->attach[0].slot == 1);
      }
    }
  }
  auto *spare = f.bar();
  for (size_t index = 0; index < kBars.size(); ++index) {
    auto *bar = f.bar(index);
    // The Baiern label does not create mounts on Baiern, nor does a Duhen
    // label or front/rear suffix restrict the real generic CFG identity.
    for (int target : {0, 701, 702}) SLRR_REQUIRE(!part_install(bar, baiern, 1, target));
    SLRR_REQUIRE(!part_install(bar, einvagen, 1, 702));
    f.expect(baiern);
    f.expect(einvagen);
    f.fit(bar, duhen, 701);
    f.expect(duhen, kStock);
    SLRR_REQUIRE(!part_install(spare, duhen, 1, 701)); // exact occupied slot, no fallback
    SLRR_REQUIRE(!installed(spare).parent && on_slot(duhen, 701) == bar);
    f.remove(bar);
    f.expect(duhen);
    f.fit(bar, duhen, 702);
    f.expect(duhen, {}, kStock);
    SLRR_REQUIRE(!part_install(spare, duhen, 1, 702));
    f.remove(bar);
    f.expect(duhen);
    f.fit(bar, einvagen, 701);
    f.expect(einvagen, kStock);
    f.remove(bar);
    f.expect(einvagen);
  }
}

// #413: the exe's Chassis.getMass returns the assembled vehicle body's mass
// (CarInfo kg/HP, Garage, catalog delivery price). Part.getMass stays the
// part's own CFG mass, so the chassis alone reads its _main_ body only.
void chassis_mass_is_the_assembled_car() {
  SwaybarFixture f;
  auto *car = f.spawn(kBaiern);
  auto mass = [&](const char *owner, Object *part) {
    const auto value = f.registry.invoke(owner, "getMass", "()F", part, {});
    SLRR_REQUIRE(value.tag == ValueTag::Float && std::isfinite(value.v.f));
    return double(value.v.f);
  };
  double sum = 0;
  for (const auto &part : part_installed_tree(car))
    sum += mass("java.game.parts.Part", part.object);
  const double body = mass("java.game.parts.Part", car);
  const double total = mass("java.game.parts.bodypart.Chassis", car);
  f.coast(car); // the released body carries the assembled physics mass
  const double physics = f.state(car).mass;
  std::printf("CHASSIS MASS body=%.1f total=%.1f sum=%.1f physics=%.1f\n", body,
              total, sum, physics);
  SLRR_REQUIRE(std::fabs(body - 838.5) < .5);
  SLRR_REQUIRE(std::fabs(total - sum) < .01 * sum);
  SLRR_REQUIRE(std::fabs(total - physics) < .01 * total);
  SLRR_REQUIRE(total > 1300 && total < 1700);
}

void refresh_remove_transfer_and_destroy_are_isolated() {
  SwaybarFixture f;
  auto *a = f.spawn(kDuhen), *b = f.spawn(kDuhen);
  auto *front = f.bar(), *rear = f.bar(1);
  f.fit(front, a, 701);
  f.fit(rear, a, 702);
  for (unsigned i = 0; i < 12; ++i) {
    f.refresh(a);
    f.expect(a, kStock, kStock); // no accumulating addrollbar contributions
    f.expect(b);
  }
  // Chassis script-update suspension must not suspend native configuration.
  a->fields["suspend_update"] = Value::make_int(1);
  f.coefficients(front, 12000, 800);
  f.refresh(a);
  f.expect(a, {12000, 800}, kStock);
  a->fields["suspend_update"] = Value::make_int(0);
  f.remove(front);
  f.expect(a, {}, kStock);
  f.fit(front, b, 702); // same object, new car AND axle
  f.expect(a, {}, kStock);
  f.expect(b, {}, {12000, 800});
  f.remove(front);
  f.expect(b);
  f.fit(front, a, 701);
  f.expect(a, {12000, 800}, kStock);
  f.coast(a);
  SLRR_REQUIRE(part_destroy(front));
  SLRR_REQUIRE(!on_slot(a, 701));
  f.advance(a); // destruction must clear before physics without forceUpdate
  f.expect(a, {}, kStock);
  f.expect(b);
  SLRR_REQUIRE(part_destroy(rear));
  f.advance(a);
  f.expect(a);
  f.refresh(a);
  f.expect(a);
}

void invalid_coefficients_disable_the_pair_without_poisoning_physics() {
  SwaybarFixture f;
  auto *car = f.spawn(kDuhen);
  auto *front = f.bar(), *rear = f.bar(1);
  f.fit(front, car, 701);
  f.fit(rear, car, 702);
  f.coast(car);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const std::array<std::array<float, 2>, 8> invalid{{
      {{-1, 2500}}, {{25000, -1}}, {{nan, 2500}}, {{25000, nan}},
      {{inf, 2500}}, {{25000, inf}}, {{-inf, 0}}, {{0, -inf}},
  }};
  for (const auto &pair : invalid) {
    f.coefficients(front, pair[0], pair[1]);
    f.refresh(car);
    f.expect(car, {}, kStock); // one bad field disables BOTH terms of this axle
    f.advance(car);
    f.expect(car, {}, kStock);
    f.coefficients(front, 12345, 678);
    f.advance(car); // a valid live edit recovers without a scripted refresh
    f.expect(car, {12345, 678}, kStock);
  }
  for (Axle valid : {Axle{0, 17}, Axle{800, 0}, Axle{0, 0}}) {
    f.coefficients(front, float(valid.stiffness), float(valid.damping));
    f.advance(car);
    f.expect(car, valid, kStock); // zero is valid, not an invalid/missing default
  }
}

void native_wear_tear_attenuate_and_stock_repair_restores() {
  SwaybarFixture f;
  auto *car = f.spawn(kDuhen);
  auto *front = f.bar(), *rear = f.bar(1);
  f.fit(front, car, 701);
  f.fit(rear, car, 702);
  // These lookalike script fields are not InstalledPart's native condition.
  front->fields["wear"] = front->fields["tear"] = Value::make_float(0);
  f.refresh(car);
  f.expect(car, kStock, kStock);
  const std::array<std::array<float, 2>, 7> conditions{{
      {{.5f, .25f}}, {{0, 1}}, {{1, 0}}, {{2, .5f}},
      {{-.5f, 1}}, {{.5f, 2}}, {{1, -.5f}},
  }};
  for (const auto &pair : conditions) {
    f.condition(front, pair[0], pair[1]);
    f.refresh(car);
    const double health = std::clamp(double(pair[0]), 0., 1.) *
                          std::clamp(double(pair[1]), 0., 1.);
    f.expect(car, {25000 * health, 2500 * health}, kStock);
  }
  f.condition(front, .5f, .25f);
  f.refresh(car);
  f.expect(car, {3125, 312.5}, kStock);
  f.vm.invoke_by_name(front->class_name, "repair", {Value::make_obj(front)}, false);
  const auto repaired = installed(front);
  // Stock repair stops at its authored repair_max values, not brand-new 1.0.
  SLRR_REQUIRE(repaired.wear == number(front, "repair_max_wear"));
  SLRR_REQUIRE(repaired.tear == number(front, "repair_max_tear"));
  SLRR_REQUIRE(repaired.wear > .5f && repaired.tear > .25f);
  f.refresh(car);
  const double health = double(repaired.wear) * repaired.tear;
  f.expect(car, {25000 * health, 2500 * health}, kStock);
  f.condition(front, 1, 1);
  f.refresh(car);
  f.expect(car, kStock, kStock);
}

void active_world_live_edits_do_not_need_controls_or_a_running_engine() {
  SwaybarFixture f;
  auto *car = f.spawn(kDuhen);
  auto *front = f.bar(), *rear = f.bar(1);
  f.fit(front, car, 701);
  f.fit(rear, car, 702);
  SLRR_REQUIRE(!part_controlled_chassis());
  f.coast(car);
  auto state = f.advance(car);
  SLRR_REQUIRE(state.rpm == 0 && state.speed_kph > 1);
  f.coefficients(front, 12345, 678);
  f.coefficients(rear, 9876, 543);
  for (unsigned i = 0; i < 6; ++i) {
    state = f.advance(car); // no forceUpdate, engine start, player or AI brain
    SLRR_REQUIRE(state.rpm == 0);
    f.expect(car, {12345, 678}, {9876, 543});
  }
  // Modal input blocks driving, not a passive installed suspension component.
  f.modal.fields["flags"] = Value::make_int(1);
  OsdWidget plate;
  plate.owner = &f.modal;
  osd_add_widget(plate);
  SLRR_REQUIRE(osd_has_modal());
  f.control(car, true);
  SLRR_REQUIRE(part_command(car, "stop"));
  f.coefficients(front, 15000, 900);
  f.advance(car);
  f.expect(car, {15000, 900}, {9876, 543});
  f.control(car, false);
  osd_clear_widgets();
  // Exercise configure_drive's actual no-engine early return as well, using
  // removal of the stock Block rather than inventing maxRPM/ratio/dyno state.
  Object *engine = nullptr;
  for (const auto &part : part_installed_tree(car))
    if (part.parent == car && f.vm.tree_instance_of(part.object, "java.game.parts.enginepart.Block"))
      engine = part.object;
  SLRR_REQUIRE(engine);
  f.remove(engine);
  f.parts.push_back(engine);
  f.control(car, true);
  SLRR_REQUIRE(part_command(car, "start"));
  f.coefficients(front, 18000, 1100);
  state = f.advance(car);
  SLRR_REQUIRE(state.rpm == 0);
  f.expect(car, {18000, 1100}, {9876, 543});
}

void stock_vehicle_save_load_reconstructs_mounts_and_native_condition() {
  SwaybarFixture f;
  auto *car = f.spawn(kDuhen);
  auto *front = f.bar(), *rear = f.bar(1);
  f.fit(front, car, 701);
  f.fit(rear, car, 702);
  f.condition(front, .5f, .25f);
  f.refresh(car);
  f.expect(car, {3125, 312.5}, kStock);
  const auto part_count = part_installed_tree(car).size();
  auto *name = f.vm.new_string("save/swaybar-vehicle");
  f.vm.invoke("java.game.Vehicle", "save", "(Ljava.lang.String;)V",
      {Value::make_obj(f.cars.front().wrapper), Value::make_obj(name)}, false);
  SLRR_REQUIRE(fs::is_regular_file(f.root / "save/swaybar-vehicle"));
  SLRR_REQUIRE(fs::file_size(f.root / "save/swaybar-vehicle") > 0);
  SLRR_REQUIRE(part_destroy(car));
  f.cars.clear();
  const auto loaded = f.vm.invoke("java.game.Vehicle", "load",
      "(Ljava.lang.String;Ljava.util.resource.GameRef;)Ljava.game.Vehicle;",
      {Value::make_obj(name), Value::make_obj(&f.map)}, true);
  SLRR_REQUIRE(loaded.tag == ValueTag::Obj && loaded.v.o);
  car = part_chassis(loaded.v.o);
  SLRR_REQUIRE(car && car->class_name.ends_with(kDuhen.script));
  f.cars.push_back({loaded.v.o, car});
  SLRR_REQUIRE(part_installed_tree(car).size() == part_count);
  auto *loaded_front = on_slot(car, 701), *loaded_rear = on_slot(car, 702);
  SLRR_REQUIRE(loaded_front && loaded_rear && loaded_front != front && loaded_rear != rear);
  SLRR_REQUIRE(f.vm.tree_instance_of(loaded_front, kSwaybar));
  SLRR_REQUIRE(f.vm.tree_instance_of(loaded_rear, kSwaybar));
  SLRR_REQUIRE(installed(loaded_front).resource == f.resource("parts.rpk", 0x41ff));
  SLRR_REQUIRE(installed(loaded_rear).resource == f.resource("parts.rpk", 0x017c));
  SLRR_REQUIRE(installed(loaded_front).wear == .5f && installed(loaded_front).tear == .25f);
  SLRR_REQUIRE(installed(loaded_rear).wear == 1 && installed(loaded_rear).tear == 1);
  // Coefficients are reconstructed from the live loaded parts, not serialized
  // native solver state. Stock Swaybar has no extra force/damping save payload.
  f.refresh(car);
  f.expect(car, {3125, 312.5}, kStock);
  f.refresh(car);
  f.expect(car, {3125, 312.5}, kStock);
  f.remove(loaded_front);
  f.parts.push_back(loaded_front);
  f.expect(car, {}, kStock);
}

// #311: the patch adds 701/702 mounts to every stock racer chassis that had
// none and uncomments each script's stock bar list, so a newly assembled car
// carries its own front and rear bars through the generic addPart.
void patched_stock_cars_install_their_listed_bars() {
  SwaybarFixture f(true);
  auto axle = [&](Object *bar) {
    SLRR_REQUIRE(bar && f.vm.tree_instance_of(bar, kSwaybar));
    const auto part = installed(bar);
    const double health = std::clamp<double>(part.wear, 0, 1) * std::clamp<double>(part.tear, 0, 1);
    return Axle{number(bar, "force") * health, number(bar, "damping") * health};
  };
  // #419: the patched bars carry per-car rates from the real cars' bar
  // diameters (N/m at the wheel) and no damping.
  const struct {
    CarType car;
    uint32_t front, rear;
    float front_force, rear_force;
  } expected[] = {
      {kDuhen, 0x41ff, 0x017c, 13200, 3400},     // authored mounts; rear entry corrected
      {kEinvagen, 0x0185, 0x0186, 20800, 13700}, // authored front, added rear mount
      {kBaiern, 0x0219, 0x021a, 43200, 8300},    // both mounts added
  };
  for (const auto &row : expected) {
    auto *car = f.spawn(row.car);
    auto *front = on_slot(car, 701), *rear = on_slot(car, 702);
    SLRR_REQUIRE(installed(front).resource == f.resource("parts.rpk", row.front));
    SLRR_REQUIRE(installed(rear).resource == f.resource("parts.rpk", row.rear));
    SLRR_REQUIRE(number(front, "force") == row.front_force && number(front, "damping") == 0);
    SLRR_REQUIRE(number(rear, "force") == row.rear_force && number(rear, "damping") == 0);
    f.expect(car, axle(front), axle(rear));
    size_t bars = 0;
    for (const auto &part : part_installed_tree(car))
      bars += f.vm.tree_instance_of(part.object, kSwaybar);
    SLRR_REQUIRE(bars == 2);
  }
}

void installed_damping_reduces_contacted_stock_chassis_roll() {
  SwaybarFixture f;
  auto *plain = f.spawn(kDuhen), *damped = f.spawn(kDuhen);
  struct RollSample {
    PartPhysicsState physics;
    double angle = 0, rate = 0;
  };
  auto sample = [&](Object *car) {
    RollSample out{f.state(car)};
    PartMatrix physical;
    SLRR_REQUIRE(scene_instances_script_matrix(car, physical.data()));
    for (float value : physical) SLRR_REQUIRE(std::isfinite(value));
    const auto *node = gameref_get(car);
    SLRR_REQUIRE(node && out.physics.active && out.physics.rpm == 0);
    // Same physical roll convention as dynamics_sync, not the interpolated
    // render matrix. The generic parent map has the identity world frame.
    out.angle = std::atan2(double(physical[1]), double(physical[5]));
    out.rate = node->angular_vel[2];
    SLRR_REQUIRE(std::isfinite(out.rate));
    SLRR_REQUIRE(std::fabs(out.angle - node->ypr[2]) < 1e-6);
    return out;
  };
  auto supported = [](const RollSample &s) {
    SLRR_REQUIRE(s.physics.contact[0] && s.physics.contact[1]);
    SLRR_REQUIRE(s.physics.force[0] > 0 && s.physics.force[1] > 0);
  };
  auto advance_pair = [&] {
    const auto before = f.state(damped).ticks;
    f.advance(plain); // one shared host tick, not one update per car
    SLRR_REQUIRE(f.state(damped).ticks == before + 2);
  };
  const float zero[] = {0, 0, 0};
  unsigned index = 0;
  for (auto *car : {plain, damped}) {
    // setState consumes a tyre-contact placement, not the already lifted
    // chassis origin left by spawn/park. Both start on the same explicit pad.
    gameref_set_matrix(car, kPad + 20 * float(index++), 0, kPad, 0, 0, 0);
    part_apply_state(car, zero, zero);
  }
  SLRR_REQUIRE(!part_controlled_chassis());
  // Fixed, bounded settling, with sustained loaded front contact and quiet
  // chassis motion required before imposing any roll excitation.
  for (unsigned tick = 0; tick < 240; ++tick) {
    advance_pair();
    if (tick >= 228)
      for (auto *car : {plain, damped}) {
        const auto s = sample(car);
        supported(s);
        SLRR_REQUIRE(std::fabs(s.physics.vertical_speed) < .02);
        SLRR_REQUIRE(std::fabs(s.rate) < .02);
      }
  }
  for (auto *car : {plain, damped}) {
    float x = 0, y = 0, z = 0;
    SLRR_REQUIRE(part_car_script_pos(car, &x, &y, &z));
    const auto *node = gameref_get(car);
    SLRR_REQUIRE(node);
    const std::array<float, 3> angles{node->ypr[0], node->ypr[1], node->ypr[2]};
    gameref_set_matrix(car, x, y, z, angles[0], angles[1], angles[2]);
    const float roll_rate[] = {0, 0, .25f};
    part_apply_state(car, zero, roll_rate);
  }
  // setState reseeds suspension contact. Establish and verify actual loaded
  // contact on BOTH no-bar cars before enabling the experimental damper.
  advance_pair();
  const auto before_plain = sample(plain), before_damped = sample(damped);
  supported(before_plain);
  supported(before_damped);
  SLRR_REQUIRE(before_plain.rate > .05 && before_damped.rate > .05);
  SLRR_REQUIRE(std::fabs(before_plain.angle - before_damped.angle) < 1e-6);
  SLRR_REQUIRE(std::fabs(before_plain.rate - before_damped.rate) < 1e-5);
  SLRR_REQUIRE(std::fabs(before_plain.physics.height - before_damped.physics.height) < 1e-5);
  SLRR_REQUIRE(before_plain.physics.mass == before_damped.physics.mass);
  auto *bar = f.bar();
  // Pure damping isolates the immediate velocity response; the kernel suite
  // separately verifies stiffness. This is still the real installed TUFA part.
  f.coefficients(bar, 0, 2500);
  f.fit(bar, damped, 701);
  f.expect(plain);
  f.expect(damped, {0, 2500});
  advance_pair();
  const auto after_plain = sample(plain), after_damped = sample(damped);
  supported(after_plain);
  supported(after_damped);
  const double roll_plain = after_plain.angle - before_plain.angle;
  const double roll_damped = after_damped.angle - before_damped.angle;
  std::printf("SWAY ROLL contacts=%d%d/%d%d rate=%.9f -> %.9f/%.9f "
              "physical droll=%.9f/%.9f\n",
      after_plain.physics.contact[0], after_plain.physics.contact[1],
      after_damped.physics.contact[0], after_damped.physics.contact[1],
      before_plain.rate, after_plain.rate, after_damped.rate, roll_plain, roll_damped);
  SLRR_REQUIRE(after_plain.physics.mass == after_damped.physics.mass);
  SLRR_REQUIRE(after_plain.rate > 0 && after_damped.rate > 0);
  SLRR_REQUIRE(after_damped.rate + 1e-4 < after_plain.rate);
  SLRR_REQUIRE(roll_plain > 1e-4 && roll_damped > 0);
  SLRR_REQUIRE(roll_damped + 1e-5 < roll_plain);
}
} // namespace

int main() {
  const standalone::Test tests[] = {
      stock_mounts_and_generic_compatibility,
      chassis_mass_is_the_assembled_car,
      refresh_remove_transfer_and_destroy_are_isolated,
      invalid_coefficients_disable_the_pair_without_poisoning_physics,
      native_wear_tear_attenuate_and_stock_repair_restores,
      active_world_live_edits_do_not_need_controls_or_a_running_engine,
      stock_vehicle_save_load_reconstructs_mounts_and_native_condition,
      installed_damping_reduces_contacted_stock_chassis_roll,
      patched_stock_cars_install_their_listed_bars,
  };
  return standalone::run("swaybars", tests);
}
