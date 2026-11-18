#pragma once
#include "save_io_fixture.hpp"
#include "slrr/gameref.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/race_runtime.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/script_compiler.hpp"
struct RaceFixture : Fixture {
  slrr::Object world, ground, controller;
  slrr::Object *player = nullptr;
  uint32_t car_type = 0;
  RaceFixture() {
    using namespace slrr;
    gameref_clear();
    register_gameref_natives(vm, packs, sections);
    register_part_natives(vm, packs);
    std::ofstream(root / "car.cfg")
        << "body 0 0 0 0 0 0 1200 box 1 1 2\nrender 0x0007\n";
    std::string payload =
        "native car " + (root / "car.cfg").generic_string() + "\r\n";
    std::vector<uint8_t> bytes(55, 0);
    auto put = [&](size_t p, uint32_t v) {
      for (int i = 0; i < 4; ++i)
        bytes[p + i] = uint8_t(v >> (8 * i));
    };
    bytes[0] = 'R';
    bytes[1] = 'P';
    bytes[2] = 'A';
    bytes[3] = 'K';
    put(4, 0x200);
    put(16, 23);
    put(20, 1);
    put(36, 6);
    bytes[40] = 8;
    put(46, 55);
    put(50, uint32_t(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    {
      std::ofstream out(root / "cars.rpk", std::ios::binary);
      out.write((char *)bytes.data(), bytes.size());
    }
    packs.set_root(fs::absolute("game").string());
    auto pack = packs.open_lib((root / "cars.rpk").string());
    car_type = RpkIndex::make_full_id(pack, 6);
    auto city = packs.open_lib("maps/city.rpk");
    auto *w = gameref_upsert(&world, GameRefKind::Dummy);
    w->flags = kWorldTreeRoot;
    auto *g = gameref_upsert(&ground, GameRefKind::GroundRef);
    g->type_id = RpkIndex::make_full_id(city, 1);
    gameref_set_parent(&ground, &world);
    // The synthetic car's resource payload is cached before changing root.
    scene_instances_set_resources(&packs);
    scene_instances_select(&world, nullptr, false);
    register_race_natives(vm, packs, sections);
  }
  slrr::Object *car() {
    using namespace slrr;
    auto *result = part_create(vm, packs, &ground, car_type, "fixture car");
    SLRR_REQUIRE(result);
    return result;
  }
  ~RaceFixture() {
    slrr::scene_instances_select(nullptr, nullptr, false);
    slrr::scene_instances_set_resources(nullptr);
    slrr::gameref_clear();
  }
  slrr::Value call(const char *name, const char *sig,
                   std::vector<slrr::Value> args = {}) {
    return registry.invoke("java.util.resource.GroundRef", name, sig, &ground,
                           args);
  }
};
