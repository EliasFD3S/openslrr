// Standalone checks for the tyre model and the car solver on flat ground.
// Build by hand like the other standalone tests, from the repository root:
//   g++ -std=c++20 -O2 -Iinclude -Itests tests/test_vehicle_dynamics.cpp
//       src/physics/vehicle_dynamics.cpp src/physics/tire_model.cpp
//       src/physics/suspension_geometry.cpp -o test_vehicle_dynamics
#include "standalone_test.hpp"

#include "slrr/tire_model.hpp"
#include "slrr/vehicle_dynamics.hpp"

#include <algorithm>
#include <cmath>

namespace {
using namespace slrr::physics;

// Tyre.java for a W-mm tyre of the given compound friction (street 1.495,
// sport 1.559, supersport 1.687): the width scales the script friction.
TireParameters tyre(double width, double compound = 1.559) {
  TireParameters t;
  t.friction = compound * (.5 + .5 * width / 275) * 1.3;
  t.maximum_load = 6500;
  t.load_smoothing = .4;
  return t;
}

double peak_lateral(const TireParameters& t, double load) {
  double best = 0;
  for (double angle = .005; angle < .5; angle += .005)
    best = std::max(best, std::abs(tire_forces(load, 1, 20, 20, 20 * std::tan(angle), t).lateral));
  return best;
}

// A 1300 kg saloon: four sprung wheels, rear drive, Baiern-like geometry.
struct Car {
  VehicleDynamics dynamics;
  explicit Car(bool instant_centers = true, double friction_scale = 1,
               double side_rise = .1, double side_run = 1.5) {
    dynamics.body.configure(1300, {-.85, .15, -2.2}, {.85, 1.4, 2.2}, {0, .45, .1},
                            {550, 2300, 2200});
    const double x[] = {-.75, .75, -.75, .75}, z[] = {-1.3, -1.3, 1.35, 1.35};
    for (int i = 0; i < 4; ++i) {
      auto& w = dynamics.wheels[i];
      w.enabled = true;
      w.center = {x[i], .31, z[i]};
      w.radius = .31;
      w.anchor = .35;
      w.rest = .478;
      w.minimum = .25;
      w.maximum = .45;
      w.spring = 25000;
      w.bump_damping = w.rebound_damping = 1500;
      w.unsprung = 25;
      const double side = i % 2 ? -1 : 1;
      w.front_instant_center = w.side_instant_center = instant_centers;
      w.front_center_offset = {side * 1.446, -.025, 0};
      w.side_center_offset = {0, side_rise, i < 2 ? side_run : -side_run};
      auto& d = dynamics.drive.wheels[i];
      d.weight = i < 2 ? 0 : 1;
      d.brake = 2000;
      d.handbrake = i < 2 ? 0 : 2000;
      d.tyre = tyre(205);
      d.tyre.friction *= friction_scale;
    }
    auto& drive = dynamics.drive;
    drive.gears = 5;
    const double ratios[] = {0, 12.9, 7.9, 5.6, 4.4, 3.6, 0, -12};
    std::copy(std::begin(ratios), std::end(ratios), drive.ratios.begin());
    drive.idle = 800;
    drive.limit = 7000;
    drive.inertia = .38;
    drive.brake_scale = drive.handbrake_scale = 1;
    drive.engaged = drive.automatic_shift = drive.auto_clutch = true;
    drive.torque = [](double rpm) { return std::clamp(150 + rpm * .02, 0., 235.); };
    dynamics.engine_running = true;
    dynamics.engine_omega = 800 / 9.549;
    Matrix frame{};
    frame[0] = frame[5] = frame[10] = frame[15] = 1;
    frame[13] = .05f;
    dynamics.body.set_frame(frame);
  }
  void run(double seconds, double throttle, double brake, double steer) {
    const Terrain flat = [](double, double, double, GroundHit& hit) {
      hit = {};
      return true;
    };
    auto& drive = dynamics.drive;
    drive.throttle = throttle;
    drive.brake = brake;
    for (int i = 0; i < 2; ++i) drive.wheels[i].steer = steer;
    for (double t = 0; t < seconds; t += 1. / 60) dynamics.advance(1. / 60, flat);
  }
  double up() const { return dynamics.body.orientation.rotate({0, 1, 0}).y; }
  double roll() const {
    const auto& o = dynamics.body.orientation;
    return std::atan2(o.rotate({1, 0, 0}).y, o.rotate({0, 1, 0}).y);
  }
  double speed() const { return dynamics.body.velocity.length(); }
};

}  // namespace

SLRR_TEST(tyre_grip_follows_the_tyre_script) {
  // A wider tyre of the same compound grips more; a softer compound too.
  SLRR_REQUIRE(peak_lateral(tyre(245), 3500) > peak_lateral(tyre(195), 3500) * 1.08);
  SLRR_REQUIRE(peak_lateral(tyre(205, 1.687), 3500) > peak_lateral(tyre(205, 1.495), 3500) * 1.08);
  // Pin a reference-curve sample of the intended 8ac2ba9 model, rather
  // than asserting the old script-friction / 1.68 peak interpretation.
  const auto reference = tire_forces(3500, 1, 20, 20, 2, TireParameters{});
  SLRR_REQUIRE(reference.longitudinal == 0);
  SLRR_REQUIRE(std::abs(reference.lateral + 4347.597081) < 1e-5);
}

SLRR_TEST(tyre_stiffness_and_sliction_shape_the_curve) {
  auto soft = tyre(205), stiff = tyre(205);
  soft.stiffness = 10;
  stiff.stiffness = 30;
  const auto small = [](const TireParameters& t) {
    return std::abs(tire_forces(3000, 1, 60, 60, 60 * std::tan(.01), t).lateral);
  };
  SLRR_REQUIRE(small(stiff) > small(soft) * 2.5);
  auto capped = stiff, above_cap = stiff;
  capped.stiffness = TireParameters{}.stiffness * 2.5;
  above_cap.stiffness = capped.stiffness * 10;
  SLRR_REQUIRE(std::abs(small(capped) - small(above_cap)) < 1e-9);
  // At saturated slip reference-scaled sliction multiplies grip, not
  // force divided by the curve's own peak.
  auto low = tyre(205), high = low;
  low.sliction = .4;
  high.sliction = 1.2;
  const auto sliding = [](const TireParameters& t) {
    return std::abs(tire_forces(3000, 1, 20, 20, 50, t).lateral);
  };
  SLRR_REQUIRE(std::abs(sliding(low) / sliding(high) - 1. / 3) < 1e-9);
}

SLRR_TEST(tyre_load_capacity_has_progressive_efficiency) {
  auto t = tyre(205);
  const double low = peak_lateral(t, 2000), high = peak_lateral(t, 13000);
  SLRR_REQUIRE(high > peak_lateral(t, 9000));
  SLRR_REQUIRE(high / 13000 < low / 2000);
  auto linear = t;
  linear.maximum_load = 0;
  SLRR_REQUIRE(std::abs(peak_lateral(linear, 13000) / peak_lateral(linear, 2000) - 6.5) < 1e-9);
  SLRR_REQUIRE(high < peak_lateral(linear, 13000));
}

SLRR_TEST(wheelspin_uses_up_cornering_grip) {
  const auto t = tyre(205);
  const double cornering = std::abs(tire_forces(3000, 1, 20, 20, 20 * std::tan(.08), t).lateral);
  const double spinning = std::abs(tire_forces(3000, 1, 30, 20, 20 * std::tan(.08), t).lateral);
  // The intended reference curve reduces lateral response before the
  // ellipse limit; it does not promise the old curve's 60% ratio.
  SLRR_REQUIRE(spinning < cornering * .8);
}

SLRR_TEST(parked_car_settles_without_vibration) {
  Car car;
  car.run(3, 0, 0, 0);
  double worst = 0;
  for (int i = 0; i < 120; ++i) {
    const auto before = car.dynamics.body.velocity;
    car.run(1. / 60, 0, 0, 0);
    worst = std::max(worst, (car.dynamics.body.velocity - before).length() * 60);
  }
  SLRR_REQUIRE(worst < .05);
  SLRR_REQUIRE(car.speed() < .01);
  for (const auto& wheel : car.dynamics.state) SLRR_REQUIRE(wheel.contact);
}

SLRR_TEST(full_lock_at_speed_slides_instead_of_rolling_over) {
  Car car;
  car.run(1, 0, 0, 0);
  car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -30});
  for (auto& wheel : car.dynamics.state) wheel.spin = 30 / .31;
  car.run(4, .3, 0, .6);
  SLRR_REQUIRE(car.up() > .9);
}

SLRR_TEST(stored_instant_centres_do_not_change_the_intended_solver) {
  double roll[2];
  for (int with = 0; with < 2; ++with) {
    Car car(with != 0);
    car.run(1, 0, 0, 0);
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -15});
    for (auto& wheel : car.dynamics.state) wheel.spin = 15 / .31;
    car.run(3, .35, 0, .12);
    roll[with] = std::abs(car.roll());
  }
  SLRR_REQUIRE(std::abs(roll[1] - roll[0]) < 1e-12);
}

SLRR_TEST(more_grip_corners_harder) {
  double lateral[2];
  for (int i = 0; i < 2; ++i) {
    Car car(true, i ? 1.25 : 1);
    car.run(1, 0, 0, 0);
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -20});
    for (auto& wheel : car.dynamics.state) wheel.spin = 20 / .31;
    car.run(3, .4, 0, .2);
    const auto omega = car.dynamics.body.omega();
    lateral[i] = std::abs(omega.y) * car.speed();
  }
  SLRR_REQUIRE(lateral[1] > lateral[0] * 1.08);
}

SLRR_TEST(high_grip_braking_remains_finite_with_legacy_centre_fields) {
  // Legacy centre fields remain inert even with extreme values. Hard
  // braking with a grippy tyre must leave the support state finite.
  Car car(true, 1.6, .3, .4);
  car.run(1, 0, 0, 0);
  car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -25});
  for (auto& wheel : car.dynamics.state) wheel.spin = 25 / .31;
  double worst_vertical = 0, worst_lift = 0;
  bool always_in_contact = true;
  for (int i = 0; i < 120; ++i) {
    const auto before = car.dynamics.body.velocity;
    car.run(1. / 60, 0, 1, 0);
    worst_vertical = std::max(worst_vertical, std::abs(car.dynamics.body.velocity.y - before.y) * 60);
    for (int w = 0; w < 4; ++w) {
      const auto& s = car.dynamics.state[w];
      always_in_contact = always_in_contact && s.contact;
      worst_lift = std::max(worst_lift, std::abs(s.jacking));
    }
  }
  SLRR_REQUIRE(always_in_contact);
  SLRR_REQUIRE(worst_lift <= 1e-6);
  SLRR_REQUIRE(worst_vertical < 3);
}

SLRR_TEST(a_brake_stronger_than_the_drive_holds_its_wheels) {
  // Full throttle against a full service brake: the brakes beat the engine,
  // so the wheels stay locked and the car does not creep away. Brake torque
  // as the stock Baiern scripts give it (about 2.9 kN m front, 2.3 rear).
  Car car;
  for (int i = 0; i < 4; ++i) car.dynamics.drive.wheels[i].brake = i < 2 ? 2900 : 2300;
  car.run(1, 0, 0, 0);
  car.run(3, 1, 1, 0);
  SLRR_REQUIRE(car.speed() < .3);
  for (int i = 2; i < 4; ++i)
    SLRR_REQUIRE(std::abs(car.dynamics.state[i].spin) * .31 < .5);
}

SLRR_TEST(the_engine_beating_weak_rear_brakes_spins_the_driven_wheels) {
  // A burnout: throttle with only the front brakes holding the car. The rear
  // wheels spin far faster than the car moves.
  Car car;
  for (int i = 0; i < 2; ++i) car.dynamics.drive.wheels[i].brake = 6000;
  for (int i = 2; i < 4; ++i) car.dynamics.drive.wheels[i].brake = 150;
  car.run(1, 0, 0, 0);
  car.run(3, 1, .6, 0);
  const double wheel = car.dynamics.state[2].spin * .31;
  SLRR_REQUIRE(wheel > 5);
  SLRR_REQUIRE(wheel - car.speed() > 5);
}

SLRR_TEST(a_car_without_an_engine_coasts_and_still_brakes) {
  // The drive configure_drive gives a car whose engine was torn off: a
  // zero-torque engine that never starts and no gears.
  const auto engineless = [](Car& car) {
    auto& drive = car.dynamics.drive;
    drive.torque = [](double) { return 0.0; };
    drive.gears = 0;
    drive.ratios = {};
    drive.starter = 0;
    car.dynamics.engine_running = false;
    car.dynamics.engine_omega = 0;
  };
  Car coasting, braking;
  for (Car* car : {&coasting, &braking}) {
    engineless(*car);
    car->run(1, 0, 0, 0);
    car->dynamics.body.velocity = car->dynamics.body.orientation.rotate({0, 0, -20});
    for (auto& wheel : car->dynamics.state) wheel.spin = 20 / .31;
  }
  coasting.run(2, 1, 0, 0);
  braking.run(2, 0, 1, 0);
  SLRR_REQUIRE(coasting.speed() > 18);  // rolls on, no parked-tyre friction
  SLRR_REQUIRE(braking.speed() < 8);    // the brakes still stop it
  SLRR_REQUIRE(!coasting.dynamics.engine_running);
}

SLRR_TEST(preapplied_brakes_hold_against_drive_in_both_directions) {
  for (bool reverse : {false, true}) {
    for (double abs : {0., 1.}) {
      for (double torque : {6000., 1000000.}) {
        Car car;
        car.dynamics.drive.reverse = reverse;
        car.dynamics.drive.abs_strength = abs;
        for (auto& wheel : car.dynamics.drive.wheels) wheel.brake = torque;
        car.run(1, 0, 1, 0);
        const auto start = car.dynamics.body.position;
        car.run(3, 1, 1, 0);
        SLRR_REQUIRE((car.dynamics.body.position - start).length() < .01);
        SLRR_REQUIRE(car.speed() < .005);
        for (const auto& wheel : car.dynamics.state)
          SLRR_REQUIRE(std::abs(wheel.spin) * .31 < .01);
      }
    }
  }
}

SLRR_TEST(abs_releases_a_locked_wheel_while_the_road_moves) {
  Car cars[2];
  for (int i = 0; i < 2; ++i) {
    auto& car = cars[i];
    car.dynamics.drive.torque = [](double) { return 0.; };
    car.dynamics.drive.gears = 0;
    car.dynamics.drive.starter = 0;
    car.dynamics.engine_running = false;
    car.dynamics.drive.abs_strength = i;
    for (auto& wheel : car.dynamics.drive.wheels) wheel.brake = 10000;
    car.run(1, 0, 0, 0);
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -20});
    // Start locked: ABS must let the tyre reaction spin it back up.
    for (auto& wheel : car.dynamics.state) wheel.spin = 0;
    car.run(.3, 0, 1, 0);
  }
  SLRR_REQUIRE(std::abs(cars[0].dynamics.state[0].spin) < 1e-6);
  SLRR_REQUIRE(cars[1].dynamics.state[0].spin * .31 > 5);
  SLRR_REQUIRE(cars[1].speed() < 20);
}

int main() {
  const slrr::standalone::Test tests[] = {
      tyre_grip_follows_the_tyre_script,
      tyre_stiffness_and_sliction_shape_the_curve,
      tyre_load_capacity_has_progressive_efficiency,
      wheelspin_uses_up_cornering_grip,
      parked_car_settles_without_vibration,
      full_lock_at_speed_slides_instead_of_rolling_over,
      stored_instant_centres_do_not_change_the_intended_solver,
      more_grip_corners_harder,
      high_grip_braking_remains_finite_with_legacy_centre_fields,
      a_brake_stronger_than_the_drive_holds_its_wheels,
      the_engine_beating_weak_rear_brakes_spins_the_driven_wheels,
      a_car_without_an_engine_coasts_and_still_brakes,
      preapplied_brakes_hold_against_drive_in_both_directions,
      abs_releases_a_locked_wheel_while_the_road_moves,
  };
  return slrr::standalone::run("vehicle_dynamics", tests);
}
