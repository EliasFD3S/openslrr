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
  explicit Car(bool instant_centers = true, double friction_scale = 1) {
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
      w.side_center_offset = {0, .1, i < 2 ? 1.5 : -1.5};
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
  // The stock sport tyre peaks near the road-tyre mu the units map to.
  const double mu = peak_lateral(tyre(205), 3000) / tire_effective_load(3000, tyre(205));
  SLRR_REQUIRE(mu > .95 && mu < 1.15);
}

SLRR_TEST(tyre_stiffness_and_sliction_shape_the_curve) {
  auto soft = tyre(205), stiff = tyre(205);
  soft.stiffness = 10;
  stiff.stiffness = 30;
  const auto small = [](const TireParameters& t) {
    return std::abs(tire_forces(3000, 1, 20, 20, 20 * std::tan(.01), t).lateral);
  };
  SLRR_REQUIRE(small(stiff) > small(soft) * 2.5);
  // The initial slope is stiffness * load per unit slip.
  const auto base = tyre(205);
  SLRR_REQUIRE(std::abs(small(base) / (tire_effective_load(3000, base) * std::tan(.01)) -
                        base.stiffness) < base.stiffness * .02);
  // A fully sliding tyre keeps about sliction of its peak.
  auto t = tyre(205);
  t.sliction = .7;
  const double slide = std::abs(tire_forces(3000, 1, 20, 20, 20 * std::tan(1.4), t).lateral) /
                       std::abs(std::cos(0.0));
  SLRR_REQUIRE(slide < peak_lateral(t, 3000) * .85);
}

SLRR_TEST(tyre_load_capacity_saturates_grip) {
  const auto t = tyre(205);
  SLRR_REQUIRE(std::abs(tire_effective_load(2000, t) - 2000) < 200);
  SLRR_REQUIRE(tire_effective_load(13000, t) < 13000 * .7);
  SLRR_REQUIRE(tire_effective_load(13000, t) > tire_effective_load(9000, t));
}

SLRR_TEST(wheelspin_uses_up_cornering_grip) {
  const auto t = tyre(205);
  const double cornering = std::abs(tire_forces(3000, 1, 20, 20, 20 * std::tan(.08), t).lateral);
  const double spinning = std::abs(tire_forces(3000, 1, 30, 20, 20 * std::tan(.08), t).lateral);
  SLRR_REQUIRE(spinning < cornering * .6);
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

SLRR_TEST(instant_centres_reduce_body_roll) {
  double roll[2];
  for (int with = 0; with < 2; ++with) {
    Car car(with != 0);
    car.run(1, 0, 0, 0);
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -15});
    for (auto& wheel : car.dynamics.state) wheel.spin = 15 / .31;
    car.run(3, .35, 0, .12);
    roll[with] = std::abs(car.roll());
  }
  SLRR_REQUIRE(roll[1] < roll[0] * .9);
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

int main() {
  const slrr::standalone::Test tests[] = {
      tyre_grip_follows_the_tyre_script,
      tyre_stiffness_and_sliction_shape_the_curve,
      tyre_load_capacity_saturates_grip,
      wheelspin_uses_up_cornering_grip,
      parked_car_settles_without_vibration,
      full_lock_at_speed_slides_instead_of_rolling_over,
      instant_centres_reduce_body_roll,
      more_grip_corners_harder,
  };
  return slrr::standalone::run("vehicle_dynamics", tests);
}
