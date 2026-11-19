// Portable kernel regressions; from the repository root:
// g++ -std=c++20 -O2 -Iinclude -Itests tests/test_vehicle_contacts.cpp
//   src/physics/vehicle_contacts.cpp src/physics/scene_collision.cpp
//   src/physics/vehicle_dynamics.cpp src/physics/tire_model.cpp
//   src/physics/suspension_geometry.cpp -o /tmp/test_vehicle_contacts
#include "standalone_test.hpp"

#include "slrr/scene_collision.hpp"
#include "slrr/vehicle_contacts.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
using namespace slrr::physics;
using slrr::collision::Box;
using slrr::collision::overlap_boxes;

bool near(double a, double b, double tolerance = 1e-8) {
  return std::abs(a - b) <= tolerance;
}
bool near(Vec3 a, Vec3 b, double tolerance = 1e-8) {
  return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
         near(a.z, b.z, tolerance);
}
RigidBody car(double mass = 1000, Vec3 position = {}, Vec3 velocity = {},
              double yaw = 0) {
  RigidBody body;
  body.configure(mass, {-.9, -.5, -2}, {.9, .5, 2}, {});
  body.position = position;
  body.velocity = velocity;
  body.orientation = {0, std::sin(yaw * .5), 0, std::cos(yaw * .5)};
  return body;
}
Vec3 linear_momentum(const RigidBody& a, const RigidBody& b) {
  return a.velocity * a.mass + b.velocity * b.mass;
}
Vec3 center_of_mass(const RigidBody& a, const RigidBody& b) {
  return (a.position * a.mass + b.position * b.mass) / (a.mass + b.mass);
}
void same_state(const RigidBody& a, const RigidBody& b) {
  SLRR_REQUIRE(near(a.position, b.position));
  SLRR_REQUIRE(near(a.velocity, b.velocity));
  SLRR_REQUIRE(near(a.momentum, b.momentum));
  SLRR_REQUIRE(near(a.orientation.x, b.orientation.x));
  SLRR_REQUIRE(near(a.orientation.y, b.orientation.y));
  SLRR_REQUIRE(near(a.orientation.z, b.orientation.z));
  SLRR_REQUIRE(near(a.orientation.w, b.orientation.w));
}

}  // namespace

SLRR_TEST(general_box_pair_query_reuses_validated_sat_without_an_index) {
  Box a, b;
  a.half_extents = b.half_extents = {1, .5, 2};
  a.center.z = 3.8;
  const auto hit = overlap_boxes(a, b);
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(near(hit->normal, {0, 0, 1}));
  SLRR_REQUIRE(near(hit->penetration, .2));
  SLRR_REQUIRE(near(hit->point, {0, 0, 2}));
  SLRR_REQUIRE(hit->owner == nullptr);
  const auto reverse = overlap_boxes(b, a);
  SLRR_REQUIRE(reverse.has_value());
  SLRR_REQUIRE(near(reverse->normal, -hit->normal));
  b.center.y = 1.01;
  SLRR_REQUIRE(!overlap_boxes(a, b));
  b.center = {};
  b.axes[0] = {};
  SLRR_REQUIRE(!overlap_boxes(a, b));
  b = a;
  b.half_extents.x = 0;
  SLRR_REQUIRE(!overlap_boxes(a, b));
  b = a;
  b.center.z = std::numeric_limits<double>::quiet_NaN();
  SLRR_REQUIRE(!overlap_boxes(a, b));
  b = a;
  b.axes[1] = b.axes[0];
  SLRR_REQUIRE(!overlap_boxes(a, b));
  a.center = b.center = {};
  a.axes = b.axes = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
  a.half_extents = b.half_extents = {1e308, 1e308, 1e308};
  SLRR_REQUIRE(!overlap_boxes(a, b)); // Finite inputs must not yield infinite metadata.
}

SLRR_TEST(unequal_masses_exchange_the_actual_normal_impulse_once) {
  auto a = car(1000, {0, 0, 1.9}, {0, 0, -12});
  auto b = car(2000, {0, 0, -1.9}, {0, 0, 4});
  const Vec3 before = linear_momentum(a, b), com = center_of_mass(a, b);
  const Vec3 va = a.velocity, vb = b.velocity;
  const auto hit = resolve_vehicle_contact(a, b);
  SLRR_REQUIRE(hit.has_value());
  const double expected = 16 * 1.08 / (1.0 / a.mass + 1.0 / b.mass);
  SLRR_REQUIRE(near(hit->speed, 16));
  SLRR_REQUIRE(near(hit->normal, {0, 0, 1}));
  SLRR_REQUIRE(near(hit->point, {}));
  SLRR_REQUIRE(near(hit->penetration, .2));
  SLRR_REQUIRE(near(hit->normal_impulse, expected));
  SLRR_REQUIRE(near(hit->impulse, {0, 0, expected}));
  SLRR_REQUIRE(near((a.velocity - va) * a.mass, hit->impulse));
  SLRR_REQUIRE(near((b.velocity - vb) * b.mass, -hit->impulse));
  SLRR_REQUIRE(near(linear_momentum(a, b), before));
  SLRR_REQUIRE(near(center_of_mass(a, b), com));
  SLRR_REQUIRE(near((a.velocity - b.velocity).dot(hit->normal), 16 * .08));
  SLRR_REQUIRE(near(a.momentum, {}) && near(b.momentum, {}));
  SLRR_REQUIRE(near(a.position.z, 1.9 + .1188 * 2 / 3));
  SLRR_REQUIRE(near(b.position.z, -1.9 - .1188 / 3));
}

SLRR_TEST(side_contact_handles_a_perpendicular_body) {
  constexpr double pi = 3.14159265358979323846;
  auto a = car(1200, {-2.7, 0, 0}, {12, 0, 0});
  auto b = car(1800, {}, {}, pi * .5);
  const Vec3 total = linear_momentum(a, b);
  const auto hit = resolve_vehicle_contact(a, b);
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(near(hit->normal, {-1, 0, 0}));
  SLRR_REQUIRE(near(hit->speed, 12));
  SLRR_REQUIRE(hit->normal_impulse > 9000);
  SLRR_REQUIRE(b.velocity.x > 4);
  SLRR_REQUIRE(a.velocity.x < 6);
  SLRR_REQUIRE(near(linear_momentum(a, b), total));
}

SLRR_TEST(offcentre_impulses_conserve_linear_and_angular_exchange) {
  auto a = car(950, {.6, 0, 3.8}, {2, 0, -15});
  auto b = car(2100, {}, {-.5, 0, 1});
  const auto before_a = a, before_b = b;
  const auto hit = resolve_vehicle_contact(a, b);
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(std::abs(a.omega().y) > .1);
  SLRR_REQUIRE(std::abs(b.omega().y) > .1);
  SLRR_REQUIRE(near(linear_momentum(a, b), linear_momentum(before_a, before_b)));
  SLRR_REQUIRE(near(a.momentum - before_a.momentum,
                    (hit->point - before_a.position).cross(hit->impulse)));
  SLRR_REQUIRE(near(b.momentum - before_b.momentum,
                    (hit->point - before_b.position).cross(-hit->impulse)));
  // Use pre-correction COM positions: positional stabilization is not an impulse.
  const Vec3 angular_before = before_a.momentum + before_b.momentum +
      before_a.position.cross(before_a.velocity * a.mass) +
      before_b.position.cross(before_b.velocity * b.mass);
  const Vec3 angular_after = a.momentum + b.momentum +
      before_a.position.cross(a.velocity * a.mass) +
      before_b.position.cross(b.velocity * b.mass);
  SLRR_REQUIRE(near(angular_before, angular_after));
  const Vec3 tangent = hit->impulse - hit->normal * hit->normal_impulse;
  SLRR_REQUIRE(tangent.length() <= .6 * hit->normal_impulse + 1e-8);
}

SLRR_TEST(angular_point_velocity_contributes_to_approach_speed) {
  auto a = car(1000, {.6, 0, 3.8});
  auto b = car(1400);
  a.set_omega({0, -2, 0});
  const auto old_a = a, old_b = b;
  const auto hit = resolve_vehicle_contact(a, b);
  SLRR_REQUIRE(hit.has_value());
  const double approach = -(old_a.point_velocity(hit->point) -
                            old_b.point_velocity(hit->point)).dot(hit->normal);
  SLRR_REQUIRE(approach > .5);
  SLRR_REQUIRE(near(hit->speed, approach));
  SLRR_REQUIRE(hit->normal_impulse > 0);
  SLRR_REQUIRE(b.velocity.z < 0);
  SLRR_REQUIRE(near(linear_momentum(a, b), {}));
}

SLRR_TEST(separated_same_height_and_overpass_pairs_do_not_interact) {
  const std::array<Vec3, 4> positions = {
      Vec3{1.81, 0, 0}, Vec3{0, 0, 4.01}, Vec3{0, 1.01, 0}, Vec3{100, 0, 0}};
  for (Vec3 position : positions) {
    auto a = car(1000, position, {-4, 0, -12});
    auto b = car(2000);
    const auto old_a = a, old_b = b;
    SLRR_REQUIRE(!resolve_vehicle_contact(a, b));
    same_state(a, old_a);
    same_state(b, old_b);
  }
}

SLRR_TEST(argument_and_creation_order_do_not_change_response) {
  auto a = car(1000, {.4, 0, 3.8}, {3, 0, -12}, .03);
  auto b = car(1800, {}, {-.2, 0, 1}, -.07);
  a.set_omega({.1, -.2, .3});
  b.set_omega({-.2, .1, .2});
  // Construct and pass the copies in reverse order, not by object-address sort.
  auto second_b = b;
  auto second_a = a;
  const auto first = resolve_vehicle_contact(a, b);
  const auto second = resolve_vehicle_contact(second_b, second_a);
  SLRR_REQUIRE(first && second);
  same_state(a, second_a);
  same_state(b, second_b);
  SLRR_REQUIRE(near(first->point, second->point));
  SLRR_REQUIRE(near(first->normal, -second->normal));
  SLRR_REQUIRE(near(first->impulse, -second->impulse));
  SLRR_REQUIRE(near(first->normal_impulse, second->normal_impulse));
  SLRR_REQUIRE(near(first->speed, second->speed));
  SLRR_REQUIRE(near(first->penetration, second->penetration));
}

SLRR_TEST(equal_depth_rotated_sat_ties_are_argument_symmetric) {
  constexpr double pi = 3.14159265358979323846;
  auto a = car(1000, {.2, 0, .2}, {2, 0, 3}, pi * .25);
  auto b = car(2000, {}, {-1, 0, -2}, -pi * .25);
  auto copy_b = b, copy_a = a;
  const auto forward = resolve_vehicle_contact(a, b);
  const auto reverse = resolve_vehicle_contact(copy_b, copy_a);
  SLRR_REQUIRE(forward && reverse);
  same_state(a, copy_a);
  same_state(b, copy_b);
  SLRR_REQUIRE(near(forward->normal, -reverse->normal));
}

SLRR_TEST(bounds_offset_and_local_com_do_not_move_the_collision_hull) {
  auto a = car(1000, {0, 0, 3.8}, {0, 0, -10});
  auto b = car(2000);
  // Same world-space shell, but move the local origin and local COM together.
  a.configure(1000, {9.1, -.5, -2}, {10.9, .5, 2}, {10, 0, 0});
  const auto hit = resolve_vehicle_contact(a, b);
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(near(hit->normal, {0, 0, 1}));
  SLRR_REQUIRE(near(hit->point, {0, 0, 1.9}));
  SLRR_REQUIRE(near(a.momentum, {}));
  SLRR_REQUIRE(near(b.momentum, {}));
}

SLRR_TEST(resting_overlap_is_corrected_without_a_restitution_kick) {
  auto a = car(1000, {1.2, 0, 0});
  auto b = car(2000);
  const Vec3 com = center_of_mass(a, b);
  const auto first = resolve_vehicle_contact(a, b);
  SLRR_REQUIRE(first.has_value());
  SLRR_REQUIRE(near(first->speed, 0) && near(first->normal_impulse, 0));
  SLRR_REQUIRE(near(first->impulse, {}));
  // .6 m initial depth exceeds the per-shared-tick correction cap.
  SLRR_REQUIRE(near(a.position.x, 1.2 + .2 * 2 / 3));
  SLRR_REQUIRE(near(b.position.x, -.2 / 3));
  for (int i = 0; i < 512; ++i) {
    const auto hit = resolve_vehicle_contact(a, b);
    SLRR_REQUIRE(hit.has_value());
    SLRR_REQUIRE(near(hit->speed, 0));
    SLRR_REQUIRE(near(hit->impulse, {}));
    SLRR_REQUIRE(near(a.velocity, {}) && near(b.velocity, {}));
    SLRR_REQUIRE(near(a.momentum, {}) && near(b.momentum, {}));
    SLRR_REQUIRE(near(center_of_mass(a, b), com));
  }
  SLRR_REQUIRE(near(1.8 - (a.position.x - b.position.x), .002));
}

SLRR_TEST(slow_and_separating_overlaps_do_not_bounce) {
  auto a = car(1000, {0, 0, 3.8}, {0, 0, -.4});
  auto b = car(2000);
  const auto hit = resolve_vehicle_contact(a, b);
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(near((a.velocity - b.velocity).dot(hit->normal), 0));
  a.velocity = {0, 0, 2};
  b.velocity = {};
  const auto old_a = a, old_b = b;
  const auto separating = resolve_vehicle_contact(a, b);
  SLRR_REQUIRE(separating.has_value());
  SLRR_REQUIRE(near(separating->speed, 0));
  SLRR_REQUIRE(near(separating->impulse, {}));
  SLRR_REQUIRE(near(a.velocity, old_a.velocity) && near(b.velocity, old_b.velocity));
  SLRR_REQUIRE(a.position.z > old_a.position.z);
}

SLRR_TEST(high_relative_speed_sweep_stops_endpoint_pass_through) {
  auto a = car(1000, {5, 0, 5}, {1200, 0, -1200});
  auto b = car(2000, {5, 0, -5}, {1200, 0, 1200});
  const auto previous_a = a, previous_b = b;
  a.position = {15, 0, -5};
  b.position = {15, 0, 5};
  const Vec3 total = linear_momentum(a, b), com = center_of_mass(a, b);
  auto discrete_a = a, discrete_b = b;
  SLRR_REQUIRE(!resolve_vehicle_contact(discrete_a, discrete_b));
  const auto hit = resolve_swept_vehicle_contact(a, b, previous_a, previous_b);
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(near(hit->normal, {0, 0, 1}));
  SLRR_REQUIRE(near(hit->speed, 2400));
  SLRR_REQUIRE(a.position.z > b.position.z);
  SLRR_REQUIRE(a.position.z - b.position.z >= 3.999);
  SLRR_REQUIRE(near(center_of_mass(a, b), com));
  SLRR_REQUIRE(near(a.position.x, 15) && near(b.position.x, 15));
  SLRR_REQUIRE(near(a.velocity.x, 1200) && near(b.velocity.x, 1200));
  SLRR_REQUIRE(near(linear_momentum(a, b), total, 1e-6));
  SLRR_REQUIRE(near(a.momentum, {}) && near(b.momentum, {}));
}

SLRR_TEST(sweep_uses_first_contact_not_the_wrong_side_of_an_end_overlap) {
  auto a = car(1000, {0, 0, 5}, {0, 0, -720});
  auto b = car(2000);
  const auto previous_a = a, previous_b = b;
  a.position.z = -1;
  const auto hit = resolve_swept_vehicle_contact(a, b, previous_a, previous_b);
  SLRR_REQUIRE(hit.has_value());
  SLRR_REQUIRE(near(hit->normal, {0, 0, 1}));
  SLRR_REQUIRE(hit->normal_impulse > 500000);
  SLRR_REQUIRE(a.position.z > b.position.z);
}

SLRR_TEST(sweep_is_argument_symmetric_and_preserves_current_orientation) {
  auto a = car(1100, {0, 0, 5}, {0, 0, -800}, .05);
  auto b = car(1700, {0, 0, -5}, {0, 0, 800}, -.08);
  const auto previous_a = a, previous_b = b;
  a.position.z = -3;
  b.position.z = 3;
  auto copy_b = b, copy_a = a;
  const auto hit = resolve_swept_vehicle_contact(a, b, previous_a, previous_b);
  const auto reverse = resolve_swept_vehicle_contact(copy_b, copy_a, previous_b, previous_a);
  SLRR_REQUIRE(hit && reverse);
  same_state(a, copy_a);
  same_state(b, copy_b);
  SLRR_REQUIRE(near(a.orientation.y, previous_a.orientation.y));
  SLRR_REQUIRE(near(b.orientation.y, previous_b.orientation.y));
  SLRR_REQUIRE(near(hit->point, reverse->point));
  SLRR_REQUIRE(near(hit->impulse, -reverse->impulse, 1e-6));
}

SLRR_TEST(sweep_leaves_departing_overlaps_and_overpasses_alone) {
  auto a = car(1000, {0, 0, 3.8}, {0, 0, 400});
  auto b = car(2000);
  const auto previous_a = a, previous_b = b;
  a.position.z = 8;
  const auto old_a = a, old_b = b;
  SLRR_REQUIRE(!resolve_swept_vehicle_contact(a, b, previous_a, previous_b));
  same_state(a, old_a);
  same_state(b, old_b);

  auto above = car(1000, {0, 2, 10}, {0, 0, -2400});
  const auto old_above = above;
  above.position.z = -10;
  const auto end_above = above;
  SLRR_REQUIRE(!resolve_swept_vehicle_contact(above, b, old_above, old_b));
  same_state(above, end_above);
  same_state(b, old_b);
}

SLRR_TEST(invalid_body_inputs_throw_without_mutating_the_other_owner) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  using Damage = void (*)(RigidBody&);
  const std::array<Damage, 10> damage = {
      +[](RigidBody& a) { a.mass = 0; },
      +[](RigidBody& a) { a.mass = -1; },
      +[](RigidBody& a) { a.mass = std::numeric_limits<double>::infinity(); },
      +[](RigidBody& a) { a.maximum.x = a.minimum.x; },
      +[](RigidBody& a) { a.minimum.y = std::numeric_limits<double>::quiet_NaN(); },
      +[](RigidBody& a) { a.center.z = std::numeric_limits<double>::infinity(); },
      +[](RigidBody& a) { a.position.x = std::numeric_limits<double>::quiet_NaN(); },
      +[](RigidBody& a) { a.velocity.z = std::numeric_limits<double>::quiet_NaN(); },
      +[](RigidBody& a) { a.momentum.y = std::numeric_limits<double>::infinity(); },
      +[](RigidBody& a) { a.orientation.w = 2; }};
  for (Damage corrupt : damage) {
    auto a = car(1000, {100, 0, 0}, {1, 2, 3});
    auto b = car(2000, {}, {4, 5, 6});
    const auto old_b = b;
    corrupt(a);
    bool threw = false;
    try { resolve_vehicle_contact(a, b); }
    catch (const std::invalid_argument&) { threw = true; }
    SLRR_REQUIRE(threw);
    same_state(b, old_b);
  }
  auto a = car(), b = car();
  const auto old_a = a;
  bool alias_threw = false;
  try { resolve_vehicle_contact(a, a); }
  catch (const std::invalid_argument&) { alias_threw = true; }
  SLRR_REQUIRE(alias_threw);
  same_state(a, old_a);
  auto invalid_previous = a;
  invalid_previous.orientation = {nan, 0, 0, 1};
  bool previous_threw = false;
  try { resolve_swept_vehicle_contact(a, b, invalid_previous, b); }
  catch (const std::invalid_argument&) { previous_threw = true; }
  SLRR_REQUIRE(previous_threw);
  same_state(a, old_a);
}

SLRR_TEST(public_validation_is_nonmutating_and_identifies_bad_participants) {
  auto valid = car(1200, {1, 2, 3}, {4, 5, 6}, .3);
  valid.set_omega({.1, .2, .3});
  const auto before = valid;
  validate_vehicle_contact_body(valid);
  same_state(valid, before);
  auto invalid = valid;
  invalid.orientation.w = 2;
  bool threw = false;
  try { validate_vehicle_contact_body(invalid); }
  catch (const std::invalid_argument&) { threw = true; }
  SLRR_REQUIRE(threw);
  SLRR_REQUIRE(invalid.orientation.w == 2);
  same_state(valid, before);
  RigidBody unconfigured;
  threw = false;
  try { validate_vehicle_contact_body(unconfigured); }
  catch (const std::invalid_argument&) { threw = true; }
  SLRR_REQUIRE(threw);
}

SLRR_TEST(nonfinite_contact_arithmetic_is_transactional) {
  auto a = car(1000, {0, 0, 3.8}, {0, 0, -1e308});
  auto b = car(2000, {}, {0, 0, 1e308});
  const auto old_a = a, old_b = b;
  bool threw = false;
  try { resolve_vehicle_contact(a, b); }
  catch (const std::runtime_error&) { threw = true; }
  SLRR_REQUIRE(threw);
  same_state(a, old_a);
  same_state(b, old_b);
}

int main() {
  const slrr::standalone::Test tests[] = {
      general_box_pair_query_reuses_validated_sat_without_an_index,
      unequal_masses_exchange_the_actual_normal_impulse_once,
      side_contact_handles_a_perpendicular_body,
      offcentre_impulses_conserve_linear_and_angular_exchange,
      angular_point_velocity_contributes_to_approach_speed,
      separated_same_height_and_overpass_pairs_do_not_interact,
      argument_and_creation_order_do_not_change_response,
      equal_depth_rotated_sat_ties_are_argument_symmetric,
      bounds_offset_and_local_com_do_not_move_the_collision_hull,
      resting_overlap_is_corrected_without_a_restitution_kick,
      slow_and_separating_overlaps_do_not_bounce,
      high_relative_speed_sweep_stops_endpoint_pass_through,
      sweep_uses_first_contact_not_the_wrong_side_of_an_end_overlap,
      sweep_is_argument_symmetric_and_preserves_current_orientation,
      sweep_leaves_departing_overlaps_and_overpasses_alone,
      invalid_body_inputs_throw_without_mutating_the_other_owner,
      public_validation_is_nonmutating_and_identifies_bad_participants,
      nonfinite_contact_arithmetic_is_transactional,
  };
  return slrr::standalone::run("vehicle_contacts", tests);
}
