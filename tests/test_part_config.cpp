#include "standalone_test.hpp"

#include "slrr/part_config.hpp"

#include <cmath>
#include <string>

namespace {

const slrr::PartBody& body_at(const slrr::PartConfig& config, size_t index) {
  SLRR_REQUIRE(index < config.bodies.size());
  return config.bodies[index];
}

}  // namespace

SLRR_TEST(part_config_reads_native_box_sphere_and_mesh_bodies) {
  const auto config = slrr::parse_part_config(
      "body 1 2 3 0.1 0.2 0.3 0 box 0.05 3 0.05\n"
      "body 0 1 0 0 0 0 2 sphere 0.75\n"
      "body 0 0 0 0 0 0 1 physics\\tree.scx\n"
      "nocollision\n"
      "render 0x123\n");
  SLRR_REQUIRE(config.errors.empty());
  SLRR_REQUIRE(config.bodies.size() == 3);

  const auto& box = body_at(config, 0);
  SLRR_REQUIRE(box.shape == slrr::PartBodyShape::Box);
  SLRR_REQUIRE(box.collision);
  SLRR_REQUIRE(std::fabs(box.pose.position[0] - 1.f) < 1e-6f);
  SLRR_REQUIRE(std::fabs(box.pose.rotation[2] - .3f) < 1e-6f);
  SLRR_REQUIRE(std::fabs(box.box_half[0] - .05f) < 1e-6f);
  SLRR_REQUIRE(std::fabs(box.box_half[1] - 3.f) < 1e-6f);
  SLRR_REQUIRE(std::fabs(box.box_half[2] - .05f) < 1e-6f);

  const auto& sphere = body_at(config, 1);
  SLRR_REQUIRE(sphere.shape == slrr::PartBodyShape::Sphere);
  SLRR_REQUIRE(sphere.collision);
  SLRR_REQUIRE(std::fabs(sphere.sphere_radius - .75f) < 1e-6f);
  SLRR_REQUIRE(std::fabs(config.mass - 3.f) < 1e-6f);

  const auto& mesh = body_at(config, 2);
  SLRR_REQUIRE(mesh.shape == slrr::PartBodyShape::Mesh);
  SLRR_REQUIRE(!mesh.collision);
  SLRR_REQUIRE(mesh.collision_mesh.empty());
}

SLRR_TEST(part_config_keeps_the_mass_of_malformed_native_primitives) {
  // A bad extent must not drop the body: its mass still counts, as it did
  // before shapes were parsed, and the CFG stays valid.
  for (const char* line : {"body 0 0 0 0 0 0 40 box 1 0 1\n", "body 0 0 0 0 0 0 40 box 1 1\n",
                           "body 0 0 0 0 0 0 40 box 1 x 1\n", "body 0 0 0 0 0 0 40 sphere\n",
                           "body 0 0 0 0 0 0 40 sphere -2\n"}) {
    const auto config = slrr::parse_part_config(std::string(line) + "render 0x123\n");
    SLRR_REQUIRE(config.errors.empty());
    SLRR_REQUIRE(config.valid);
    SLRR_REQUIRE(config.bodies.size() == 1);
    SLRR_REQUIRE(config.bodies[0].shape == slrr::PartBodyShape::None);
    SLRR_REQUIRE(config.mass == 40.f);
    SLRR_REQUIRE(config.unknown.size() == 1);
  }
}

SLRR_TEST(part_config_keeps_unknown_body_shapes_compatible) {
  const auto config = slrr::parse_part_config(
      "body 0 0 0 0 0 0 0 capsule 1 2\n"
      "render 0x123\n");
  SLRR_REQUIRE(config.errors.empty());
  SLRR_REQUIRE(config.bodies.size() == 1);
  SLRR_REQUIRE(config.bodies[0].shape == slrr::PartBodyShape::None);
  SLRR_REQUIRE(config.bodies[0].collision);
}

int main() {
  const slrr::standalone::Test tests[] = {
      part_config_reads_native_box_sphere_and_mesh_bodies,
      part_config_keeps_the_mass_of_malformed_native_primitives,
      part_config_keeps_unknown_body_shapes_compatible,
  };
  return slrr::standalone::run("part_config", tests);
}
