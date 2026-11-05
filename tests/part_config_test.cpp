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
            std::fabs(config.mass - 10.5f) < .001f &&
            config.bodies.size() == 1 &&
            std::fabs(config.bodies[0].pose.position[2]) < .001f,
        "paint category and mass are retained");
  auto sprung = parse_part_config("type 10 sphere 0.315\n"
                                  "spring 0.290 0.100 0.050\n"
                                  "wheel -0.67 -0.630 -1.146 0 0 0 1 0 1 0\n"
                                  "type 10 sphere 0.315\n"
                                  "spring 0.290 0.100 0.050\n"
                                  "wheel 0.67 -0.630 -1.146 0 0 0 1 0 1 0\n"
                                  "body 0 0 0.2 0 0 0 1292 box 1 1 1\n");
  const float force = 4.6f * 1000.f * 9.81f;
  const float load = 1292.f * 0.539f / 2.f;
  float length = 0.f;
  const float rest = 9.5f * 0.0254f + load * 9.81f / force;
  check(sprung.wheel_springs.size() == 2 && sprung.wheel_springs[0].authored &&
            std::fabs(sprung.wheel_springs[0].base - 0.29f) < 1e-6f &&
            std::fabs(sprung.wheel_springs[0].radius - 0.315f) < 1e-6f &&
            part_parked_spring_length(rest, 7.f * 0.0254f, 11.5f * 0.0254f,
                                      force, load, length) &&
            std::fabs(length - 0.2413f) < 1e-4f,
        "parked spring length matches the installed preload");
  check(std::fabs(
            (sprung.wheel_springs[0].base + sprung.wheel_springs[0].travel) -
            length - 0.1487f) < 1e-4f,
        "ride shift is the anchor minus the loaded length");
  check(std::fabs(sprung.wheel_springs[0].mass - 10.f) < 1e-6f &&
            std::fabs(sprung.wheel_springs[1].mass - 10.f) < 1e-6f,
        "CFG type mass is the unsprung wheel mass");
  check(config.unknown.size() == 1, "unsupported commands remain visible");
  auto steering_config =
      parse_part_config("maxsteer 0.65 2.548\nwheel -0.683 -0.635 -1.333 0 0 0 "
                        "1 1 1 0\nwheel 0.660 -0.655 1.215 0 0 0 0 0 1 1\n");
  check(std::fabs(steering_config.max_steer - .65f) < 1e-6 &&
            std::fabs(steering_config.ackermann_length - 2.548f) < 1e-6 &&
            steering_config.wheel_steering == std::vector<float>({1, 0}),
        "CFG steering limit, Ackermann length and per-wheel factors are "
        "retained");
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
  auto hood = parse_part_config(
      "slot 0 0.155 -0.797 0 0 0 601\n"
      "0 0.155 -0.797 1 0 0 0 0 -1 0 0.873\n");
  check(hood.valid && hood.slots.size() == 1 && hood.slots[0].has_flap &&
            hood.slots[0].id == 601 &&
            std::fabs(hood.slots[0].flap_hi - 0.873f) < 1e-4f &&
            hood.slots[0].flap_axis[0] == 1.f &&
            std::fabs(hood.slots[0].flap_pivot[2] + 0.797f) < 1e-4f,
        "hood flap hinge follows the slot");
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
