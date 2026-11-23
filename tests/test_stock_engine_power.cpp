// #352: every stock MWM racer keeps its pre-#215 dyno maxHP and peak rpm.
// Reads the installed game (engine classes must be stock); classes and saves
// are copied into Fixture::root. Values were measured on main 614eef1.
#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/race_runtime.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/part_runtime.hpp"
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

namespace slrr { void spawn_ground_shell(Jvm &, const RpkIndex &, Object *); }
using namespace slrr;
namespace {
float num(Object *o, const char *n) {
  if (!o) return -1;
  auto it = o->fields.find(n);
  if (it == o->fields.end()) return -2;
  return it->second.tag == ValueTag::Float ? it->second.v.f : it->second.tag == ValueTag::Int ? float(it->second.v.i) : -3;
}
struct Probe : Fixture {
  Object world, ground;
  Probe() {
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
    register_lang_natives(vm, packs, sections);
    register_gameref_natives(vm, packs, sections);
    register_resref_natives(vm, packs, sections);
    register_renderref_natives(vm, packs, sections);
    register_part_natives(vm, packs);
    register_race_natives(vm, packs, sections);
    gameref_upsert(&world, GameRefKind::Dummy)->flags = kWorldTreeRoot;
    ground.class_name = "java.util.resource.GroundRef";
    gameref_upsert(&ground, GameRefKind::GroundRef);
    gameref_set_parent(&ground, &world);
    scene_instances_set_resources(&packs);
    scene_instances_select(&world, nullptr, false);
  }
};
struct Expected {
  const char *pack;
  uint32_t local;
  const char *chassis;
  float hp;
  float rpm;
};
constexpr Expected kStock[] = {
    {"Baiern", 0x6, "Baiern_CoupeSport", 195.73f, 6250},
    {"Baiern", 0xfd, "Baiern_CoupeSport_GT_III", 457.70f, 7750},
    {"Baiern", 0x157, "Baiern_DevilSport", 311.64f, 6250},
    {"Duhen", 0x6, "Duhen_SunStrip_1_5_DVC", 142.42f, 7500},
    {"Duhen", 0x126, "Duhen_SunStrip_1_8_DVC", 162.76f, 6750},
    {"Duhen", 0x127, "Duhen_SunStrip_2_2_DVC", 202.05f, 6250},
    {"Duhen", 0x128, "Duhen_Racing_SunStrip_2_0_CDVC", 297.36f, 8000},
    {"Einvagen", 0x6, "Einvagen_110_GT", 127.73f, 5000},
    {"Einvagen", 0x112, "Einvagen_110_GTK", 168.67f, 5000},
    {"Einvagen", 0x113, "Einvagen_140_GTA", 234.53f, 5750},
    {"Enula", 0x6, "Ishima_Enula_WRY", 177.30f, 6750},
    {"Enula", 0x17c, "Ishima_Enula_WRZ", 220.23f, 6750},
    {"Enula", 0x17e, "Ishima_Enula_WR_SuperTurizmo", 365.88f, 7000},
    {"Focer", 0x6, "Shimutshibu_Focer_RC_200", 187.42f, 6500},
    {"Focer", 0x107, "Shimutshibu_Focer_RC_300", 252.28f, 7000},
    {"Focer", 0x108, "Shimutshibu_Focer_WRC", 377.46f, 6500},
    {"MC", 0x6, "MC_GT", 360.99f, 7000},
    {"MC", 0x109, "MC_GTB", 371.28f, 7250},
    {"MC", 0x10a, "MC_GTLE", 545.58f, 6250},
    {"Nonus", 0x6, "Emer_Nonus_Street_GT", 266.86f, 6500},
    {"Nonus", 0xdf, "Emer_MotorSport_Nonus_GT_II", 619.27f, 9000},
    {"Prime", 0x6, "Prime_DLH_500", 492.95f, 8500},
    {"SuperDuty", 0x6, "Hauler_s_SuperDuty_500", 419.98f, 5000},
    {"SuperDuty", 0x112, "Hauler_s_SuperDuty_Extra_750", 530.12f, 5000},
};
void stock_racers_keep_their_dyno_power() {
  Probe f;
  size_t checked = 0;
  for (const auto &want : kStock) {
    const auto lib = f.packs.open_lib((std::string("cars/racers/") + want.pack + ".rpk").c_str());
    auto *vehicle = vehicle_spawn_stock(f.vm, f.packs, &f.ground, RpkIndex::make_full_id(lib, want.local));
    auto *car = vehicle ? part_chassis(vehicle) : nullptr;
    SLRR_REQUIRE(car && car->class_name.ends_with(want.chassis));
    Object *dyno = nullptr;
    for (const auto &part : part_installed_tree(car)) {
      if (part.parent != car) continue;
      const auto it = part.object->fields.find("dynodata");
      if (it != part.object->fields.end() && it->second.tag == ValueTag::Obj && it->second.v.o)
        dyno = it->second.v.o;
    }
    SLRR_REQUIRE(dyno);
    const float hp = num(dyno, "maxHP"), rpm = num(dyno, "RPM_maxHP");
    std::printf("POWER %s %.2f hp at %.0f rpm (stock %.2f at %.0f)\n", want.chassis, hp, rpm, want.hp,
                want.rpm);
    SLRR_REQUIRE(std::fabs(hp - want.hp) <= want.hp * .005f && rpm == want.rpm);
    part_destroy(car);
    ++checked;
  }
  SLRR_REQUIRE(checked == std::size(kStock));
  scene_instances_select(nullptr, nullptr, false);
  scene_instances_set_resources(nullptr);
  gameref_clear();
  rpk_set_active(nullptr);
}
} // namespace
int main() {
  const standalone::Test tests[] = {stock_racers_keep_their_dyno_power};
  return standalone::run("stock_engine_power", tests);
}
