// #352: stock racer engines, with the shipped patch installed over the game
// (hard-linked view; patched files are copies, the game is never written).
// Pins each racer's maxHP and peak rpm, and checks that the 24 racers keep
// their pre-#215 maxHP on average (`legacy`, measured on main 614eef1).
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
  fs::path overlay;
  fs::path patched_game() {
    const auto game = fs::absolute(fs::is_directory("game") ? "game" : "data/game");
    const auto patch = fs::absolute("patch");
    SLRR_REQUIRE(fs::is_directory(game) && fs::is_directory(patch));
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
  ~Probe() {
    std::error_code error;
    if (!overlay.empty()) fs::remove_all(overlay, error); // links and copies only
  }
  Probe() {
    const auto game = patched_game();
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
  float legacy; // maxHP before #215
};
constexpr Expected kStock[] = {
    {"Baiern", 0x6, "Baiern_CoupeSport", 296.62f, 6250, 195.73f},
    {"Baiern", 0xfd, "Baiern_CoupeSport_GT_III", 519.92f, 7250, 457.70f},
    {"Baiern", 0x157, "Baiern_DevilSport", 413.24f, 6250, 311.64f},
    {"Duhen", 0x6, "Duhen_SunStrip_1_5_DVC", 163.96f, 7000, 142.42f},
    {"Duhen", 0x126, "Duhen_SunStrip_1_8_DVC", 177.82f, 6500, 162.76f},
    {"Duhen", 0x127, "Duhen_SunStrip_2_2_DVC", 188.35f, 5250, 202.05f},
    {"Duhen", 0x128, "Duhen_Racing_SunStrip_2_0_CDVC", 265.97f, 7750, 297.36f},
    {"Einvagen", 0x6, "Einvagen_110_GT", 148.27f, 4750, 127.73f},
    {"Einvagen", 0x112, "Einvagen_110_GTK", 201.31f, 5000, 168.67f},
    {"Einvagen", 0x113, "Einvagen_140_GTA", 225.26f, 5000, 234.53f},
    {"Enula", 0x6, "Ishima_Enula_WRY", 168.31f, 5500, 177.30f},
    {"Enula", 0x17c, "Ishima_Enula_WRZ", 223.46f, 6000, 220.23f},
    {"Enula", 0x17e, "Ishima_Enula_WR_SuperTurizmo", 366.08f, 6750, 365.88f},
    {"Focer", 0x6, "Shimutshibu_Focer_RC_200", 191.35f, 5750, 187.42f},
    {"Focer", 0x107, "Shimutshibu_Focer_RC_300", 275.47f, 6250, 252.28f},
    {"Focer", 0x108, "Shimutshibu_Focer_WRC", 408.68f, 5750, 377.46f},
    {"MC", 0x6, "MC_GT", 376.34f, 6250, 360.99f},
    {"MC", 0x109, "MC_GTB", 399.43f, 6250, 371.28f},
    {"MC", 0x10a, "MC_GTLE", 370.65f, 7250, 545.58f},
    {"Nonus", 0x6, "Emer_Nonus_Street_GT", 261.34f, 6000, 266.86f},
    {"Nonus", 0xdf, "Emer_MotorSport_Nonus_GT_II", 661.73f, 7750, 619.27f},
    {"Prime", 0x6, "Prime_DLH_500", 297.03f, 8000, 492.95f},
    {"SuperDuty", 0x6, "Hauler_s_SuperDuty_500", 188.92f, 5000, 419.98f},
    {"SuperDuty", 0x112, "Hauler_s_SuperDuty_Extra_750", 275.73f, 5000, 530.12f},
};
void stock_racers_keep_their_dyno_power() {
  Probe f;
  size_t checked = 0;
  double ratio = 0;
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
    std::printf("POWER %s %.2f hp at %.0f rpm (expected %.2f at %.0f, %.0f before #215)\n", want.chassis, hp, rpm, want.hp,
                want.rpm, want.legacy);
    SLRR_REQUIRE(std::fabs(hp - want.hp) <= want.hp * .005f && rpm == want.rpm);
    ratio += hp / want.legacy;
    part_destroy(car);
    ++checked;
  }
  SLRR_REQUIRE(checked == std::size(kStock));
  ratio /= double(checked);
  std::printf("POWER mean %.4f of the pre-#215 maxHP\n", ratio);
  SLRR_REQUIRE(ratio > .98 && ratio < 1.02);
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
