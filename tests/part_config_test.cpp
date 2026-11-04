#include "slrr/part_config.hpp"
#include "slrr/rpk_index.hpp"
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace slrr;
int main(int argc, char **argv) {
  int failures = 0;
  auto check = [&](bool ok, const char *label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok)
      ++failures;
  };
  auto config = parse_part_config(
      "# independent slot ID and native wheel frame\n"
      "render 0x10\ndeformable 0x11\ntexture 0x25\n"
      "wheel -0.7 -0.59 -1.48 0 0 0 1 0 1 0\n"
      "slot 0.01 0 0 0 0 0 121\nslottype 101\nattach 0x1234 1\n"
      "steering 0 0.2 0 0 0 0\nslot 0 0 0 0 0 0 99\nslottype 11\n"
      "stockpart 0x44 ; preserve order and repeats\nstockpart 0x44\n"
      "body 0 0 0 0 0 0 10.5 sphere 1\ncategory 2\nfuture_directive "
      "123\neof\n");
  check(config.valid && config.slots.size() == 2, "CFG slots and poses parse");
  check(config.render == 0x11 && config.renders[0].resource == 0x10,
        "deformable shell precedes steering render");
  check(config.stock == std::vector<uint32_t>({0x44, 0x44}),
        "repeated stock wheel references remain ordered");
  check(config.texture == 0x25 && config.category == 2 &&
            std::fabs(config.mass - 10.5f) < .001f,
        "paint category and mass are retained");
  check(config.unknown.size() == 1, "unsupported commands remain visible");
  PartMatrix matrix;
  check(part_slot_matrix(config, config.slots[0], matrix) &&
            std::fabs(matrix[12] + .69f) < .0001f &&
            std::fabs(matrix[14] + 1.48f) < .0001f,
        "slottype uses wheel frame independently of slot ID");
  check(part_slot_matrix(config, config.slots[1], matrix) &&
            std::fabs(matrix[13] - .2f) < .0001f,
        "steering slot uses its native frame");
  PartPose pose;
  pose.position = {1, 2, 3};
  pose.rotation = {.9f, -.2f, .4f};
  auto original = part_pose_matrix(pose);
  auto identity =
      part_matrix_multiply(original, part_matrix_inverse_rigid(original));
  bool exact = true;
  for (int i = 0; i < 16; i++)
    exact = exact &&
            std::fabs(identity[i] -
                      ((i == 0 || i == 5 || i == 10 || i == 15) ? 1.f : 0.f)) <
                .0001f;
  check(exact, "rigid slot inverse preserves attachment transform");
  check(!parse_part_config("attach 0x1 2\n").errors.empty(),
        "orphan attachment fails explicitly");
  check(!parse_part_config("slot nan 0 0 0 0 0 1\n").errors.empty(),
        "nonfinite poses fail explicitly");
  if (argc > 1) {
    RpkIndex resources;
    resources.set_root(argv[1]);
    resources.open_all();
    const auto *pack = resources.pack_by_name("cars/racers/Baiern.rpk");
    check(pack != nullptr, "original Baiern RPK resolves");
    if (pack) {
      auto car =
          load_part_config(resources, RpkIndex::make_full_id(pack->pack_id, 6));
      check(car.valid && car.native_kind == "car" && car.wheels.size() == 4 &&
                car.slots.size() > 20,
            "original car CFG resolves wheel and body slots");
      for (const auto &error : car.errors)
        std::printf("CFG ERROR %s\n", error.c_str());
      auto hood = load_part_config(
          resources, RpkIndex::make_full_id(pack->pack_id, 0x10d));
      check(hood.valid && hood.slots.size() == 1 && hood.slots[0].id == 601,
            "original hood mount parses");
      if (hood.valid && !hood.slots[0].attach.empty())
        check(hood.slots[0].attach[0].resource == car.resource,
              "hood attachment retains its owning RPK identity");
      check(resources
                .model_for_resource(RpkIndex::pack_from_full(car.render),
                                    car.render & 0xffff)
                .ok,
            "car deformable shell resolves to actual SCX model");
    }
  }
  return failures ? 1 : 0;
}
