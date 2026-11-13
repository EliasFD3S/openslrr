#include "standalone_test.hpp"

#include "slrr/scene_collision.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

using slrr::PartMatrix;
using slrr::ScxGeometry;
using slrr::collision::Box;
using slrr::collision::Contact;
using slrr::collision::QueryStats;
using slrr::collision::Sphere;
using slrr::collision::StaticIndex;
using slrr::physics::Vec3;

constexpr double kTolerance = 1e-7;

PartMatrix identity(Vec3 centre = {}) {
  PartMatrix result{};
  result[0] = result[5] = result[10] = result[15] = 1.0f;
  result[12] = static_cast<float>(centre.x);
  result[13] = static_cast<float>(centre.y);
  result[14] = static_cast<float>(centre.z);
  return result;
}

PartMatrix rotated_y(double angle, Vec3 centre = {}) {
  PartMatrix result = identity(centre);
  const float c = static_cast<float>(std::cos(angle));
  const float s = static_cast<float>(std::sin(angle));
  result[0] = c;
  result[2] = -s;
  result[8] = s;
  result[10] = c;
  return result;
}

bool near(double a, double b, double tolerance = kTolerance) {
  return std::abs(a - b) <= tolerance;
}

bool near(Vec3 a, Vec3 b, double tolerance = kTolerance) {
  return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
         near(a.z, b.z, tolerance);
}

double length(Vec3 value) { return std::sqrt(value.dot(value)); }

void require_unit(Vec3 value) { SLRR_REQUIRE(near(length(value), 1.0, 1e-8)); }

void require_valid_mesh(const ScxGeometry& mesh) {
  SLRR_REQUIRE(mesh.info.ok);
  SLRR_REQUIRE(mesh.position_scale == 1.0f);
  SLRR_REQUIRE(mesh.info.error.empty());
  SLRR_REQUIRE(!mesh.positions.empty());
  SLRR_REQUIRE(mesh.positions.size() % 3 == 0);
  SLRR_REQUIRE(mesh.normals.size() == mesh.positions.size());
  SLRR_REQUIRE(!mesh.indices.empty());
  SLRR_REQUIRE(mesh.indices.size() % 3 == 0);
  SLRR_REQUIRE(mesh.info.vertices == static_cast<int32_t>(mesh.positions.size() / 3));
  SLRR_REQUIRE(mesh.info.triangles == static_cast<int32_t>(mesh.indices.size() / 3));
  SLRR_REQUIRE(mesh.info.groups == 1);
  SLRR_REQUIRE(mesh.groups.size() == 1);
  SLRR_REQUIRE(mesh.groups[0].index_start == 0);
  SLRR_REQUIRE(mesh.groups[0].index_count == mesh.indices.size());
  for (int axis = 0; axis < 3; ++axis) {
    float minimum = mesh.positions[axis];
    float maximum = minimum;
    for (std::size_t i = axis + 3; i < mesh.positions.size(); i += 3) {
      minimum = std::min(minimum, mesh.positions[i]);
      maximum = std::max(maximum, mesh.positions[i]);
    }
    SLRR_REQUIRE(mesh.info.bbox_min[axis] == minimum);
    SLRR_REQUIRE(mesh.info.bbox_max[axis] == maximum);
  }
  for (uint32_t index : mesh.indices)
    SLRR_REQUIRE(index < mesh.positions.size() / 3);
}

}  // namespace

SLRR_TEST(box_mesh_is_world_space_geometry_in_metres) {
  const Vec3 centre{10, 2, -3};
  const Vec3 half{1.25, 0.5, 2.0};
  const ScxGeometry mesh = slrr::collision::make_box_mesh(identity(centre), half);
  require_valid_mesh(mesh);
  SLRR_REQUIRE(mesh.positions.size() == 24 * 3);
  SLRR_REQUIRE(mesh.indices.size() == 36);
  SLRR_REQUIRE(mesh.uvs.size() == 24 * 2);
  SLRR_REQUIRE(near(mesh.info.bbox_min[0], centre.x - half.x, 1e-6));
  SLRR_REQUIRE(near(mesh.info.bbox_min[1], centre.y - half.y, 1e-6));
  SLRR_REQUIRE(near(mesh.info.bbox_min[2], centre.z - half.z, 1e-6));
  SLRR_REQUIRE(near(mesh.info.bbox_max[0], centre.x + half.x, 1e-6));
  SLRR_REQUIRE(near(mesh.info.bbox_max[1], centre.y + half.y, 1e-6));
  SLRR_REQUIRE(near(mesh.info.bbox_max[2], centre.z + half.z, 1e-6));
}

SLRR_TEST(sphere_mesh_has_bounded_metre_positions_and_real_triangles) {
  const Vec3 centre{2, 1, -4};
  constexpr double radius = 0.75;
  constexpr uint32_t segments = 16;
  constexpr uint32_t rings = 8;
  const ScxGeometry mesh =
      slrr::collision::make_sphere_mesh(identity(centre), radius, segments, rings);
  require_valid_mesh(mesh);
  SLRR_REQUIRE(mesh.info.vertices == 2 + (rings - 1) * segments);
  SLRR_REQUIRE(mesh.info.triangles == 2 * (rings - 1) * segments);
  for (int axis = 0; axis < 3; ++axis) {
    const double centre_component = axis == 0 ? centre.x : axis == 1 ? centre.y : centre.z;
    SLRR_REQUIRE(near(mesh.info.bbox_min[axis], centre_component - radius, 1e-6));
    SLRR_REQUIRE(near(mesh.info.bbox_max[axis], centre_component + radius, 1e-6));
  }
  for (std::size_t i = 0; i < mesh.positions.size(); i += 3) {
    const Vec3 point{mesh.positions[i], mesh.positions[i + 1], mesh.positions[i + 2]};
    SLRR_REQUIRE(near(length(point - centre), radius, 1e-6));
  }
  for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
    Vec3 vertex[3];
    for (int corner = 0; corner < 3; ++corner) {
      const std::size_t offset = static_cast<std::size_t>(mesh.indices[i + corner]) * 3;
      vertex[corner] = {mesh.positions[offset], mesh.positions[offset + 1],
                        mesh.positions[offset + 2]};
    }
    SLRR_REQUIRE(length((vertex[1] - vertex[0]).cross(vertex[2] - vertex[0])) > 1e-9);
  }
}

SLRR_TEST(invalid_dimensions_and_frames_are_rejected_without_geometry) {
  Box box;
  Sphere sphere;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  SLRR_REQUIRE(!slrr::collision::make_box(identity(), {1, 0, 1}, box));
  SLRR_REQUIRE(!slrr::collision::make_box(identity(), {-1, 1, 1}, box));
  SLRR_REQUIRE(!slrr::collision::make_box(identity(), {1, nan, 1}, box));
  SLRR_REQUIRE(!slrr::collision::make_sphere(identity(), 0, sphere));
  SLRR_REQUIRE(!slrr::collision::make_sphere(identity(), -1, sphere));
  SLRR_REQUIRE(!slrr::collision::make_sphere(
      identity(), std::numeric_limits<double>::infinity(), sphere));
  PartMatrix singular{};
  SLRR_REQUIRE(!slrr::collision::make_box(singular, {1, 1, 1}, box));

  ScxGeometry mesh = slrr::collision::make_box_mesh(identity(), {0, 1, 1});
  SLRR_REQUIRE(!mesh.info.ok);
  SLRR_REQUIRE(!mesh.info.error.empty());
  SLRR_REQUIRE(mesh.position_scale == 1.0f);
  SLRR_REQUIRE(mesh.positions.empty());
  SLRR_REQUIRE(mesh.indices.empty());
  mesh = slrr::collision::make_sphere_mesh(identity(), 1, 2, 8);
  SLRR_REQUIRE(!mesh.info.ok);
  SLRR_REQUIRE(mesh.indices.empty());
  mesh = slrr::collision::make_sphere_mesh(
      identity(), 1, std::numeric_limits<uint32_t>::max(),
      std::numeric_limits<uint32_t>::max());
  SLRR_REQUIRE(!mesh.info.ok);
  SLRR_REQUIRE(mesh.indices.empty());

  StaticIndex index;
  SLRR_REQUIRE(!index.add_box(identity(), {1, -1, 1}));
  SLRR_REQUIRE(!index.add_sphere(identity(), 0));
  SLRR_REQUIRE(index.size() == 0);
  bool threw = false;
  try {
    StaticIndex invalid(0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  SLRR_REQUIRE(threw);
}

SLRR_TEST(oriented_vehicle_box_finds_a_thin_trunk_between_corner_probes) {
  constexpr double probe_radius = 0.18;
  StaticIndex index(2.0);
  int trunk_owner = 0;
  SLRR_REQUIRE(index.add_box(identity({0, 1.5, 0}), {0.08, 1.5, 0.08},
                             &trunk_owner));
  const Vec3 moving_centre{0.75, 1.4, 0};
  const Vec3 moving_half{1.0, 0.3, 1.6};

  // This is the blind spot of four corner sphere probes: every probe is clear,
  // while the oriented chassis volume contains the trunk.
  for (double x : {-1.0, 1.0})
    for (double z : {-1.0, 1.0}) {
      const Vec3 probe = moving_centre +
                         Vec3{x * (moving_half.x - probe_radius), 0,
                              z * (moving_half.z - probe_radius)};
      const Vec3 closest{std::clamp(probe.x, -0.08, 0.08), probe.y,
                         std::clamp(probe.z, -0.08, 0.08)};
      SLRR_REQUIRE(length(probe - closest) > probe_radius);
    }

  QueryStats stats;
  const auto hit = index.overlap_box(identity(moving_centre), moving_half, nullptr, &stats);
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(hit->owner == &trunk_owner);
  SLRR_REQUIRE(hit->shape_index == 0);
  SLRR_REQUIRE(near(hit->normal, {1, 0, 0}));
  SLRR_REQUIRE(near(hit->penetration, 0.33, 1e-8));
  SLRR_REQUIRE(near(hit->point, {0.08, 1.4, 0}, 1e-6));
  SLRR_REQUIRE(stats.broadphase_candidates == 1);
  SLRR_REQUIRE(stats.narrowphase_tests == 1);
}

SLRR_TEST(rotated_box_sat_returns_the_obstacle_to_moving_normal) {
  constexpr double angle = 0.63;
  const PartMatrix obstacle_frame = rotated_y(angle);
  const Vec3 local_x{std::cos(angle), 0, -std::sin(angle)};
  StaticIndex index(2.0);
  SLRR_REQUIRE(index.add_box(obstacle_frame, {1.0, 0.5, 0.25}));
  const PartMatrix moving_frame = rotated_y(angle, local_x * 1.3 + Vec3{0, 0.1, 0});
  const auto hit = index.overlap_box(moving_frame, {0.5, 0.5, 0.25});
  SLRR_REQUIRE(hit.has_value());
  require_unit(hit->normal);
  SLRR_REQUIRE(near(hit->normal, local_x, 1e-7));
  SLRR_REQUIRE(near(hit->penetration, 0.2, 1e-7));
}

SLRR_TEST(rotated_separated_boxes_survive_broadphase_but_fail_sat) {
  constexpr double angle = 0.7;
  const Vec3 local_x{std::cos(angle), 0, -std::sin(angle)};
  StaticIndex index(16.0);
  SLRR_REQUIRE(index.add_box(rotated_y(angle), {1.0, 1.0, 0.1}));
  QueryStats stats;
  const auto hit = index.overlap_box(rotated_y(angle, local_x * 2.1),
                                     {1.0, 1.0, 0.1}, nullptr, &stats);
  SLRR_REQUIRE(!hit.has_value());
  SLRR_REQUIRE(stats.broadphase_candidates == 1);
  SLRR_REQUIRE(stats.narrowphase_tests == 1);  // Their world AABBs still overlap.
}

SLRR_TEST(sphere_contact_uses_closest_point_and_sphere_surface) {
  StaticIndex index;
  int sphere_owner = 0;
  SLRR_REQUIRE(index.add_sphere(identity(), 0.5, &sphere_owner));
  const auto hit = index.overlap_box(identity({1.2, 0, 0}), {1.0, 0.5, 0.5});
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(hit->owner == &sphere_owner);
  SLRR_REQUIRE(near(hit->normal, {1, 0, 0}));
  SLRR_REQUIRE(near(hit->penetration, 0.3));
  SLRR_REQUIRE(near(hit->point, {0.5, 0, 0}));
}

SLRR_TEST(sphere_centre_inside_box_has_a_deterministic_exit_contact) {
  StaticIndex index;
  SLRR_REQUIRE(index.add_sphere(identity({0.8, 0, 0}), 0.2));
  auto hit = index.overlap_box(identity(), {1, 1, 1});
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(near(hit->normal, {-1, 0, 0}));
  SLRR_REQUIRE(near(hit->penetration, 0.4));
  SLRR_REQUIRE(near(hit->point, {1.0, 0, 0}));

  index.clear();
  SLRR_REQUIRE(index.add_sphere(identity(), 0.2));
  hit = index.overlap_box(identity(), {1, 2, 3});
  SLRR_REQUIRE(hit.has_value());
  require_unit(hit->normal);
  SLRR_REQUIRE(near(hit->normal, {1, 0, 0}));
  SLRR_REQUIRE(near(hit->penetration, 1.2));
  SLRR_REQUIRE(near(hit->point, {-0.2, 0, 0}));
}

SLRR_TEST(coincident_boxes_still_return_finite_inside_contact) {
  StaticIndex index;
  SLRR_REQUIRE(index.add_box(identity(), {1, 2, 3}));
  const auto hit = index.overlap_box(identity(), {1, 2, 3});
  SLRR_REQUIRE(hit.has_value());
  require_unit(hit->normal);
  SLRR_REQUIRE(near(hit->penetration, 2.0));
  SLRR_REQUIRE(std::isfinite(hit->point.x));
  SLRR_REQUIRE(std::isfinite(hit->point.y));
  SLRR_REQUIRE(std::isfinite(hit->point.z));
  SLRR_REQUIRE(near(length(hit->point - Vec3{1, 0, 0}), 0.0));
}

SLRR_TEST(broadphase_rejects_distant_cells_and_owner_filter_skips_narrowphase) {
  StaticIndex index(1.0);
  int near_owner = 0;
  int far_owner = 0;
  SLRR_REQUIRE(index.add_box(identity(), {0.5, 0.5, 0.5}, &near_owner));
  SLRR_REQUIRE(index.add_box(identity({100, 0, 0}), {0.5, 0.5, 0.5}, &far_owner));
  QueryStats stats;
  auto hit = index.overlap_box(identity(), {0.5, 0.5, 0.5}, nullptr, &stats);
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(hit->owner == &near_owner);
  SLRR_REQUIRE(stats.broadphase_candidates == 1);
  SLRR_REQUIRE(stats.narrowphase_tests == 1);

  hit = index.overlap_box(identity(), {0.5, 0.5, 0.5}, &near_owner, &stats);
  SLRR_REQUIRE(!hit.has_value());
  SLRR_REQUIRE(stats.broadphase_candidates == 1);
  SLRR_REQUIRE(stats.narrowphase_tests == 0);
}

SLRR_TEST(oversized_static_shapes_remain_queryable_without_grid_explosion) {
  StaticIndex index(1.0);
  SLRR_REQUIRE(index.add_box(identity(), {100, 1, 100}));
  QueryStats stats;
  const auto hit = index.overlap_box(identity({50, 0, 0}), {1, 1, 1}, nullptr, &stats);
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(stats.broadphase_candidates == 1);
  SLRR_REQUIRE(stats.narrowphase_tests == 1);
  index.clear();
  SLRR_REQUIRE(index.size() == 0);
  SLRR_REQUIRE(!index.overlap_box(identity({50, 0, 0}), {1, 1, 1}).has_value());
}

SLRR_TEST(analytic_bounds_cover_full_sphere_and_clear_with_index) {
  StaticIndex index;
  SLRR_REQUIRE(!index.bounds().has_value());
  SLRR_REQUIRE(index.add_sphere(rotated_y(0.63, {2, 1, -4}), 1.0));
  SLRR_REQUIRE(index.add_box(identity({10, 0, 0}), {1, 2, 3}));
  const auto bounds = index.bounds();
  SLRR_REQUIRE(bounds.has_value());
  SLRR_REQUIRE(near(bounds->minimum.x, 1.0));
  SLRR_REQUIRE(near(bounds->maximum.x, 11.0));
  SLRR_REQUIRE(near(bounds->minimum.y, -2.0));
  SLRR_REQUIRE(near(bounds->maximum.y, 2.0));
  SLRR_REQUIRE(near(bounds->minimum.z, -5.0));
  SLRR_REQUIRE(near(bounds->maximum.z, 3.0));
  index.clear();
  SLRR_REQUIRE(!index.bounds().has_value());
}

SLRR_TEST(box_pressed_into_a_wall_triangle_is_pushed_out_along_the_face) {
  using slrr::collision::overlap_triangle;
  // A vertical wall in the x = 0 plane, facing +x, larger than the box.
  const std::array<Vec3, 3> wall = {Vec3{0, -5, -10}, Vec3{0, 10, 0}, Vec3{0, -5, 10}};
  Box box;
  SLRR_REQUIRE(slrr::collision::make_box(identity({0.8, 0.5, 0.3}), {1.0, 0.4, 2.0}, box));
  const auto hit = overlap_triangle(box, wall);
  SLRR_REQUIRE(hit.has_value());
  require_unit(hit->normal);
  SLRR_REQUIRE(near(hit->normal, {1, 0, 0}));
  SLRR_REQUIRE(near(hit->penetration, 0.2));
  SLRR_REQUIRE(near(hit->point, {0, 0.5, 0.3}));

  // The same wall seen from its other side pushes the box back to that side.
  SLRR_REQUIRE(slrr::collision::make_box(identity({-0.9, 0.5, 0.3}), {1.0, 0.4, 2.0}, box));
  const auto back = overlap_triangle(box, wall);
  SLRR_REQUIRE(back.has_value());
  SLRR_REQUIRE(near(back->normal, {-1, 0, 0}));
  SLRR_REQUIRE(near(back->penetration, 0.1));
}

SLRR_TEST(house_corner_between_corner_probes_is_found_by_the_chassis_box) {
  using slrr::collision::overlap_triangle;
  constexpr double probe_radius = 0.18;
  // The convex corner of a house at the origin, walls along -x and -z.
  // A car heads towards -z with the corner just inside its front bumper.
  const Vec3 car_centre{0.0, 0.6, 2.2};
  const Vec3 car_half{0.9, 0.4, 2.3};
  const std::array<Vec3, 3> side_wall = {Vec3{0, 0, 0}, Vec3{0, 3, 0}, Vec3{0, 0, -10}};
  const std::array<Vec3, 3> front_wall = {Vec3{0, 0, 0}, Vec3{-10, 0, 0}, Vec3{0, 3, 0}};
  // Both front corner probes miss: they straddle the corner by 0.72 m.
  for (double x : {-1.0, 1.0}) {
    const Vec3 probe = car_centre + Vec3{x * (car_half.x - probe_radius), 0,
                                          -(car_half.z - probe_radius)};
    SLRR_REQUIRE(std::abs(probe.x) > probe_radius);
  }
  Box box;
  SLRR_REQUIRE(slrr::collision::make_box(identity(car_centre), car_half, box));
  const auto side = overlap_triangle(box, side_wall);
  const auto front = overlap_triangle(box, front_wall);
  SLRR_REQUIRE(side.has_value());
  SLRR_REQUIRE(front.has_value());
  // The bumper is 0.1 m past the front wall: that is the shallow way out.
  SLRR_REQUIRE(near(front->normal, {0, 0, 1}));
  SLRR_REQUIRE(near(front->penetration, 0.1));
}

SLRR_TEST(separated_and_degenerate_triangles_do_not_overlap) {
  using slrr::collision::overlap_triangle;
  Box box;
  SLRR_REQUIRE(slrr::collision::make_box(rotated_y(0.4, {0, 1, 0}), {1, 1, 1}, box));
  // Beyond the rotated box's corner reach (sqrt 2 in the xz plane).
  const std::array<Vec3, 3> far_wall = {Vec3{1.5, -5, -10}, Vec3{1.5, 10, 0}, Vec3{1.5, -5, 10}};
  SLRR_REQUIRE(!overlap_triangle(box, far_wall).has_value());
  const std::array<Vec3, 3> line = {Vec3{0, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 2, 0}};
  SLRR_REQUIRE(!overlap_triangle(box, line).has_value());
  const std::array<Vec3, 3> invalid = {Vec3{0, 0, 0}, Vec3{std::nan(""), 1, 0}, Vec3{0, 2, 1}};
  SLRR_REQUIRE(!overlap_triangle(box, invalid).has_value());
}

int main() {
  const slrr::standalone::Test tests[] = {
      box_pressed_into_a_wall_triangle_is_pushed_out_along_the_face,
      house_corner_between_corner_probes_is_found_by_the_chassis_box,
      separated_and_degenerate_triangles_do_not_overlap,
      box_mesh_is_world_space_geometry_in_metres,
      sphere_mesh_has_bounded_metre_positions_and_real_triangles,
      invalid_dimensions_and_frames_are_rejected_without_geometry,
      oriented_vehicle_box_finds_a_thin_trunk_between_corner_probes,
      rotated_box_sat_returns_the_obstacle_to_moving_normal,
      rotated_separated_boxes_survive_broadphase_but_fail_sat,
      sphere_contact_uses_closest_point_and_sphere_surface,
      sphere_centre_inside_box_has_a_deterministic_exit_contact,
      coincident_boxes_still_return_finite_inside_contact,
      broadphase_rejects_distant_cells_and_owner_filter_skips_narrowphase,
      oversized_static_shapes_remain_queryable_without_grid_explosion,
      analytic_bounds_cover_full_sphere_and_clear_with_index,
  };
  return slrr::standalone::run("scene_collision", tests);
}
