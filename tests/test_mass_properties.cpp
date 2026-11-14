#include "standalone_test.hpp"

#include "slrr/mass_properties.hpp"

#include <cmath>
#include <vector>

using slrr::physics::MassProperties;
using slrr::physics::Matrix;
using slrr::physics::Vec3;

namespace {

bool near(double a, double b, double tolerance = 1e-6) {
  return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b));
}

// Axis-aligned box as a triangle soup, in centimetres like an SCX phys hull.
// Vertices are duplicated per face, as SCX splits them by normal.
void box_soup(Vec3 lo, Vec3 hi, bool inward, std::vector<float>& positions,
              std::vector<uint32_t>& indices) {
  const auto corner = [&](int i) {
    return Vec3{i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z};
  };
  // Outward-wound quads (counter-clockwise seen from outside).
  const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                           {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& face : faces) {
    const auto base = static_cast<uint32_t>(positions.size() / 3);
    for (int k : face) {
      const Vec3 p = corner(k);
      positions.insert(positions.end(), {float(p.x * 100), float(p.y * 100), float(p.z * 100)});
    }
    const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; ++k)
      indices.push_back(base + quad[inward ? 5 - k : k]);
  }
}

Matrix yaw_frame(double angle, Vec3 origin) {
  const float c = float(std::cos(angle)), s = float(std::sin(angle));
  return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0,
          float(origin.x), float(origin.y), float(origin.z), 1};
}

}  // namespace

SLRR_TEST(closed_phys_hull_matches_the_analytic_box) {
  for (bool inward : {false, true}) {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    box_soup({0.2, -0.1, 1.0}, {1.0, 0.3, 2.2}, inward, positions, indices);
    MassProperties mesh;
    SLRR_REQUIRE(slrr::physics::mesh_mass(60.0, positions.data(), positions.size(), 0.01,
                                          indices.data(), indices.size(), mesh));
    const auto box = slrr::physics::box_mass(60.0, {0.6, 0.1, 1.6}, {0.4, 0.2, 0.6});
    SLRR_REQUIRE(near(mesh.mass, 60.0));
    SLRR_REQUIRE(near(mesh.center.x, 0.6, 1e-5) && near(mesh.center.y, 0.1, 1e-5) &&
                 near(mesh.center.z, 1.6, 1e-5));
    for (size_t i = 0; i < 9; ++i)
      SLRR_REQUIRE(std::fabs(mesh.inertia[i] - box.inertia[i]) < 1e-4);
  }
}

SLRR_TEST(open_shell_is_rejected_for_the_bounds_fallback) {
  std::vector<float> positions;
  std::vector<uint32_t> indices;
  box_soup({0, 0, 0}, {1, 1, 1}, false, positions, indices);
  indices.resize(indices.size() - 12);  // drop two faces
  MassProperties mesh;
  SLRR_REQUIRE(!slrr::physics::mesh_mass(10.0, positions.data(), positions.size(), 0.01,
                                         indices.data(), indices.size(), mesh));
  SLRR_REQUIRE(mesh.empty());
}

SLRR_TEST(parallel_axis_sum_of_two_points_is_the_dumbbell) {
  MassProperties sum;
  slrr::physics::accumulate(sum, slrr::physics::point_mass(2.0, {-1, 0, 0}));
  slrr::physics::accumulate(sum, slrr::physics::point_mass(2.0, {1, 0, 0}));
  SLRR_REQUIRE(near(sum.mass, 4.0));
  SLRR_REQUIRE(near(sum.center.x, 0.0) && near(sum.center.y, 0.0));
  SLRR_REQUIRE(near(sum.inertia[0], 0.0));
  SLRR_REQUIRE(near(sum.inertia[4], 4.0) && near(sum.inertia[8], 4.0));
}

SLRR_TEST(split_box_sums_to_the_whole_box) {
  // Two halves of a 2 x 1 x 4 m box, added in a rotated and moved frame.
  const auto front = slrr::physics::box_mass(50.0, {0, 0, -1}, {1, 0.5, 1});
  const auto rear = slrr::physics::box_mass(50.0, {0, 0, 1}, {1, 0.5, 1});
  const auto whole = slrr::physics::box_mass(100.0, {0, 0, 0}, {1, 0.5, 2});
  MassProperties sum;
  slrr::physics::accumulate(sum, front);
  slrr::physics::accumulate(sum, rear);
  for (size_t i = 0; i < 9; ++i) SLRR_REQUIRE(std::fabs(sum.inertia[i] - whole.inertia[i]) < 1e-9);
  const auto frame = yaw_frame(0.7, {3, 1, -2});
  MassProperties moved;
  slrr::physics::accumulate(moved, slrr::physics::transformed(front, frame));
  slrr::physics::accumulate(moved, slrr::physics::transformed(rear, frame));
  const auto expected = slrr::physics::transformed(whole, frame);
  SLRR_REQUIRE(near(moved.center.x, 3) && near(moved.center.y, 1) && near(moved.center.z, -2));
  for (size_t i = 0; i < 9; ++i)
    SLRR_REQUIRE(std::fabs(moved.inertia[i] - expected.inertia[i]) < 1e-4);
  // Yaw keeps the vertical moment and mixes the horizontal ones.
  SLRR_REQUIRE(near(expected.inertia[4], whole.inertia[4], 1e-5));
  SLRR_REQUIRE(std::fabs(expected.inertia[2]) > 1);
}

SLRR_TEST(sphere_and_degenerate_inputs) {
  const auto sphere = slrr::physics::sphere_mass(10.0, {1, 2, 3}, 0.5);
  SLRR_REQUIRE(near(sphere.inertia[0], 1.0) && near(sphere.inertia[8], 1.0));
  SLRR_REQUIRE(slrr::physics::point_mass(0.0, {}).empty());
  SLRR_REQUIRE(slrr::physics::point_mass(-1.0, {}).empty());
  MassProperties sum;
  slrr::physics::accumulate(sum, slrr::physics::point_mass(0.0, {5, 5, 5}));
  SLRR_REQUIRE(sum.empty());
}

// The body-less fallback: CFG masses as points at the part origins.
SLRR_TEST(point_mass_fallback_keeps_mass_and_centre_with_unknown_spread) {
  MassProperties single;
  slrr::physics::accumulate(single, slrr::physics::point_mass(900.0, {0.1, -0.2, 0.3}));
  SLRR_REQUIRE(near(single.mass, 900.0) && near(single.center.z, 0.3));
  // One point has no spread: every moment is zero, which callers read as
  // "unknown" and replace with the bounds box.
  SLRR_REQUIRE(single.moments().x == 0 && single.moments().y == 0 && single.moments().z == 0);
  MassProperties pair = single;
  slrr::physics::accumulate(pair, slrr::physics::point_mass(100.0, {0.1, -0.2, -1.7}));
  SLRR_REQUIRE(near(pair.mass, 1000.0) && near(pair.center.z, 0.1));
  SLRR_REQUIRE(near(pair.inertia[0], 900 * 0.04 + 100 * 3.24) && pair.inertia[8] == 0);
}

int main() {
  constexpr slrr::standalone::Test tests[] = {
      closed_phys_hull_matches_the_analytic_box,
      open_shell_is_rejected_for_the_bounds_fallback,
      parallel_axis_sum_of_two_points_is_the_dumbbell,
      split_box_sums_to_the_whole_box,
      sphere_and_degenerate_inputs,
      point_mass_fallback_keeps_mass_and_centre_with_unknown_spread,
  };
  return slrr::standalone::run("mass_properties", tests);
}
