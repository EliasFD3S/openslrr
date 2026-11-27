// Standalone floor-support checks; build with vehicle_dynamics.cpp,
// tire_model.cpp and suspension_geometry.cpp, -Iinclude -Itests.
#include "standalone_test.hpp"
#include "slrr/vehicle_dynamics.hpp"
#include <algorithm>
#include <cmath>

using namespace slrr::physics;
namespace {
VehicleDynamics car(bool mechanical = false) {
  VehicleDynamics d;
  d.body.configure(1300, {-.85, .15, -2.2}, {.85, 1.4, 2.2}, {0, .45, .1}, {550, 2300, 2200});
  for (size_t i = 0; i < 4; ++i) {
    auto& w = d.wheels[i];
    w.enabled = true;
    w.center = {i % 2 ? .75 : -.75, .31, i < 2 ? -1.3 : 1.35};
    w.radius = .31;
    w.anchor = .39;
    w.rest = .478;
    w.minimum = .19;
    w.maximum = .45;
    w.spring = 25000;
    w.bump_damping = w.rebound_damping = 1500;
    w.unsprung = 25;
    d.drive.wheels[i].brake = 6000;
  }
  if (mechanical) {
    d.drive.torque = [](double) { return 0.; };
    d.drive.inertia = .38;
    d.drive.idle = 800;
    d.drive.limit = 7000;
    d.drive.brake_scale = 1;
  }
  Matrix frame{};
  frame[0] = frame[5] = frame[10] = frame[15] = 1;
  frame[13] = .1f;
  d.body.set_frame(frame);
  return d;
}
Terrain flat() {
  return [](double, double, double, GroundHit& hit) { hit = {}; return true; };
}
void run(VehicleDynamics& d, double seconds, const Terrain& terrain) {
  for (int i = 0; i < int(std::round(seconds * 60)); ++i) d.advance(1. / 60, terrain);
}
void missing_spring(VehicleDynamics& d, size_t corner) {
  auto& w = d.wheels[corner];
  w.rigid_contact = true;
  w.minimum = w.maximum = w.rest = .19;
  w.spring = 0;
}
double horizontal(Vec3 a) { return std::hypot(a.x, a.z); }
}

SLRR_TEST(missing_spring_uses_current_substep_support_and_settles) {
  for (bool mechanical : {false, true}) {
    auto d = car(mechanical);
    // This isolated tyre/bump-stop case has enough underside clearance to
    // reach the compression stop before the chassis-box fallback hits.
    d.body.minimum.y = .35;
    missing_spring(d, 3);
    d.drive.brake = mechanical ? 1 : 0;
    // A deliberately poisoned old load must never reach the current tyre force.
    d.state[3].force = 1e12;
    run(d, 5, flat());
    const auto start = d.body.position;
    run(d, 20, flat());
    SLRR_REQUIRE(d.state[3].contact);
    SLRR_REQUIRE(d.state[3].force > 0 && d.state[3].force < d.body.mass * 9.81);
    SLRR_REQUIRE(horizontal(d.body.position - start) < .01);
    SLRR_REQUIRE(d.body.velocity.length() < .01);
    SLRR_REQUIRE(d.body.orientation.rotate({0, 1, 0}).y > .9);
  }
}

SLRR_TEST(three_wheel_car_stays_on_garage_rails_for_ten_minutes) {
  auto d = car();
  d.wheels[0].radius = .12;
  d.wheels[0].metal_contact = true;
  for (double x : {-.8, .8}) for (double z : {-1.3, 1.3})
    d.body_supports[d.body_support_count++] = {x, .12, z};
  const Terrain rails = [](double x, double z, double, GroundHit& hit) {
    hit = {};
    hit.height = std::abs(x) >= .6 && std::abs(x) <= 1.0 && std::abs(z) < 2.5 ? 0 : -.2;
    return true;
  };
  run(d, 8, rails);
  const auto start = d.body.position;
  run(d, 600, rails);
  SLRR_REQUIRE(horizontal(d.body.position - start) < .01);
  SLRR_REQUIRE(std::abs(d.body.position.y - start.y) < .005);
  SLRR_REQUIRE(d.body.velocity.length() < .001);
}

SLRR_TEST(installed_body_points_support_a_car_without_wheels) {
  auto d = car();
  for (auto& w : d.wheels) w.enabled = false;
  // Actual low body points, below the chassis-box bottom; no phantom rim.
  for (double x : {-.8, .8}) for (double z : {-1.3, 1.3})
    d.body_supports[d.body_support_count++] = {x, .05, z};
  run(d, 5, flat());
  const auto start = d.body.position;
  run(d, 20, flat());
  const auto frame = d.body.frame();
  SLRR_REQUIRE(std::abs(frame[13] + .05) < .01);
  SLRR_REQUIRE(horizontal(d.body.position - start) < .01);
  SLRR_REQUIRE(d.body.velocity.length() < .01);
}

SLRR_TEST(no_ground_means_air_even_with_body_support_points) {
  auto d = car();
  d.body_supports[0] = {0, -.5, 0};
  d.body_support_count = 1;
  const double before = d.body.position.y;
  run(d, .5, [](double, double, double, GroundHit&) { return false; });
  SLRR_REQUIRE(d.body.position.y < before - 1);
  for (const auto& s : d.state) SLRR_REQUIRE(!s.contact && s.force == 0);
}

SLRR_TEST(static_support_does_not_pin_a_car_beyond_its_friction_cone) {
  auto d = car();
  for (auto& w : d.wheels) w.enabled = false;
  for (double x : {-.8, .8}) for (double z : {-1.3, 1.3})
    d.body_supports[d.body_support_count++] = {x, .05, z};
  const Terrain slope = [](double x, double, double, GroundHit& hit) {
    hit.height = x * .8;
    hit.normal = Vec3{-.8, 1, 0}.normalized();
    return true;
  };
  run(d, 3, slope);
  const double before = d.body.position.x;
  run(d, 2, slope);
  SLRR_REQUIRE(d.body.position.x < before - .1);
}

SLRR_TEST(force_pose_split_matches_existing_free_body_integrator) {
  RigidBody a, b;
  a.configure(1000, {-1, -1, -2}, {1, 1, 2}, {});
  b = a;
  a.add_force({100, 20, -30}, {1, 0, 0});
  b.add_force({100, 20, -30}, {1, 0, 0});
  a.integrate(1. / 120);
  b.integrate_forces(1. / 120);
  b.integrate_pose(1. / 120);
  SLRR_REQUIRE((a.position - b.position).length() < 1e-12);
  SLRR_REQUIRE((a.velocity - b.velocity).length() < 1e-12);
  SLRR_REQUIRE((a.omega() - b.omega()).length() < 1e-12);
}

SLRR_TEST(hub_hardware_never_uses_stale_rubber_grip) {
  double force[2]{};
  for (int variation = 0; variation < 2; ++variation) {
    auto d = car(true);
    d.body.position.y -= .11;
    d.body.velocity = {0, 0, -5};
    for (size_t i = 0; i < 4; ++i) {
      auto& w = d.wheels[i];
      w.rigid_contact = w.metal_contact = true;
      w.minimum = w.maximum = w.rest = w.anchor;
      w.spring = 0;
      d.drive.wheels[i].tyre.friction = variation ? 1000 : .001;
      d.drive.wheels[i].tyre.stiffness = variation ? 1000 : .001;
    }
    d.advance(1. / 120, flat());
    const auto& w = d.state[0];
    SLRR_REQUIRE(w.contact && w.force > 0);
    force[variation] = w.tire_force.longitudinal;
    SLRR_REQUIRE(std::hypot(w.tire_force.longitudinal, w.tire_force.lateral) <= .4 * w.force + 1e-9);
  }
  SLRR_REQUIRE(std::abs(force[0] - force[1]) < 1e-9);
}

SLRR_TEST(native_traffic_can_pull_away_after_a_supported_stop) {
  auto d = car();
  d.drive.native_traffic = true;
  d.drive.brake = 1;
  run(d, 3, flat());
  const auto start = d.body.position;
  d.drive.brake = 0;
  d.drive.throttle = 1;
  run(d, 2, flat());
  SLRR_REQUIRE(horizontal(d.body.position - start) > 2);
  SLRR_REQUIRE(d.body.velocity.length() > 2);
}

SLRR_TEST(weak_brakes_cannot_borrow_unlimited_static_tyre_grip) {
  auto d = car(true);
  for (auto& w : d.drive.wheels) w.brake = 1e-6;
  run(d, 3, flat());
  d.body.velocity = d.body.orientation.rotate({0, 0, -.02});
  for (auto& s : d.state) s.spin = .02 / .31;
  d.drive.brake = 1;
  run(d, .5, flat());
  SLRR_REQUIRE(d.body.velocity.length() > .015);
}

SLRR_TEST(body_contacts_stick_on_a_supported_gentle_slope) {
  auto d = car();
  for (auto& w : d.wheels) w.enabled = false;
  for (double x : {-.8, .8}) for (double z : {-1.3, -.4, .4, 1.3})
    d.body_supports[d.body_support_count++] = {x, .05, z};
  const double angle = std::atan(.2);
  d.body.orientation = {0, 0, std::sin(angle / 2), std::cos(angle / 2)};
  const Terrain slope = [](double x, double, double, GroundHit& hit) {
    hit.height = x * .2;
    hit.normal = Vec3{-.2, 1, 0}.normalized();
    return true;
  };
  run(d, 5, slope);
  const auto start = d.body.position;
  run(d, 120, slope);
  SLRR_REQUIRE(horizontal(d.body.position - start) < .01);
  SLRR_REQUIRE(d.body.velocity.length() < .001);
}

SLRR_TEST(compliant_support_scales_with_mass_without_changing_the_pose) {
  Vec3 reference;
  for (double mass : {1300., 100., 10000.}) {
    auto d = car();
    const double ratio = mass / 1300;
    d.body.configure(mass, {-.85, .15, -2.2}, {.85, 1.4, 2.2}, {0, .45, .1},
                     Vec3{550, 2300, 2200} * ratio);
    for (auto& w : d.wheels) w.enabled = false;
    for (double x : {-.8, .8}) for (double z : {-1.3, -.4, .4, 1.3})
      d.body_supports[d.body_support_count++] = {x, .05, z};
    run(d, 10, flat());
    if (mass == 1300) reference = d.body.position;
    else SLRR_REQUIRE((d.body.position - reference).length() < 1e-8);
    SLRR_REQUIRE(d.body.velocity.length() < .001);
  }
}

SLRR_TEST(hard_body_landing_still_reports_a_ground_impact) {
  auto d = car();
  for (auto& w : d.wheels) w.enabled = false;
  for (double x : {-.8, .8}) for (double z : {-1.3, 1.3})
    d.body_supports[d.body_support_count++] = {x, .05, z};
  d.body.position.y += 2;
  d.body.velocity = {0, -12, 0};
  double worst = 0;
  for (int i = 0; i < 60; ++i) {
    d.advance(1. / 60, flat());
    worst = std::max(worst, d.ground_impact.speed);
  }
  SLRR_REQUIRE(worst > 1);
  SLRR_REQUIRE(d.body.position.finite());
}

VehicleDynamics baiern_on_struts() {
  // Geometry captured from the 8ac2ba9 Baiern replay (see below).
  auto d = car(true);
  d.body.configure(1523.64, {-.865, -.577, -2.106}, {.865, .577, 2.106},
                    {.003, -.221, -.046});
  Matrix frame{};
  frame[0] = frame[5] = frame[10] = frame[15] = 1;
  frame[13] = .9f;
  d.body.set_frame(frame);
  for (size_t i = 0; i < 4; ++i) {
    auto& w = d.wheels[i];
    const double side = i % 2 ? 1 : -1;
    w.center = {side * (i < 2 ? .709 : .727), -.590, i < 2 ? -1.480 : 1.256};
    w.radius = .329;
    w.rest = w.maximum = i < 2 ? .265 : .268;
    w.minimum = .178;
    w.anchor = .390;
    w.spring = i < 2 ? 71613 : 73575;
    w.bump_damping = w.rebound_damping = i < 2 ? 3300 : 3000;
    w.unsprung = 10;
    w.arm_length = i < 2 ? .296 : .420;
    w.arm_pivot = {side * (i < 2 ? .415 : .375), i < 2 ? -.560 : -.408,
                   i < 2 ? -1.480 : 1.080};
    w.arm_axis = i < 2 ? Vec3{0, 0, 1} : Vec3{-side * .904, 0, 1.077};
    w.has_hub = true;
    if (i < 2) w.hub = {0, float(side * .077), -.043f, -.004f,
                       float(-side * .071), .407f, 0, float(-side * .071), .407f, 0};
    else {
      w.hub = {-1, float(-side * .121), 0, 0, 0, 0, 0, 0, 0, 0};
      w.down = {side * .026, -1, 0};
      w.hub_alignment = Quaternion{.0001, side * .0087, side * .0131, .9999}.normalized();
    }
  }
  return d;
}

SLRR_TEST(steered_struts_lift_the_car_on_their_kingpins) {
  // #458: steered, a strut's hub swings about its kingpin; the inclination
  // lowers the wheel either way, so the parked car rises on its springs and
  // stays where it is.
  auto d = baiern_on_struts();
  d.drive.brake = 1;
  run(d, 4, flat());
  const auto start = d.body.position;
  for (int i = 0; i < 2; ++i) d.drive.wheels[i].steer = .35;
  run(d, 3, flat());
  const double lift = d.body.position.y - start.y;
  std::printf("KINGPIN lift %.2f mm at 20 deg\n", lift * 1000);
  SLRR_REQUIRE(lift > .0005 && lift < .01);
  SLRR_REQUIRE(horizontal(d.body.position - start) < .01);
  SLRR_REQUIRE(d.body.velocity.length() < .01);
}

SLRR_TEST(baiern_rear_hub_geometry_does_not_glide_with_a_missing_spring) {
  // Geometry captured from the 8ac2ba9 Baiern replay. Use the intended
  // generic box inertia here; the application replay checks assembly moments.
  auto d = car(true);
  d.body.configure(1523.64, {-.865, -.577, -2.106}, {.865, .577, 2.106},
                    {.003, -.221, -.046});
  Matrix frame{};
  frame[0] = frame[5] = frame[10] = frame[15] = 1;
  frame[13] = .9f;
  d.body.set_frame(frame);
  for (size_t i = 0; i < 4; ++i) {
    auto& w = d.wheels[i];
    const double side = i % 2 ? 1 : -1;
    w.center = {side * (i < 2 ? .709 : .727), -.590, i < 2 ? -1.480 : 1.256};
    w.radius = .329;
    w.rest = w.maximum = i < 2 ? .265 : .268;
    w.minimum = .178;
    w.anchor = .390;
    w.spring = i < 2 ? 71613 : 73575;
    w.bump_damping = w.rebound_damping = i < 2 ? 3300 : 3000;
    w.unsprung = 10;
    w.arm_length = i < 2 ? .296 : .420;
    w.arm_pivot = {side * (i < 2 ? .415 : .375), i < 2 ? -.560 : -.408,
                   i < 2 ? -1.480 : 1.080};
    w.arm_axis = i < 2 ? Vec3{0, 0, 1} : Vec3{-side * .904, 0, 1.077};
    w.has_hub = true;
    if (i < 2) w.hub = {0, float(side * .077), -.043f, -.004f,
                       float(-side * .071), .407f, 0, float(-side * .071), .407f, 0};
    else {
      w.hub = {-1, float(-side * .121), 0, 0, 0, 0, 0, 0, 0, 0};
      w.down = {side * .026, -1, 0};
      w.hub_alignment = Quaternion{.0001, side * .0087, side * .0131, .9999}.normalized();
    }
  }
  missing_spring(d, 3);
  d.drive.brake = 1;
  run(d, 5, flat());
  const auto start = d.body.position;
  run(d, 60, flat());
  SLRR_REQUIRE(horizontal(d.body.position - start) < .01);
  SLRR_REQUIRE(d.body.velocity.length() < .01);
  SLRR_REQUIRE(d.body.orientation.rotate({0, 1, 0}).y > .95);
}

SLRR_TEST(zero_longitudinal_grip_is_not_replaced_by_parking_friction) {
  auto d = car(true);
  for (auto& w : d.drive.wheels) w.tyre.longitudinal_grip = 0;
  run(d, 3, flat());
  d.body.velocity = d.body.orientation.rotate({0, 0, -.02});
  d.drive.brake = 1;
  run(d, .5, flat());
  SLRR_REQUIRE(d.body.velocity.length() > .015);
}

SLRR_TEST(low_grip_tyres_cannot_stick_above_their_reference_friction_limit) {
  auto d = car(true);
  for (auto& w : d.drive.wheels) w.tyre.friction = .01;
  d.drive.brake = 1;
  const double angle = std::atan(.5);
  d.body.orientation = {0, 0, std::sin(angle / 2), std::cos(angle / 2)};
  const Terrain slope = [](double x, double, double, GroundHit& hit) {
    hit.height = x * .5;
    hit.normal = Vec3{-.5, 1, 0}.normalized();
    return true;
  };
  run(d, 3, slope);
  const double before = d.body.position.x;
  run(d, 1, slope);
  SLRR_REQUIRE(d.body.position.x < before - .1);
}

SLRR_TEST(static_tyre_limits_keep_longitudinal_and_lateral_script_inputs_separate) {
  TireParameters tyre;
  const auto reference = tire_friction_limits(4000, 1, tyre);
  tyre.longitudinal_grip = 0;
  const auto no_rolling_grip = tire_friction_limits(4000, 1, tyre);
  SLRR_REQUIRE(no_rolling_grip.longitudinal == 0);
  SLRR_REQUIRE(no_rolling_grip.lateral == reference.lateral);
  tyre.longitudinal_grip = 1;
  tyre.friction *= .2;
  const auto low = tire_friction_limits(4000, 1, tyre);
  SLRR_REQUIRE(std::abs(low.longitudinal / reference.longitudinal - .2) < 1e-12);
  SLRR_REQUIRE(std::abs(low.lateral / reference.lateral - .2) < 1e-12);
  SLRR_REQUIRE(tire_friction_limits(4000, 0, tyre).lateral == 0);
}

int main() {
  const slrr::standalone::Test tests[] = {
    missing_spring_uses_current_substep_support_and_settles,
    three_wheel_car_stays_on_garage_rails_for_ten_minutes,
    installed_body_points_support_a_car_without_wheels,
    no_ground_means_air_even_with_body_support_points,
    static_support_does_not_pin_a_car_beyond_its_friction_cone,
    force_pose_split_matches_existing_free_body_integrator,
    hub_hardware_never_uses_stale_rubber_grip,
    native_traffic_can_pull_away_after_a_supported_stop,
    weak_brakes_cannot_borrow_unlimited_static_tyre_grip,
    body_contacts_stick_on_a_supported_gentle_slope,
    compliant_support_scales_with_mass_without_changing_the_pose,
    hard_body_landing_still_reports_a_ground_impact,
    baiern_rear_hub_geometry_does_not_glide_with_a_missing_spring,
    zero_longitudinal_grip_is_not_replaced_by_parking_friction,
    low_grip_tyres_cannot_stick_above_their_reference_friction_limit,
    static_tyre_limits_keep_longitudinal_and_lateral_script_inputs_separate,
    steered_struts_lift_the_car_on_their_kingpins,
  };
  return slrr::standalone::run("missing_support", tests);
}
