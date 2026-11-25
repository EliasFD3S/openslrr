// Standalone checks for the tyre model and the car solver on flat ground.
// Build by hand like the other standalone tests, from the repository root:
//   g++ -std=c++20 -O2 -Iinclude -Itests tests/test_vehicle_dynamics.cpp
//       src/physics/vehicle_dynamics.cpp src/physics/tire_model.cpp
//       src/physics/suspension_geometry.cpp -o test_vehicle_dynamics
#include "standalone_test.hpp"

#include "slrr/brake_balance.hpp"
#include "slrr/steering_feedback.hpp"
#include "slrr/tire_model.hpp"
#include "slrr/vehicle_dynamics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

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

const Terrain flat_ground = [](double, double, double, GroundHit& hit) {
  hit = {};
  return true;
};

// Extended, contacting massless suspensions with no current spring load. The
// chassis and bump stops stay clear of the floor, isolating the added couple
// from tyre/rest friction and from the existing spring/support solvers.
VehicleDynamics anti_roll_fixture() {
  VehicleDynamics car;
  car.body.configure(1200, {-.85, .4, -1.5}, {.85, 1.4, 1.5}, {0, .75, 0},
                     {1000, 1500, 1800});
  car.body.position = {0, .75, 0};
  for (size_t i = 0; i < car.wheels.size(); ++i) {
    auto& w = car.wheels[i];
    w.enabled = true;
    w.center = {i % 2 ? .75 : -.75, .4, i < 2 ? -1.0 : 1.0};
    w.radius = .3;
    w.anchor = .3;
    w.rest = .25;
    w.minimum = .1;
    w.maximum = .8;
    w.spring = 10000;
  }
  return car;
}

void set_roll(VehicleDynamics& car, double angle) {
  car.body.orientation = {0, 0, std::sin(angle * .5), std::cos(angle * .5)};
}

void one_step(VehicleDynamics& car, const Terrain& terrain = flat_ground) {
  SLRR_REQUIRE(car.advance(VehicleDynamics::timestep, terrain) == 1);
}

Vec3 anti_roll_hardpoint(const VehicleDynamics& car, size_t corner) {
  const auto& w = car.wheels[corner];
  const Vec3 local = w.center - w.down.normalized() * w.anchor_length();
  return car.body.position + car.body.orientation.rotate(local - car.body.center);
}

Vec3 couple_momentum(const VehicleDynamics& before, size_t axle, double force) {
  const Vec3 span = anti_roll_hardpoint(before, 2 * axle) -
                    anti_roll_hardpoint(before, 2 * axle + 1);
  return span.cross({0, force, 0}) *
         (VehicleDynamics::timestep * std::exp(-.015 * VehicleDynamics::timestep));
}

void require_same_body(const RigidBody& a, const RigidBody& b) {
  SLRR_REQUIRE((a.position - b.position).length() == 0);
  SLRR_REQUIRE((a.velocity - b.velocity).length() == 0);
  SLRR_REQUIRE((a.momentum - b.momentum).length() == 0);
  SLRR_REQUIRE(a.orientation.x == b.orientation.x);
  SLRR_REQUIRE(a.orientation.y == b.orientation.y);
  SLRR_REQUIRE(a.orientation.z == b.orientation.z);
  SLRR_REQUIRE(a.orientation.w == b.orientation.w);
}

}  // namespace

SLRR_TEST(tyre_grip_follows_the_tyre_script) {
  // A wider tyre of the same compound grips more; a softer compound too.
  SLRR_REQUIRE(peak_lateral(tyre(245), 3500) > peak_lateral(tyre(195), 3500) * 1.08);
  SLRR_REQUIRE(peak_lateral(tyre(205, 1.687), 3500) > peak_lateral(tyre(205, 1.495), 3500) * 1.08);
  // Pin a sample of the combined-slip curve (#383): stock tyre, 3500 N,
  // 5.7 degrees of slip angle.
  const auto reference = tire_forces(3500, 1, 20, 20, 2, TireParameters{});
  SLRR_REQUIRE(reference.longitudinal == 0);
  SLRR_REQUIRE(std::abs(reference.lateral + 3886.494694) < 1e-5);
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
  capped.stiffness = 60;  // the clamp
  above_cap.stiffness = 600;
  SLRR_REQUIRE(std::abs(small(capped) - small(above_cap)) < 1e-9);
  // sliction is the slide/grip ratio (#383): a low one loses most of the
  // peak in a full slide, 1 or more keeps it.
  auto low = tyre(205), high = low;
  low.sliction = .4;
  high.sliction = 1.2;
  const auto sliding = [](const TireParameters& t) {
    return std::abs(tire_forces(3000, 1, 20, 20, 20 * std::tan(1.4), t).lateral);
  };
  SLRR_REQUIRE(sliding(low) < sliding(high) * .6);
  SLRR_REQUIRE(sliding(high) > peak_lateral(high, 3000) * .97);
  SLRR_REQUIRE(sliding(low) < peak_lateral(low, 3000) * .6);
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
    // Coasting, clutch open: launched at 72 km/h, the box would pick first and
    // the engine braking would lock the rear wheels, which with the combined
    // slip tyre (#383) spins the car instead of measuring its grip.
    car.dynamics.drive.engaged = false;
    car.run(2, 0, 0, .1);
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
  // At about 2.4 g of braking the load transfer and the pitch of the dive
  // can lift the rear wheels for an instant at the 1000 Hz solver rate (#380);
  // the 120 Hz solver damped that peak. The support must stay finite and the
  // car must not hop: no wheel stays off for more than three samples.
  int off_run[4] = {}, longest_off = 0;
  for (int i = 0; i < 120; ++i) {
    const auto before = car.dynamics.body.velocity;
    car.run(1. / 60, 0, 1, 0);
    worst_vertical = std::max(worst_vertical, std::abs(car.dynamics.body.velocity.y - before.y) * 60);
    for (int w = 0; w < 4; ++w) {
      const auto& s = car.dynamics.state[w];
      off_run[w] = s.contact ? 0 : off_run[w] + 1;
      longest_off = std::max(longest_off, off_run[w]);
      worst_lift = std::max(worst_lift, std::abs(s.jacking));
    }
  }
  SLRR_REQUIRE(longest_off <= 3);
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
  // wheels spin far faster than the car moves. Use an explicit 470 Nm net
  // engine here: the default 235 Nm curve no longer has a hidden x2 gain.
  Car car;
  car.dynamics.drive.torque = [](double rpm) {
    return std::clamp(300 + rpm * .04, 0., 470.);
  };
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

SLRR_TEST(sliding_backwards_in_a_forward_gear_keeps_the_wheels_rolling) {
  // A spun car travels backwards in first gear: nothing may brake the
  // wheels, with or without the accelerator, so the slide stays catchable.
  for (double throttle : {0., 1.}) {
    Car car;
    car.run(1, 0, 0, 0);
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, 15});
    for (auto& wheel : car.dynamics.state) wheel.spin = -15 / .31;
    car.dynamics.gear = 1;
    car.run(.5, throttle, 0, 0);
    SLRR_REQUIRE(car.dynamics.gear == 1);
    for (int i = 0; i < 2; ++i)  // the undriven front wheels roll freely
      SLRR_REQUIRE(car.dynamics.state[i].spin * .31 < -8);
    if (throttle == 0)
      for (int i = 2; i < 4; ++i) SLRR_REQUIRE(car.dynamics.state[i].spin * .31 < -8);
    else  // the drive turns the rear wheels forwards against the motion
      for (int i = 2; i < 4; ++i)
        SLRR_REQUIRE(car.dynamics.state[i].spin > car.dynamics.state[0].spin + 5);
  }
}

SLRR_TEST(reverse_while_rolling_forwards_drives_the_wheels_backwards) {
  Car car;
  car.run(1, 0, 0, 0);
  car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -8});
  for (auto& wheel : car.dynamics.state) wheel.spin = 8 / .31;
  car.dynamics.drive.reverse = true;
  // The auto clutch takes up once the engine reaches its launch rpm, then
  // the drive turns the rear wheels backwards; the brakes stay off.
  car.run(.5, 1, 0, 0);
  SLRR_REQUIRE(car.dynamics.gear == 7);
  SLRR_REQUIRE(car.dynamics.state[0].spin * .31 > 4);  // fronts unbraked
  for (int i = 2; i < 4; ++i)
    SLRR_REQUIRE(car.dynamics.state[i].spin < car.dynamics.state[0].spin - 5);
  car.run(4, 1, 0, 0);
  SLRR_REQUIRE(car.dynamics.body.velocity.dot(
                   car.dynamics.body.orientation.rotate({0, 0, -1})) < -1);
}

SLRR_TEST(nitrous_blends_the_wet_bank_without_changing_dry_drive) {
  Car dry, wet, unavailable;
  for (Car* car : {&dry, &wet, &unavailable}) {
    car->run(1, 0, 0, 0);
    car->dynamics.drive.nitro_torque = [](double) { return 350.; };
  }
  wet.dynamics.drive.nitro = 1;
  unavailable.dynamics.drive.nitro = 1;
  unavailable.dynamics.drive.nitro_torque = {};
  dry.run(4, 1, 0, 0);
  wet.run(4, 1, 0, 0);
  unavailable.run(4, 1, 0, 0);
  std::printf("NITRO matched 4s drive dry=%.3f wet=%.3f unavailable=%.3f m/s\n",
              dry.speed(), wet.speed(), unavailable.speed());
  SLRR_REQUIRE(wet.speed() > dry.speed());
  SLRR_REQUIRE(unavailable.speed() == dry.speed());
  // Original injection is not subject to the accelerator's visual threshold.
  Car released, injected;
  for (Car* car : {&released, &injected}) {
    car->dynamics.engine_omega = 2500 / 9.54929658551372;
    car->dynamics.drive.clutch = 0;
    car->dynamics.drive.nitro_torque = [](double) { return 350.; };
  }
  injected.dynamics.drive.nitro = 1;
  released.run(.1, 0, 0, 0);
  injected.run(.1, 0, 0, 0);
  SLRR_REQUIRE(injected.dynamics.engine_omega > released.dynamics.engine_omega);
}

namespace {
// The Car test body (1.7 × 1.25 × 4.4 m bounds), alone in the air.
VehicleDynamics aero_body(Vec3 velocity) {
  VehicleDynamics car;
  car.body.configure(1300, {-.85, .15, -2.2}, {.85, 1.4, 2.2}, {0, .45, .1},
                     {550, 2300, 2200});
  Matrix frame{};
  frame[0] = frame[5] = frame[10] = frame[15] = 1;
  car.body.set_frame(frame);
  car.body.velocity = velocity;
  car.aero.body = true;
  return car;
}
constexpr double half_rho = .5 * 1.225;
}  // namespace

SLRR_TEST(body_drag_and_lift_follow_half_rho_v_squared) {
  const double frontal = .85 * 1.7 * 1.25;
  for (double speed : {20.0, 40.0}) {
    const auto car = aero_body({0, 0, -speed});  // forward is -Z
    const auto load = aero_load(car.aero, car.body, VehicleDynamics::timestep);
    SLRR_REQUIRE(std::abs(load.force.z - half_rho * .32 * frontal * speed * speed) < 1e-6);
    SLRR_REQUIRE(std::abs(load.force.y - half_rho * .25 * frontal * speed * speed) < 1e-6);
    SLRR_REQUIRE(std::abs(load.force.x) < 1e-9);
  }
  // Without the body flag (native traffic) only C_drag remains.
  auto bare = aero_body({0, 0, -30});
  bare.aero.body = false;
  SLRR_REQUIRE(aero_load(bare.aero, bare.body, VehicleDynamics::timestep).force.length() == 0);
}

SLRR_TEST(sliding_sideways_meets_the_side_area_without_lift) {
  const auto ahead = aero_body({0, 0, -30}), sideways = aero_body({30, 0, 0});
  const auto front = aero_load(ahead.aero, ahead.body, VehicleDynamics::timestep);
  const auto side = aero_load(sideways.aero, sideways.body, VehicleDynamics::timestep);
  SLRR_REQUIRE(std::abs(side.force.x + half_rho * 1.0 * .85 * 4.4 * 1.25 * 900) < 1e-6);
  SLRR_REQUIRE(std::abs(side.force.x) > 5 * std::abs(front.force.z));
  SLRR_REQUIRE(std::abs(side.force.y) < 1e-9);
}

SLRR_TEST(a_yawed_car_gets_a_side_force_ahead_without_extra_axial_drag) {
  // 30 m/s along the car with 5 m/s across: 9.5 degrees of yaw.
  const auto car = aero_body({-5, 0, -30});
  const auto load = aero_load(car.aero, car.body, VehicleDynamics::timestep);
  const double frontal = .85 * 1.7 * 1.25, side = .85 * 4.4 * 1.25;
  const double slope = 3.14159265358979 * 1.25 / 4.4;  // πH/L
  // Frontal drag from the flow along the car only.
  SLRR_REQUIRE(std::abs(load.force.z - half_rho * .32 * frontal * 900) < 1e-6);
  // Slender-body side force plus crossflow drag on the side area.
  SLRR_REQUIRE(std::abs(load.force.x - half_rho * side * (slope * 150 + 25)) < 1e-6);
  // About 0.04 per degree of yaw on the frontal area, as road cars measure.
  const double per_degree = (load.force.x / (half_rho * 925) - side * 25 / 925) / frontal /
                            (std::atan(5. / 30) * 180 / 3.14159265358979);
  std::printf("AERO side force slope %.4f per degree\n", per_degree);
  SLRR_REQUIRE(per_degree > .03 && per_degree < .05);
  // The quarter-chord side force acts ahead of the centre of mass and turns
  // the nose downwind (towards +X here), as a road car's does.
  SLRR_REQUIRE(load.torque.y < 0);
}

SLRR_TEST(a_wing_face_meeting_the_air_makes_downforce_until_it_stalls) {
  const Vec3 tilted = Vec3{0, .94, -.34}.normalized();  // up, towards the front
  const auto wing_force = [&](Vec3 normal, double speed) {
    auto car = aero_body({0, 0, -speed});
    const auto body = aero_load(car.aero, car.body, VehicleDynamics::timestep);
    car.aero.wings = {{{0, .45, .1}, normal, .3}};
    const auto both = aero_load(car.aero, car.body, VehicleDynamics::timestep);
    return both.force - body.force;
  };
  // sin α = 0.34: normal coefficient 4.2 · 0.34, along -normal, plus 0.05
  // profile drag along the flow.
  const Vec3 at30 = wing_force(tilted, 30);
  const double normal = half_rho * .3 * 900 * 4.2 * tilted.dot({0, 0, 1}) * -1;
  SLRR_REQUIRE(std::abs(at30.y - (-normal * tilted.y)) < 1e-6);
  SLRR_REQUIRE(std::abs(at30.z - (-normal * tilted.z + half_rho * .3 * 900 * .05)) < 1e-6);
  SLRR_REQUIRE(at30.y < -150 && at30.z > 0);  // downforce and drag
  // It scales with speed squared.
  SLRR_REQUIRE(std::abs(wing_force(tilted, 60).y - 4 * at30.y) < 1e-6);
  // The face turned away from the air carries nothing (the other face of
  // the same element does).
  SLRR_REQUIRE(wing_force(-tilted, 30).length() < 1e-9);
  // A steep face is stalled at a normal coefficient of 1.5.
  const Vec3 steep = Vec3{0, .5, -.866}.normalized();
  SLRR_REQUIRE(std::abs(wing_force(steep, 30).y + half_rho * .3 * 900 * 1.5 * steep.y) < 1e-6);
}

SLRR_TEST(a_rear_wing_moves_load_to_the_rear_axle) {
  const auto loads = [](bool wing) {
    Car car;
    auto& drive = car.dynamics.drive;
    drive.torque = [](double) { return 0.0; };
    drive.gears = 0;
    drive.ratios = {};
    drive.starter = 0;
    car.dynamics.engine_running = false;
    car.dynamics.engine_omega = 0;
    car.dynamics.aero.body = true;
    if (wing)  // behind the rear axle (z = 1.35)
      car.dynamics.aero.wings = {{{0, 1.3, 2.0}, Vec3{0, .94, -.34}.normalized(), .4}};
    car.run(1, 0, 0, 0);
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -40});
    for (auto& wheel : car.dynamics.state) wheel.spin = 40 / .31;
    car.run(1.5, 0, 0, 0);
    const auto& s = car.dynamics.state;
    return std::pair{s[0].force + s[1].force, s[2].force + s[3].force};
  };
  const auto [front_plain, rear_plain] = loads(false);
  const auto [front_wing, rear_wing] = loads(true);
  // About 0.6 kN of downforce at the wing: more than all of it on the rear
  // axle, and the front lifted by the pitch moment.
  SLRR_REQUIRE(rear_wing - rear_plain > 500);
  SLRR_REQUIRE(front_wing < front_plain);
  SLRR_REQUIRE((front_wing + rear_wing) - (front_plain + rear_plain) > 300);
}

SLRR_TEST(chassis_drag_slows_a_coasting_car) {
  const auto coast = [](double drag) {
    Car car;
    auto& drive = car.dynamics.drive;
    drive.torque = [](double) { return 0.0; };
    drive.gears = 0;
    drive.ratios = {};
    drive.starter = 0;
    car.dynamics.engine_running = false;
    car.dynamics.engine_omega = 0;
    car.dynamics.aero.drag = drag;
    car.run(1, 0, 0, 0);
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -40});
    for (auto& wheel : car.dynamics.state) wheel.spin = 40 / .31;
    car.run(2, 0, 0, 0);
    return car.speed();
  };
  // C v^2 / m = 0.4 * 40^2 / 1300 ~ 0.49 m/s^2 at the start of the coast.
  const double lost = coast(0) - coast(.4);
  SLRR_REQUIRE(lost > .75 && lost < 1.05);
}

SLRR_TEST(chassis_drag_never_reverses_the_motion) {
  const Terrain air = [](double, double, double, GroundHit&) { return false; };
  VehicleDynamics light;
  light.body.configure(1, {-1, -1, -1}, {1, 1, 1}, {0, 0, 0}, {1, 1, 1});
  Matrix frame{};
  frame[0] = frame[5] = frame[10] = frame[15] = 1;
  light.body.set_frame(frame);
  light.body.velocity = {2, 0, 0};
  light.aero.drag = 1e6;
  light.advance(VehicleDynamics::timestep, air);
  SLRR_REQUIRE(light.body.velocity.x > -1e-9 && light.body.velocity.x < 1e-6);
}

SLRR_TEST(wind_follows_the_log_profile_with_gusts_it_carries) {
  SLRR_REQUIRE(wind_velocity({}, {12, 0, -40}, 30).length() == 0);  // calm
  const Wind breeze{10, 90};  // towards +X
  // The 10 m report at car height: ln(1/0.03) / ln(10/0.03) of it.
  const double mean = 10 * std::log(1 / .03) / std::log(10 / .03);
  const Vec3 here{3, 1, 7};
  const int samples = 400000;
  double along = 0, across = 0, along2 = 0, across2 = 0, vertical = 0;
  for (int i = 0; i < samples; ++i) {
    const Vec3 air = wind_velocity(breeze, here, i * .25);
    along += air.x;
    across += air.z;
    along2 += air.x * air.x;
    across2 += air.z * air.z;
    vertical = std::max(vertical, std::abs(air.y));
  }
  along /= samples;
  across /= samples;
  const double sigma_along = std::sqrt(along2 / samples - along * along);
  const double sigma_across = std::sqrt(across2 / samples - across * across);
  std::printf("WIND mean %.3f (expected %.3f) across %.4f sigma %.3f/%.3f m/s\n", along,
              mean, across, sigma_along, sigma_across);
  SLRR_REQUIRE(std::abs(along - mean) < .01 * mean);
  SLRR_REQUIRE(std::abs(across) < .01 * mean);
  // Turbulence intensity 1/ln(z/z0) along the wind, three quarters across.
  SLRR_REQUIRE(std::abs(sigma_along - mean / std::log(1 / .03)) < .03 * sigma_along);
  SLRR_REQUIRE(std::abs(sigma_across - .75 * sigma_along) < .03 * sigma_across);
  SLRR_REQUIRE(vertical == 0);
  // Frozen gusts: the air here now is the air 2 s downwind of here later.
  const Vec3 now = wind_velocity(breeze, here, 100);
  const Vec3 later = wind_velocity(breeze, here + Vec3{mean * 2, 0, 0}, 102);
  SLRR_REQUIRE((now - later).length() < 1e-9);
  SLRR_REQUIRE((now - wind_velocity(breeze, here, 101)).length() > 1e-3);
}

SLRR_TEST(wind_changes_the_air_speed_the_car_meets) {
  const double frontal = .85 * 1.7 * 1.25;
  // Forward (-Z) at 30 m/s into a 5 m/s headwind meets 35 m/s of air; with
  // the same wind behind, 25 m/s.
  for (double wind : {5.0, -5.0}) {
    auto car = aero_body({0, 0, -30});
    car.aero.wind = {0, 0, wind};
    const double air = 30 + wind;
    const auto load = aero_load(car.aero, car.body, VehicleDynamics::timestep);
    SLRR_REQUIRE(std::abs(load.force.z - half_rho * .32 * frontal * air * air) < 1e-6);
    SLRR_REQUIRE(std::abs(load.force.y - half_rho * .25 * frontal * air * air) < 1e-6);
  }
  // A crosswind on a standing car meets its side area, without lift. It acts
  // at the centre of the bounds, ahead of the centre of mass, so it turns
  // the nose downwind.
  auto standing = aero_body({});
  standing.aero.wind = {10, 0, 0};
  const auto side = aero_load(standing.aero, standing.body, VehicleDynamics::timestep);
  SLRR_REQUIRE(std::abs(side.force.x - half_rho * 1.0 * .85 * 4.4 * 1.25 * 100) < 1e-6);
  SLRR_REQUIRE(std::abs(side.force.y) < 1e-9);
  SLRR_REQUIRE(side.torque.y < 0);
}

SLRR_TEST(a_braked_car_holds_against_a_strong_crosswind) {
  Car car;
  car.dynamics.aero.body = true;
  car.run(1, 0, 1, 0);
  const Vec3 start = car.dynamics.body.position;
  car.dynamics.aero.wind = {15, 0, 0};  // a gale at car height
  car.run(3, 0, 1, 0);
  const double moved = (car.dynamics.body.position - start).length();
  std::printf("WIND braked car moved %.4f m in a 15 m/s crosswind\n", moved);
  SLRR_REQUIRE(moved < .02);
}

SLRR_TEST(default_anti_roll_is_identical_to_the_unforced_body) {
  auto car = anti_roll_fixture();
  for (const auto& axle : car.anti_roll) {
    SLRR_REQUIRE(axle.stiffness == 0);
    SLRR_REQUIRE(axle.damping == 0);
  }
  set_roll(car, .04);
  car.body.velocity = {3, .2, -12};
  car.body.set_omega({0, 0, .2});
  auto expected = car.body;
  expected.integrate(VehicleDynamics::timestep);
  one_step(car);
  require_same_body(car.body, expected);
  for (const auto& s : car.state) {
    SLRR_REQUIRE(s.contact);
    SLRR_REQUIRE(s.force == 0);
  }
}

SLRR_TEST(anti_roll_has_no_symmetric_heave_force) {
  for (double heave : {-1.0, 0.0, 1.0}) {
    auto car = anti_roll_fixture(), baseline = car;
    car.body.velocity = baseline.body.velocity = {3, heave, -12};
    car.anti_roll = {{{20000, 1500}, {35000, 2200}}};
    one_step(baseline);
    one_step(car);
    require_same_body(car.body, baseline.body);
  }
}

SLRR_TEST(anti_roll_neutral_rest_offsets_do_not_preload_the_chassis) {
  auto car = anti_roll_fixture(), baseline = car;
  for (auto* v : {&car, &baseline}) {
    for (size_t corner : {size_t{1}, size_t{3}}) {
      v->wheels[corner].center.y += .125;
      v->wheels[corner].rest += .125;
    }
    v->body.velocity.y = -.4;
  }
  car.anti_roll = {{{20000, 1500}, {35000, 2200}}};
  one_step(baseline);
  one_step(car);
  SLRR_REQUIRE((car.body.momentum - baseline.body.momentum).length() < 1e-9);
  SLRR_REQUIRE((car.body.velocity - baseline.body.velocity).length() == 0);
}

SLRR_TEST(anti_roll_resists_roll_with_mirrored_equal_opposite_forces) {
  const Terrain different_normals = [](double x, double, double, GroundHit& hit) {
    hit = {};
    hit.normal = {x < 0 ? .1 : -.2, 1, .05};
    return true;
  };
  for (size_t axle : {size_t{0}, size_t{1}}) {
    Vec3 mirror_sum;
    for (double angle : {-.04, .04}) {
      auto car = anti_roll_fixture();
      set_roll(car, angle);
      auto baseline = car;
      const auto before = car;
      car.anti_roll[axle].stiffness = 12000;
      one_step(baseline, different_normals);
      one_step(car, different_normals);
      const size_t left = axle * 2, right = left + 1;
      SLRR_REQUIRE(car.state[left].contact && car.state[right].contact);
      const double force = (car.state[right].length - car.state[left].length) * 12000;
      const Vec3 delta = car.body.momentum - baseline.body.momentum;
      SLRR_REQUIRE((delta - couple_momentum(before, axle, force)).length() < 1e-9);
      SLRR_REQUIRE(delta.z * angle < 0);
      SLRR_REQUIRE(std::abs(delta.x) < 1e-12 && std::abs(delta.y) < 1e-12);
      SLRR_REQUIRE((car.body.velocity - baseline.body.velocity).length() == 0);
      for (size_t i = 0; i < car.state.size(); ++i)
        SLRR_REQUIRE(car.state[i].force == baseline.state[i].force);
      mirror_sum += delta;
    }
    SLRR_REQUIRE(mirror_sum.length() < 1e-9);
  }
}

SLRR_TEST(anti_roll_damping_dissipates_roll_without_net_lift) {
  for (double roll_speed : {-.3, .3}) {
    auto car = anti_roll_fixture();
    car.body.set_omega({0, 0, roll_speed});
    car.body.velocity = {3, -.2, -12};
    auto baseline = car;
    const auto before = car;
    car.anti_roll[0].damping = 2000;
    const double relative_speed = before.body.point_velocity(anti_roll_hardpoint(before, 0)).y -
                                  before.body.point_velocity(anti_roll_hardpoint(before, 1)).y;
    one_step(baseline);
    one_step(car);
    const Vec3 delta = car.body.momentum - baseline.body.momentum;
    SLRR_REQUIRE((delta - couple_momentum(before, 0, -2000 * relative_speed)).length() < 1e-9);
    SLRR_REQUIRE(delta.dot(before.body.omega()) < 0);
    SLRR_REQUIRE(car.body.momentum.length() < baseline.body.momentum.length());
    SLRR_REQUIRE((car.body.velocity - baseline.body.velocity).length() == 0);
  }
}

SLRR_TEST(front_and_rear_anti_roll_configs_are_independent) {
  auto baseline = anti_roll_fixture();
  baseline.wheels[1].center.y += .04;
  baseline.wheels[3].center.y -= .02;
  auto front = baseline, rear = baseline, both = baseline;
  front.anti_roll[0].stiffness = both.anti_roll[0].stiffness = 10000;
  rear.anti_roll[1].stiffness = both.anti_roll[1].stiffness = 30000;
  const auto before = baseline;
  one_step(baseline);
  one_step(front);
  one_step(rear);
  one_step(both);
  const Vec3 front_delta = front.body.momentum - baseline.body.momentum;
  const Vec3 rear_delta = rear.body.momentum - baseline.body.momentum;
  SLRR_REQUIRE(front_delta.z < 0 && rear_delta.z > 0);
  const double front_force = (front.state[1].length - front.state[0].length) * 10000;
  const double rear_force = (rear.state[3].length - rear.state[2].length) * 30000;
  SLRR_REQUIRE((front_delta - couple_momentum(before, 0, front_force)).length() < 1e-9);
  SLRR_REQUIRE((rear_delta - couple_momentum(before, 1, rear_force)).length() < 1e-9);
  SLRR_REQUIRE((both.body.momentum - baseline.body.momentum - front_delta - rear_delta).length() < 1e-9);
  SLRR_REQUIRE((both.body.velocity - baseline.body.velocity).length() == 0);
}

SLRR_TEST(anti_roll_requires_two_enabled_contacting_nonrigid_corners) {
  for (size_t corner : {size_t{0}, size_t{1}}) {
    for (int policy = 0; policy < 3; ++policy) {
      auto baseline = anti_roll_fixture();
      baseline.body.set_omega({0, 0, .2});
      Terrain terrain = flat_ground;
      if (policy == 0) {
        terrain = [corner](double x, double, double, GroundHit& hit) {
          hit = {};
          return corner == 0 ? x > 0 : x < 0;
        };
      } else if (policy == 1) {
        baseline.wheels[corner].enabled = false;
        baseline.state[corner].contact = true; // stale state cannot keep a bar alive
      } else {
        auto& w = baseline.wheels[corner];
        w.rigid_contact = true;
        w.minimum = w.maximum = w.rest = .41;
        w.spring = 0;
      }
      auto car = baseline;
      car.anti_roll[0] = {20000, 2000};
      one_step(baseline, terrain);
      one_step(car, terrain);
      require_same_body(car.body, baseline.body);
      if (policy == 2) {
        SLRR_REQUIRE(car.state[0].contact && car.state[1].contact);
      } else {
        SLRR_REQUIRE(!car.state[corner].contact);
      }
    }
  }
}

SLRR_TEST(invalid_anti_roll_coefficients_individually_act_as_zero) {
  const double invalid[] = {-1, -std::numeric_limits<double>::max(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
  for (double value : invalid) {
    for (int component = 0; component < 3; ++component) {
      auto baseline = anti_roll_fixture();
      set_roll(baseline, .04);
      baseline.body.set_omega({0, 0, .2});
      auto car = baseline;
      if (component == 0) {
        car.anti_roll[0] = {value, value};
      } else if (component == 1) {
        baseline.anti_roll[0] = {0, 2000};
        car.anti_roll[0] = {value, 2000};
      } else {
        baseline.anti_roll[0] = {12000, 0};
        car.anti_roll[0] = {12000, value};
      }
      one_step(baseline);
      one_step(car);
      require_same_body(car.body, baseline.body);
    }
  }
}

SLRR_TEST(extreme_anti_roll_coefficients_stay_finite_at_the_paired_weight_cap) {
  for (double coefficient : {1e12, std::numeric_limits<double>::max()}) {
    for (double roll_speed : {-2.0, 2.0}) {
      auto car = anti_roll_fixture();
      // Displacement=2m and speed=+/-3m/s make both DBL_MAX products
      // overflow; opposing spring/damper terms must not become NaN.
      car.wheels[1].center.y += 2;
      car.wheels[1].maximum = 5;
      car.body.set_omega({0, 0, roll_speed});
      const auto before = car;
      auto baseline = car;
      car.anti_roll[0] = {coefficient, coefficient};
      one_step(baseline);
      one_step(car);
      SLRR_REQUIRE(car.state[0].contact && car.state[1].contact);
      const double force = std::copysign(.5 * car.body.mass * 9.81, roll_speed);
      const Vec3 delta = car.body.momentum - baseline.body.momentum;
      SLRR_REQUIRE((delta - couple_momentum(before, 0, force)).length() < 1e-9);
      SLRR_REQUIRE((car.body.velocity - baseline.body.velocity).length() == 0);
      SLRR_REQUIRE(car.body.position.finite() && car.body.velocity.finite());
      SLRR_REQUIRE(car.body.momentum.finite() && car.body.omega().finite());
    }
  }
}

SLRR_TEST(anti_roll_on_an_engineless_coasting_car_does_not_rewrite_tyre_loads) {
  Car coasting;
  auto& before = coasting.dynamics;
  before.drive.torque = [](double) { return 0.0; };
  before.drive.gears = 0;
  before.drive.ratios = {};
  before.drive.starter = 0;
  before.engine_running = false;
  before.engine_omega = 0;
  set_roll(before, .04);
  before.body.velocity = {2, 0, -12};
  for (auto& s : before.state) s.spin = 12 / .31;
  auto baseline = before, car = before;
  car.anti_roll = {{{20000, 500}, {30000, 800}}};
  one_step(baseline);
  one_step(car);
  SLRR_REQUIRE(!car.engine_running && car.engine_omega == 0 && car.gear == 0);
  SLRR_REQUIRE((car.body.momentum - baseline.body.momentum).length() > 1e-5);
  SLRR_REQUIRE((car.body.velocity - baseline.body.velocity).length() < 1e-12);
  for (size_t i = 0; i < car.state.size(); ++i) {
    const auto& a = car.state[i];
    const auto& b = baseline.state[i];
    SLRR_REQUIRE(a.contact && b.contact && a.force > 0);
    SLRR_REQUIRE(a.force == b.force && a.length == b.length);
    SLRR_REQUIRE(a.tire_force.longitudinal == b.tire_force.longitudinal);
    SLRR_REQUIRE(a.tire_force.lateral == b.tire_force.lateral);
    SLRR_REQUIRE(a.spin == b.spin);
  }
}

// Force feedback (steering_feedback.cpp). One front tyre at slip angle a
// (rad), the solver's sign: lateral speed to the right, force to the left.
SteeringFeedbackWheel front_tyre(double a, double load = 3500) {
  const double t = std::tan(a);
  return {true, load, t, tire_forces(load, 1, 20, 20, 20 * t, tyre(205)).lateral, tyre(205), 0};
}
float settled_torque(double a, double speed = 20, double load = 3500) {
  SteeringFeedback feedback;
  float torque = 0;
  for (int i = 0; i < 120; ++i)
    torque = feedback.update({front_tyre(a, load), front_tyre(a, load)}, speed, 1. / 60).torque;
  return torque;
}

SLRR_TEST(steering_feedback_pulls_back_and_goes_light_past_the_peak) {
  const double peak = SteeringFeedback::peak_slip_angle(3500, tyre(205));
  SLRR_REQUIRE(peak > 6 * std::numbers::pi / 180 && peak < 10 * std::numbers::pi / 180);
  // Heading left of the travel (lateral speed to the right): turn back right.
  SLRR_REQUIRE(settled_torque(.03) < -.2f);
  SLRR_REQUIRE(settled_torque(-.03) > .2f);
  SLRR_REQUIRE(std::abs(settled_torque(.03) + settled_torque(-.03)) < 1e-6f);
  // The aligning torque peaks before the grip, then the wheel goes light.
  const float building = std::abs(settled_torque(.4 * peak));
  const float sliding = std::abs(settled_torque(2 * peak));
  SLRR_REQUIRE(sliding < building * .6f);
  // Its scale is the front tyres' peak torque: never past full range.
  float strongest = 0;
  for (double a = .005; a < .5; a += .005) strongest = std::max(strongest, std::abs(settled_torque(a)));
  SLRR_REQUIRE(strongest > .9f && strongest <= 1.f);
  // A heavier car pulls the same share of the range at the same slip.
  SLRR_REQUIRE(std::abs(settled_torque(.03, 20, 5000) - settled_torque(.03)) < .05f);
}

SLRR_TEST(steering_feedback_fades_when_parked_or_airborne) {
  SLRR_REQUIRE(settled_torque(.05, 0) == 0.f);
  SteeringFeedback feedback;
  const auto parked = feedback.update({front_tyre(0), front_tyre(0)}, 0, 1. / 60);
  const auto moving = feedback.update({front_tyre(0), front_tyre(0)}, 30, 1. / 60);
  SLRR_REQUIRE(parked.damper > moving.damper * 2);
  for (int i = 0; i < 60; ++i) feedback.update({front_tyre(.05), front_tyre(.05)}, 20, 1. / 60);
  SteeringFeedbackOutput out;
  for (int i = 0; i < 30; ++i) out = feedback.update({SteeringFeedbackWheel{}, SteeringFeedbackWheel{}}, 20, 1. / 60);
  SLRR_REQUIRE(std::abs(out.torque) < 1e-3f);
}

SLRR_TEST(steering_feedback_kicks_on_bumps_and_jolts_on_impacts) {
  SteeringFeedback feedback;
  for (int i = 0; i < 30; ++i) feedback.update({front_tyre(0), front_tyre(0)}, 20, 1. / 60);
  // A bump under the left wheel (its suspension shortening) turns it left.
  auto left = front_tyre(0), right = front_tyre(0);
  left.suspension_speed = -.4;
  SLRR_REQUIRE(feedback.update({left, right}, 20, 1. / 60).detail > .1f);
  left.suspension_speed = 0;
  right.suspension_speed = -.4;
  SLRR_REQUIRE(feedback.update({left, right}, 20, 1. / 60).detail < -.1f);
  // Both wheels together (a dip across the road): no kick, some vibration.
  left.suspension_speed = right.suspension_speed = -.4;
  SteeringFeedbackOutput out;
  for (int i = 0; i < 20; ++i) out = feedback.update({left, right}, 20, 1. / 60);
  SLRR_REQUIRE(std::abs(out.detail) < 1e-6f && out.vibration > .05f);
  // An impact jolts the wheel, then dies out within a few tenths.
  left.suspension_speed = right.suspension_speed = 0;
  for (int i = 0; i < 60; ++i) feedback.update({left, right}, 20, 1. / 60);
  out = feedback.update({left, right}, 20, 1. / 60, .8);
  SLRR_REQUIRE(out.detail > .5f);
  for (int i = 0; i < 30; ++i) out = feedback.update({left, right}, 20, 1. / 60);
  SLRR_REQUIRE(std::abs(out.detail) < .01f);
}

SLRR_TEST(steering_feedback_vibrates_when_the_front_tyres_scrub) {
  const double peak = SteeringFeedback::peak_slip_angle(3500, tyre(205));
  SteeringFeedback grip, scrub;
  SteeringFeedbackOutput a, b;
  for (int i = 0; i < 60; ++i) {
    a = grip.update({front_tyre(.5 * peak), front_tyre(.5 * peak)}, 20, 1. / 60);
    b = scrub.update({front_tyre(2 * peak), front_tyre(2 * peak)}, 20, 1. / 60);
  }
  SLRR_REQUIRE(a.vibration < .01f);
  SLRR_REQUIRE(b.vibration > .2f && b.vibration_hz >= 20 && b.vibration_hz <= 30);
}

SLRR_TEST(steering_feedback_centres_a_car_in_a_turn) {
  for (double steer : {.05, -.05}) {
    Car car;
    car.run(1, 0, 0, 0);
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -20});
    for (auto& wheel : car.dynamics.state) wheel.spin = 20 / .31;
    car.dynamics.drive.engaged = false;
    SteeringFeedback feedback;
    float torque = 0;
    for (int i = 0; i < 90; ++i) {
      car.run(1. / 60, 0, 0, steer);
      std::array<SteeringFeedbackWheel, 2> front;
      for (int w = 0; w < 2; ++w) {
        const auto& st = car.dynamics.state[w];
        front[w] = {st.contact, st.force, st.slip_angle, st.tire_force.lateral,
                    car.dynamics.drive.wheels[w].tyre, st.speed};
      }
      torque = feedback.update(front, car.speed(), 1. / 60).torque;
    }
    // Steered left (positive), the tyres turn the wheel back right.
    SLRR_REQUIRE(steer > 0 ? torque < -.1f : torque > .1f);
  }
}

// Brake distribution (brake_balance.hpp): the stock Baiern CoupeSport discs,
// 3718 Nm front and 2928 Nm rear before Brake.java's split.
std::array<double, 4> stock_split(double balance) {
  return {3718 * (1 - balance), 3718 * (1 - balance), 2928 * balance, 2928 * balance};
}
double front_share(const std::array<double, 4>& b) {
  return (b[0] + b[1]) / (b[0] + b[1] + b[2] + b[3]);
}

SLRR_TEST(stock_brake_balance_puts_seventy_percent_on_the_front) {
  auto b = stock_split(.5);
  const double before = b[0] + b[1] + b[2] + b[3];
  SLRR_REQUIRE(std::abs(front_share(b) - .559) < .001);
  slrr::distribute_brakes(b, 2, .5);
  SLRR_REQUIRE(std::abs(front_share(b) - .7) < 1e-9);
  SLRR_REQUIRE(std::abs(b[0] + b[1] + b[2] + b[3] - before) < 1e-6);
  SLRR_REQUIRE(b[0] == b[1] && b[2] == b[3]);
}

SLRR_TEST(the_brake_adjuster_moves_the_front_share) {
  double previous = 2;
  for (double balance : {0.0, .2, .35, .5, .65, .8, 1.0}) {
    auto b = stock_split(balance);
    slrr::distribute_brakes(b, 2, balance);
    const double share = front_share(b);
    SLRR_REQUIRE(share < previous && share >= .4 - 1e-9 && share <= 1 + 1e-9);
    // At an end of the adjuster the empty axle is rebuilt from the other one.
    if (balance > 0 && balance < 1)
      SLRR_REQUIRE(std::abs(b[0] + b[1] + b[2] + b[3] - .5 * 2 * (3718 + 2928)) < 1e-6);
    previous = share;
  }
  // A car with no rear brakes keeps what the scripts gave.
  std::array<double, 4> front_only{1000, 1000, 0, 0};
  slrr::distribute_brakes(front_only, 2, .5);
  SLRR_REQUIRE(front_only[0] == 1000 && front_only[2] == 0);
}

int main() {
  const slrr::standalone::Test tests[] = {
      stock_brake_balance_puts_seventy_percent_on_the_front,
      the_brake_adjuster_moves_the_front_share,
      steering_feedback_pulls_back_and_goes_light_past_the_peak,
      steering_feedback_fades_when_parked_or_airborne,
      steering_feedback_kicks_on_bumps_and_jolts_on_impacts,
      steering_feedback_vibrates_when_the_front_tyres_scrub,
      steering_feedback_centres_a_car_in_a_turn,
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
      sliding_backwards_in_a_forward_gear_keeps_the_wheels_rolling,
      reverse_while_rolling_forwards_drives_the_wheels_backwards,
      nitrous_blends_the_wet_bank_without_changing_dry_drive,
      body_drag_and_lift_follow_half_rho_v_squared,
      sliding_sideways_meets_the_side_area_without_lift,
      a_yawed_car_gets_a_side_force_ahead_without_extra_axial_drag,
      a_wing_face_meeting_the_air_makes_downforce_until_it_stalls,
      a_rear_wing_moves_load_to_the_rear_axle,
      chassis_drag_slows_a_coasting_car,
      chassis_drag_never_reverses_the_motion,
      wind_follows_the_log_profile_with_gusts_it_carries,
      wind_changes_the_air_speed_the_car_meets,
      a_braked_car_holds_against_a_strong_crosswind,
      default_anti_roll_is_identical_to_the_unforced_body,
      anti_roll_has_no_symmetric_heave_force,
      anti_roll_neutral_rest_offsets_do_not_preload_the_chassis,
      anti_roll_resists_roll_with_mirrored_equal_opposite_forces,
      anti_roll_damping_dissipates_roll_without_net_lift,
      front_and_rear_anti_roll_configs_are_independent,
      anti_roll_requires_two_enabled_contacting_nonrigid_corners,
      invalid_anti_roll_coefficients_individually_act_as_zero,
      extreme_anti_roll_coefficients_stay_finite_at_the_paired_weight_cap,
      anti_roll_on_an_engineless_coasting_car_does_not_rewrite_tyre_loads,
  };
  return slrr::standalone::run("vehicle_dynamics", tests);
}
