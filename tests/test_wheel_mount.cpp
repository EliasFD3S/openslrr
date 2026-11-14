#include "standalone_test.hpp"

#include "slrr/wheel_mount.hpp"

#include <cmath>

using slrr::physics::RigidBody;
using slrr::physics::Vec3;

namespace {

RigidBody car() {
  RigidBody body;
  body.configure(1300.0, {-0.9, -0.6, -2.2}, {0.9, 0.6, 2.2}, {0, 0, 0});
  body.position = {0, 0.5, 0};
  return body;
}

}  // namespace

SLRR_TEST(central_impact_charges_the_whole_mass) {
  const auto body = car();
  // Through the centre of mass the effective mass is the body mass.
  const double energy = slrr::physics::contact_impulse_energy(body, {0, 0.5, -2.2}, {0, 0, 1}, 10.0);
  const double expected = std::pow(1.12 * 10.0 * 1300.0, 2) / (2 * 1300.0);
  SLRR_REQUIRE(std::fabs(energy - expected) < 1e-6 * expected);
  // An offset corner hit turns the car and stops less of it.
  const double corner = slrr::physics::contact_impulse_energy(body, {0.9, 0.5, -2.2}, {0, 0, 1}, 10.0);
  SLRR_REQUIRE(corner < energy && corner > 0.4 * energy);
  SLRR_REQUIRE(slrr::physics::contact_impulse_energy(body, {0, 0.5, -2.2}, {0, 0, 1}, -1.0) == 0);
}

SLRR_TEST(hardpoint_travel_ends_the_mount_at_three_decimetres) {
  SLRR_REQUIRE(std::fabs(slrr::physics::wheel_mount_condition(1.f, .15, 0, 0) - .5f) < 1e-6);
  SLRR_REQUIRE(slrr::physics::wheel_mount_condition(1.f, .3, 0, 0) <= 0.f);
  // Travel is a minimum over time, not cumulative.
  SLRR_REQUIRE(std::fabs(slrr::physics::wheel_mount_condition(.5f, .06, 0, 0) - .5f) < 1e-6);
}

SLRR_TEST(impact_energy_accumulates_with_distance_weight) {
  // 55 kJ at the corner ends the mount; the same energy 0.65 m away costs 1/e.
  SLRR_REQUIRE(slrr::physics::wheel_mount_condition(1.f, 0, 55000, 0) <= 1e-6f);
  const float far = slrr::physics::wheel_mount_condition(1.f, 0, 55000, .65);
  SLRR_REQUIRE(std::fabs(far - float(1 - std::exp(-1.0))) < 1e-5);
  float condition = 1.f;
  for (int i = 0; i < 3; ++i) condition = slrr::physics::wheel_mount_condition(condition, 0, 20000, 0);
  SLRR_REQUIRE(condition < 0.f);
}

SLRR_TEST(moderate_corner_crash_keeps_the_wheel_heavy_one_breaks_it) {
  const auto body = car();
  // A wheel 0.55 m from a front-corner wall contact.
  const Vec3 point{0.85, 0.3, -2.2}, normal{0, 0, 1};
  const double slow = slrr::physics::contact_impulse_energy(body, point, normal, 30 / 3.6);
  const double fast = slrr::physics::contact_impulse_energy(body, point, normal, 80 / 3.6);
  SLRR_REQUIRE(slrr::physics::wheel_mount_condition(1.f, 0, slow, .55) > .5f);
  SLRR_REQUIRE(slrr::physics::wheel_mount_condition(1.f, 0, fast, .55) <= 0.f);
}

SLRR_TEST(pre_contact_velocity_undoes_the_contact_impulse) {
  auto body = car();
  body.velocity = {0, 0, -20};
  body.set_omega({0, 0.3, 0});
  const Vec3 contact{-0.4, 0.3, -2.2}, normal{0, 0, 1}, engine{0, 0.2, -1.4};
  const Vec3 before = body.point_velocity(engine);
  const double closing = -body.point_velocity(contact).dot(normal);
  body.contact(contact, normal, .12, 0);  // no friction: a pure normal impulse
  const Vec3 after = body.point_velocity(engine);
  const Vec3 restored = slrr::physics::pre_contact_velocity(body, contact, normal, closing, engine);
  SLRR_REQUIRE((after - before).length() > 5);
  SLRR_REQUIRE((restored - before).length() < 1e-6);
  // No closing speed, no change.
  const Vec3 same = slrr::physics::pre_contact_velocity(body, contact, normal, 0, engine);
  SLRR_REQUIRE((same - after).length() < 1e-12);
}

SLRR_TEST(engine_mount_needs_a_clearly_bigger_hit_than_a_wheel) {
  const auto& engine = slrr::physics::kEngineMountTuning;
  const auto body = car();
  // A frontal hit 0.77 m from the engine block (a front bumper contact).
  const Vec3 point{-0.24, 0.5, -2.2}, normal{0, 0, 1};
  const double hit74 = slrr::physics::contact_impulse_energy(body, point, normal, 74 / 3.6);
  const double hit110 = slrr::physics::contact_impulse_energy(body, point, normal, 110 / 3.6);
  const float after74 = slrr::physics::wheel_mount_condition(1.f, 0, hit74, .77, engine);
  SLRR_REQUIRE(after74 > 0.f);
  SLRR_REQUIRE(slrr::physics::wheel_mount_condition(1.f, 0, hit110, .77, engine) <= 0.f);
  // The same 74 km/h energy at the wheel distance ends a wheel strut.
  SLRR_REQUIRE(slrr::physics::wheel_mount_condition(1.f, 0, hit74, .5) <= 0.f);
  // Hardpoint travel does not apply to the engine.
  SLRR_REQUIRE(slrr::physics::wheel_mount_condition(1.f, 0, 0, 0, engine) == 1.f);
}

int main() {
  constexpr slrr::standalone::Test tests[] = {
      central_impact_charges_the_whole_mass,
      hardpoint_travel_ends_the_mount_at_three_decimetres,
      impact_energy_accumulates_with_distance_weight,
      moderate_corner_crash_keeps_the_wheel_heavy_one_breaks_it,
      pre_contact_velocity_undoes_the_contact_impulse,
      engine_mount_needs_a_clearly_bigger_hit_than_a_wheel,
  };
  return slrr::standalone::run("wheel_mount", tests);
}
