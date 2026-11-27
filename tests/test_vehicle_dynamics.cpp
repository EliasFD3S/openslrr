// Standalone checks for the tyre model and the car solver on flat ground.
// Build by hand like the other standalone tests, from the repository root:
//   g++ -std=c++20 -O2 -Iinclude -Itests tests/test_vehicle_dynamics.cpp
//       src/physics/vehicle_dynamics.cpp src/physics/tire_model.cpp
//       src/physics/brake_system.cpp src/physics/tyre_thermal.cpp
//       src/physics/suspension_geometry.cpp
//       -o test_vehicle_dynamics
#include "standalone_test.hpp"

#include "slrr/brake_balance.hpp"
#include "slrr/steering_feedback.hpp"
#include "slrr/suspension_geometry.hpp"
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
  // Pin a sample of the Magic Formula lateral curve (#418): stock tyre,
  // 3500 N, 5.7 degrees of slip angle.
  const auto reference = tire_forces(3500, 1, 20, 20, 2, TireParameters{});
  SLRR_REQUIRE(reference.longitudinal == 0);
  SLRR_REQUIRE(std::abs(reference.lateral + 3973.602191) < 1e-5);
}

SLRR_TEST(tyre_stiffness_and_sliction_shape_the_curve) {
  auto soft = tyre(205), stiff = tyre(205);
  soft.stiffness = 10;
  stiff.stiffness = 30;
  const auto small = [](const TireParameters& t) {
    return std::abs(tire_forces(3000, 1, 60, 60, 60 * std::tan(.01), t).lateral);
  };
  // Above the stock 17.68 the slope grows with the square root (#418): 30
  // acts as 23.
  SLRR_REQUIRE(small(stiff) > small(soft) * 2.2);
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
  // Without setMaxLoad the nominal load is 4000 N (#418): the friction still
  // falls with load, as for a tyre rated at 8000 N.
  auto unrated = t, rated = t;
  unrated.maximum_load = 0;
  rated.maximum_load = 8000;
  SLRR_REQUIRE(std::abs(peak_lateral(unrated, 13000) - peak_lateral(rated, 13000)) < 1e-9);
  SLRR_REQUIRE(peak_lateral(unrated, 13000) / peak_lateral(unrated, 2000) < 6.5);
}

SLRR_TEST(wheelspin_uses_up_cornering_grip) {
  const auto t = tyre(205);
  const double cornering = std::abs(tire_forces(3000, 1, 20, 20, 20 * std::tan(.08), t).lateral);
  const double spinning = std::abs(tire_forces(3000, 1, 30, 20, 20 * std::tan(.08), t).lateral);
  // The intended reference curve reduces lateral response before the
  // ellipse limit; it does not promise the old curve's 60% ratio.
  SLRR_REQUIRE(spinning < cornering * .8);
}

SLRR_TEST(camber_leaning_into_the_force_adds_grip) {
  // Lateral speed to +side, force to -side (#418). A tyre whose top leans
  // to -side (negative camber on the outside of the turn) adds camber
  // thrust; leaning the other way takes it off, and either loses peak grip
  // when it leans far.
  const auto t = tyre(205);
  constexpr double degree = std::numbers::pi / 180;
  const auto lateral = [&](double camber) {
    return -tire_forces(3500, 1, 20, 20, 20 * std::tan(4 * degree), t, camber).lateral;
  };
  SLRR_REQUIRE(lateral(-3 * degree) > lateral(0) * 1.02);
  SLRR_REQUIRE(lateral(3 * degree) < lateral(0) * .98);
  // Camber thrust at zero slip angle, towards the side the top leans to.
  SLRR_REQUIRE(tire_forces(3500, 1, 20, 20, 0, t, 3 * degree).lateral > 100);
  SLRR_REQUIRE(tire_forces(3500, 1, 20, 20, 0, t, -3 * degree).lateral < -100);
  // A parked or barely rolling tyre is not pushed sideways by its camber.
  SLRR_REQUIRE(tire_forces(3500, 1, 0, 0, 0, t, 3 * degree).lateral == 0);
  // Far over (a wheel lying down) keeps a quarter of its grip at most.
  const auto limits = tire_friction_limits(3500, 1, t, 1.2);
  SLRR_REQUIRE(limits.lateral <= tire_friction_limits(3500, 1, t).lateral * .25 + 1e-9);
}

SLRR_TEST(aligning_torque_turns_the_tyre_towards_its_travel) {
  // Mz (#458): lateral speed to +side, force to -side, and the moment turns
  // the heading towards +side (negative): it has the lateral force's sign.
  const auto t = tyre(205);
  constexpr double degree = std::numbers::pi / 180;
  const auto at = [&](double angle, double load = 3500, double wheel = 20) {
    return tire_forces(load, 1, wheel, 20, 20 * std::tan(angle * degree), t);
  };
  SLRR_REQUIRE(at(0).aligning == 0);
  SLRR_REQUIRE(at(2).aligning < 0 && at(-2).aligning > 0);
  SLRR_REQUIRE(std::abs(at(2).aligning + at(-2).aligning) < 1e-9);
  // Its lever at small slip: 0.09 R0 at the nominal load (6500 / 2 N).
  const auto small = tire_forces(3250, 1, 20, 20, 20 * std::tan(.2 * degree), t);
  SLRR_REQUIRE(std::abs(small.aligning / small.lateral - .09 * .31) < 1e-3);
  // It peaks before the lateral force does, then the wheel goes light and
  // past the full slide the trail turns negative, less than half as strong.
  const double peak = SteeringFeedback::peak_slip_angle(3500, t) / degree;
  double strongest = 0, strongest_at = 0;
  for (double a = .1; a < 30; a += .1)
    if (-at(a).aligning > strongest) strongest = -at(a).aligning, strongest_at = a;
  SLRR_REQUIRE(strongest_at < .75 * peak && strongest > 40 && strongest < 200);
  SLRR_REQUIRE(-at(peak).aligning < .5 * strongest);
  SLRR_REQUIRE(at(25).aligning > 0 && at(25).aligning < .5 * strongest);
  // More load, a longer patch: more torque at the same slip angle.
  SLRR_REQUIRE(-at(2, 5000).aligning > -at(2).aligning * 1.3);
  // Braking or wheelspin at the same slip angle uses up the patch: lighter.
  SLRR_REQUIRE(-at(2, 3500, 16).aligning < -at(2).aligning * .7);
  SLRR_REQUIRE(-at(2, 3500, 24).aligning < -at(2).aligning * .7);
  // A larger wheel has a longer patch.
  auto big = t;
  big.radius = .36;
  SLRR_REQUIRE(tire_forces(3500, 1, 20, 20, 20 * std::tan(2 * degree), big).aligning <
               at(2).aligning * 1.1);
  // The patch moves with the lateral force, about 2 cm at 4 kN on a tyre
  // of about 200 kN/m, and the longitudinal force acting there turns the
  // wheel: braking towards the turn (lighter), traction away (heavier).
  const auto braking = at(2, 3500, 19.4), driving = at(2, 3500, 20.6);
  SLRR_REQUIRE(at(6).offset < -.01 && at(6).offset > -.03);
  SLRR_REQUIRE(-driving.aligning > -braking.aligning + 10);
  // A softer, longer patch (under-inflated: a longer relaxation length) has
  // a longer trail; the scale is capped.
  auto soft = t;
  soft.relaxation_length = .42;
  const auto soft_small = tire_forces(3250, 1, 20, 20, 20 * std::tan(.2 * degree), soft);
  SLRR_REQUIRE(std::abs(soft_small.aligning / soft_small.lateral - 1.4 * .09 * .31) < 2e-3);
  // Parked: no moment.
  SLRR_REQUIRE(tire_forces(3500, 1, 0, 0, 0, t).aligning == 0);
}

SLRR_TEST(caster_trail_reads_the_strut_axis) {
  // The stock Baiern CoupeSport front left strut (#450): its axis has no
  // side-view inclination, so no caster is authored.
  WheelConfig w;
  w.enabled = true;
  w.center = {-.709, -.590, -1.480};
  w.radius = .31;
  w.rest = w.maximum = .265;
  w.minimum = .178;
  w.anchor = .390;
  w.arm_length = .296;
  w.arm_pivot = w.center + Vec3{.3116, .1365, 0};
  w.arm_axis = {0, 0, 1};
  w.has_hub = true;
  w.hub = {0, -.077f, -.043f, -.004f, .071f, .407f, 0, .1207f, .7043f, 0};
  SLRR_REQUIRE(std::isnan(caster_trail(w, .25)));
  // The strut top moved 5 cm back (+z) for about 5 degrees of caster:
  // the axis meets the ground ahead of the patch.
  w.hub[9] = .05f;
  const double trail = caster_trail(w, .25);
  SLRR_REQUIRE(trail > .01 && trail < .08);
  // Leaning forward, it meets the ground behind.
  w.hub[9] = -.05f;
  SLRR_REQUIRE(caster_trail(w, .25) < 0);
  // A trailing arm (no strut) has none.
  w.hub[0] = -1;
  SLRR_REQUIRE(std::isnan(caster_trail(w, .25)));
}

// The patched Nonus GT II front left strut (#458): 7 degrees of caster.
WheelConfig caster_strut() {
  WheelConfig w;
  w.enabled = true;
  w.center = {-.705, -.596, -1.351};
  w.radius = .33;
  w.rest = w.maximum = .26;
  w.minimum = .12;
  w.anchor = .390;
  w.arm_length = .296;
  w.arm_pivot = w.center + Vec3{.3212, .1945, -.0009};
  w.arm_axis = {0, 0, 1};
  w.has_hub = true;
  w.hub = {0, -.077f, -.043f, -.004f, .071f, .4049f, .0409f, .1572f, .7311f, .0688f};
  return w;
}

SLRR_TEST(a_strut_with_caster_steers_about_its_kingpin) {
  const auto w = caster_strut();
  constexpr double degree = std::numbers::pi / 180;
  bool fitted = false;
  const auto pose = wheel_pose(w, .175, &fitted);
  SLRR_REQUIRE(fitted);
  // The kingpin leans back by the caster and in by about 16 degrees (ball
  // joint to strut top).
  const Vec3 kingpin = pose.hub_rotation.rotate(pose.steer_axis);
  SLRR_REQUIRE(std::abs(std::atan2(kingpin.z, kingpin.y) - 7 * degree) < .2 * degree);
  SLRR_REQUIRE(kingpin.x > .2 && kingpin.x < .4);
  // Its trail: R tan(caster) and the ball joint's offset, a few cm ahead.
  const double trail = caster_trail(w, .175);
  SLRR_REQUIRE(trail > .03 && trail < .06);
  // The static camber (-3 degrees: the top leans inboard, +x) is the fit's.
  // Camber against the wheel's own heading: the tilt of its top towards
  // its inboard side (+x when straight).
  const auto camber = [&](double steer) {
    const auto turn = pose.hub_rotation * wheel_steering(pose.steer_axis, steer);
    const Vec3 up = turn.rotate({0, 1, 0}), forward = turn.rotate({0, 0, -1});
    const Vec3 inboard = Vec3{-forward.z, 0, forward.x}.normalized();
    return -std::asin(up.dot(inboard)) / degree;
  };
  SLRR_REQUIRE(std::abs(camber(0) + 3) < .1);
  // At 20 degrees the caster leans the outer wheel's top into the turn by
  // about 2.4 degrees and stands the inner one up; the kingpin's inclination
  // adds about 1.1 degrees of positive camber to both.
  SLRR_REQUIRE(camber(-20 * degree) < camber(0) - 1);
  SLRR_REQUIRE(camber(20 * degree) > camber(0) + 3);
  // The heading turns by the steering angle.
  const Vec3 forward = (pose.hub_rotation * wheel_steering(pose.steer_axis, 20 * degree)).rotate({0, 0, -1});
  SLRR_REQUIRE(std::abs(std::atan2(-forward.x, -forward.z) - 20 * degree) < .3 * degree);
  // Jacking: steered either way the inclination lifts the car, so the
  // rate's even part centres the wheel; caster's part, odd, lifts one side
  // and lowers the other (it cancels between the two wheels).
  const double left = kingpin_lift_rate(w, .175, 20 * degree);
  const double right = kingpin_lift_rate(w, .175, -20 * degree);
  SLRR_REQUIRE(left - right > .005);
  // At 3500 N and 20 degrees, about 40 N m per wheel against the lock.
  SLRR_REQUIRE(3500 * (left - right) / 2 > 20 && 3500 * (left - right) / 2 < 60);
  SLRR_REQUIRE(std::isnan(kingpin_lift_rate(WheelConfig{}, .2, 0)));
  // The hub itself swings about the kingpin: steered either way, its centre
  // drops (the car rises on its spring) and moves along the arc.
  const auto at = [&](double steer) { return wheel_pose(w, .175, nullptr, steer).hub_origin; };
  const Vec3 straight = at(0), turned_left = at(20 * degree), turned_right = at(-20 * degree);
  SLRR_REQUIRE(straight.y - turned_left.y + straight.y - turned_right.y > .002);
  SLRR_REQUIRE(std::abs(turned_left.z - turned_right.z) > .02);
  SLRR_REQUIRE((wheel_pose(w, .175).hub_origin - straight).length() == 0);
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

// A car sliding sideways comes to rest without swinging back: the relaxed
// slip angle lags the sliding, and bounding the low-speed force by it pushed
// the stopped car the other way, left and right like a spring.
SLRR_TEST(a_sideways_slide_stops_without_swinging_back) {
  Car car;
  car.run(2, 0, 1, 0);
  car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({4, 0, 0});
  double back = 0;
  for (int i = 0; i < 240; ++i) {
    car.run(1. / 60, 0, 1, 0);
    // At the contact patches: the body's roll on its springs is not a slide.
    double patch = 0;
    for (const auto& wheel : car.dynamics.state)
      patch += car.dynamics.body.orientation.inverse_rotate(
                   car.dynamics.body.point_velocity(wheel.patch)).x / 4;
    back = std::min(back, patch);
  }
  std::printf("SLIDE stop: largest swing back %.3f m/s\n", -back);
  SLRR_REQUIRE(back > -.05);
  SLRR_REQUIRE(car.speed() < .05);
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
    // The tyres' lateral force, not yaw rate x speed: after 2 s at 0.1 rad
    // of steer the car slides at 0.2-0.3 rad of slip, where the yaw rate
    // swings with any small moment (the tyres' aligning torque, #458).
    double force = 0;
    for (const auto& wheel : car.dynamics.state) force += wheel.tire_force.lateral;
    lateral[i] = std::abs(force) / car.dynamics.body.mass;
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
    for (auto& wheel : car.dynamics.state) wheel.spin = 20 / .31;
    // Braking from 72 km/h, the wheels suddenly locked (#446: the ABS
    // estimates the car's speed from the wheels it has seen rolling): ABS
    // must let the tyre reaction spin them back up.
    car.run(.05, 0, 1, 0);
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
        SLRR_REQUIRE(car.dynamics.state[i].spin > car.dynamics.state[0].spin + 3);
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
  const auto f = tire_forces(load, 1, 20, 20, 20 * t, tyre(205));
  return {true, load, t, f.lateral, tyre(205), 0, f.aligning};
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
  // A heavier car pulls about the same share of the range at the same slip;
  // the cornering stiffness per newton falls with load (#418).
  SLRR_REQUIRE(std::abs(settled_torque(.03, 20, 5000) - settled_torque(.03)) < .1f);
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
  SteeringFeedbackOutput out;
  left.suspension_speed = -.4;
  for (int i = 0; i < 4; ++i) out = feedback.update({left, right}, 20, 1. / 60);
  SLRR_REQUIRE(out.detail > .1f && out.detail <= SteeringFeedback::kBumpLimit + 1e-6);
  left.suspension_speed = 0;
  right.suspension_speed = -.4;
  for (int i = 0; i < 6; ++i) out = feedback.update({left, right}, 20, 1. / 60);
  SLRR_REQUIRE(out.detail < -.1f);
  // Both wheels together (a dip across the road): no kick, no vibration (a
  // rough road no longer plays a sine, #405).
  left.suspension_speed = right.suspension_speed = -.4;
  for (int i = 0; i < 20; ++i) out = feedback.update({left, right}, 20, 1. / 60);
  SLRR_REQUIRE(std::abs(out.detail) < 1e-3f && out.vibration == 0.f);
  // The solver's step-to-step chatter (small rates flipping every frame)
  // stays below the dead band: no rattle.
  float chatter = 0;
  for (int i = 0; i < 60; ++i) {
    left.suspension_speed = i % 2 ? .08 : -.08;
    right.suspension_speed = -left.suspension_speed;
    chatter = std::max(chatter, std::abs(feedback.update({left, right}, 20, 1. / 60).detail));
  }
  SLRR_REQUIRE(chatter < .03f);
  // An impact knocks the wheel once, then dies out within a few tenths,
  // without changing sign (no ring).
  left.suspension_speed = right.suspension_speed = 0;
  for (int i = 0; i < 60; ++i) feedback.update({left, right}, 20, 1. / 60);
  out = feedback.update({left, right}, 20, 1. / 60, .8);
  SLRR_REQUIRE(out.impact > .4f && out.impact <= SteeringFeedback::kImpactLimit + 1e-6);
  for (int i = 0; i < 30; ++i) {
    out = feedback.update({left, right}, 20, 1. / 60);
    SLRR_REQUIRE(out.impact >= 0.f);
  }
  SLRR_REQUIRE(out.impact < .01f);
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
  // Light and low: a scrub texture, not a rumble motor.
  SLRR_REQUIRE(b.vibration > .1f && b.vibration <= .15f);
  SLRR_REQUIRE(b.vibration_hz >= 18 && b.vibration_hz <= 26);
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
        auto t = car.dynamics.drive.wheels[w].tyre;
        t.radius = car.dynamics.wheels[w].radius;
        front[w] = {st.contact, st.force, st.slip_angle, st.tire_force.lateral, t, st.speed,
                    st.tire_force.aligning, caster_trail(car.dynamics.wheels[w], st.length)};
      }
      torque = feedback.update(front, car.speed(), 1. / 60).torque;
    }
    // Steered left (positive), the tyres turn the wheel back right.
    SLRR_REQUIRE(steer > 0 ? torque < -.1f : torque > .1f);
  }
}

SLRR_TEST(steering_feedback_centres_a_rolling_wheel_from_its_kingpin) {
  // Barely rolling, the tyres' torque has faded; the kingpin's still centres.
  auto left = front_tyre(0), right = front_tyre(0);
  left.jacking = right.jacking = -20;  // steered left: pushes right
  SteeringFeedback feedback;
  float rolling = 0, parked = 0;
  for (int i = 0; i < 30; ++i) rolling = feedback.update({left, right}, 1, 1. / 60).torque;
  SteeringFeedback stopped;
  for (int i = 0; i < 30; ++i) parked = stopped.update({left, right}, 0, 1. / 60).torque;
  SLRR_REQUIRE(rolling < -.02f);
  SLRR_REQUIRE(parked == 0.f);
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

SLRR_TEST(hard_braking_declutches_so_the_engine_never_pushes_the_rear_wheels) {
  // #448: a 100-0 stop with the automatic box and the auto clutch. The box
  // downshifts to first near 44 km/h; with the clutch closed the engine's
  // reflected inertia (0.38 x 12.9^2 kg m^2) pushed the rear wheels and
  // their braking force fell from about 1.8 to 0.5 kN below 37 km/h.
  // Rear brakes too weak to lock the wheels stand in for the ABS, which
  // keeps them rolling: a locked wheel hides the engine's push.
  Car car;
  for (int i = 2; i < 4; ++i) car.dynamics.drive.wheels[i].brake = 700;
  while (car.speed() * 3.6 < 100) car.run(1. / 60, 1, 0, 0);
  double high = 0, low = 0;
  int high_n = 0, low_n = 0;
  while (car.speed() > .5) {
    car.run(1. / 60, 0, 1, 0);
    const double kph = car.speed() * 3.6;
    const double rear = std::abs(car.dynamics.state[2].tire_force.longitudinal) +
                        std::abs(car.dynamics.state[3].tire_force.longitudinal);
    if (kph < 70 && kph > 50) high += rear, ++high_n;
    if (kph < 37 && kph > 10) low += rear, ++low_n;
  }
  SLRR_REQUIRE(high_n > 0 && low_n > 0);
  high /= high_n;
  low /= low_n;
  std::printf("BRAKE rear force 50-70 km/h %.0f N, 10-37 km/h %.0f N, gear %d\n", high, low,
              car.dynamics.gear);
  SLRR_REQUIRE(low > high * .85);
  SLRR_REQUIRE(car.dynamics.gear == 1);  // stopped in first, ready to launch
}

SLRR_TEST(coasting_engine_braking_fades_with_the_speed) {
  // #448: lifting off at 100 km/h. The automatic box downshifted at 2940 rpm
  // back up to 4000-4650 rpm, so the engine braking grew as the car slowed
  // (0.06 g in third, 0.16 g in first at 43 km/h). It now holds its gear and
  // downshifts near idle.
  Car car;
  while (car.speed() * 3.6 < 100) car.run(1. / 60, 1, 0, 0);
  const int lifted_gear = car.dynamics.gear;
  double decel_90 = 0, decel_40 = 0, worst_rpm = 0;
  while (car.speed() * 3.6 > 15) {
    const double before = car.speed();
    car.run(1. / 60, 0, 0, 0);
    const double decel = (before - car.speed()) * 60 / 9.81;
    const double kph = car.speed() * 3.6;
    if (std::abs(kph - 90) < 1) decel_90 = decel;
    if (std::abs(kph - 40) < 1) decel_40 = decel;
    if (car.dynamics.gear < lifted_gear)
      worst_rpm = std::max(worst_rpm, car.dynamics.engine_omega * 9.549);
  }
  std::printf("COAST %.3f g at 90 km/h, %.3f g at 40 km/h, %.0f rpm after downshifts\n",
              decel_90, decel_40, worst_rpm);
  SLRR_REQUIRE(decel_90 > 0 && decel_40 > 0);
  SLRR_REQUIRE(decel_40 < decel_90);
  SLRR_REQUIRE(worst_rpm < 7000 * .42);
}

SLRR_TEST(releasing_a_hard_brake_at_speed_takes_up_without_a_jolt) {
  // #448: the auto clutch opens for a hard stop. Released at 50 km/h, it
  // takes up again with the engine already at the road speed's rpm.
  Car car;
  for (int i = 2; i < 4; ++i) car.dynamics.drive.wheels[i].brake = 700;
  while (car.speed() * 3.6 < 100) car.run(1. / 60, 1, 0, 0);
  while (car.speed() * 3.6 > 50) car.run(1. / 60, 0, 1, 0);
  double worst = 0;
  for (int i = 0; i < 60; ++i) {
    const double before = car.speed();
    car.run(1. / 60, 0, 0, 0);
    if (i >= 18) worst = std::max(worst, (before - car.speed()) * 60 / 9.81);
  }
  std::printf("TAKE-UP worst deceleration %.3f g, gear %d, %.0f rpm\n", worst,
              car.dynamics.gear, car.dynamics.engine_omega * 9.549);
  SLRR_REQUIRE(worst < .08);
}

// Brakes as hardware and the ABS/ASR driving aids (#446).

// A straight stop from `kph` at full pedal; returns the distance and the
// time any wheel spent below 30 % of the car's speed.
struct Stop {
  double distance = 0, locked = 0;
};
Stop full_stop(Car& car, double kph) {
  car.run(1, 0, 0, 0);
  car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -kph / 3.6});
  for (auto& wheel : car.dynamics.state) wheel.spin = kph / 3.6 / .31;
  car.dynamics.gear = 0;
  const Vec3 start = car.dynamics.body.position;
  Stop out;
  for (int tick = 0; tick < 60 * 15 && car.speed() > .1; ++tick) {
    car.run(1. / 60, 0, 1, 0);
    for (const auto& w : car.dynamics.state)
      if (car.speed() > 3 && std::abs(w.spin) * .31 < .3 * car.speed()) out.locked += 1. / 60 / 4;
  }
  const Vec3 d = car.dynamics.body.position - start;
  out.distance = std::hypot(d.x, d.z);
  return out;
}

SLRR_TEST(abs_keeps_the_wheels_turning_and_stops_shorter_than_locked) {
  Stop result[2];
  for (int abs = 0; abs < 2; ++abs) {
    Car car;
    for (auto& wheel : car.dynamics.drive.wheels) wheel.brake = 5000;  // locks at once
    car.dynamics.drive.abs_strength = abs;
    result[abs] = full_stop(car, 100);
  }
  SLRR_REQUIRE(result[0].locked > 1);
  SLRR_REQUIRE(result[1].locked < .05);
  SLRR_REQUIRE(result[1].distance < result[0].distance - 1);
  SLRR_REQUIRE(result[1].distance > 25);  // still bounded by the tyres
}

SLRR_TEST(abs_cycles_one_wheel_holds_the_rear_axle_together_and_delays_the_yaw) {
  AbsModulator abs;
  const std::array<bool, 4> all{true, true, true, true};
  std::array<double, 4> wheels{30, 30, 30, 30}, rates{};
  // Front left and rear left on ice: their wheels drop towards a lock.
  double minimum_left = 1, high_side = 0;
  for (int step = 0; step < 400; ++step) {
    const bool locking = abs.pressure(0) > .3;
    wheels[0] = wheels[2] = locking ? 18 : 29;
    rates[0] = rates[2] = locking ? -60 : 40;
    abs.update(.001, 1, 1, 30, 9, wheels, rates, all, all, 2);
    minimum_left = std::min(minimum_left, abs.pressure(0));
    if (step == 100) high_side = abs.pressure(1);
    SLRR_REQUIRE(abs.pressure(2) == abs.pressure(3));  // select-low rear
    SLRR_REQUIRE(abs.pressure(0) <= 1 && abs.pressure(1) <= 1);
  }
  SLRR_REQUIRE(minimum_left < .3);
  SLRR_REQUIRE(abs.active());
  // #474: each cycle on the icy wheels counts one release (the feedback's
  // knocks); the grippy front never released its wheel.
  SLRR_REQUIRE(abs.releases(0) >= 2 && abs.releases(0) < 20);
  SLRR_REQUIRE(abs.releases(1) == 0);
  // The grippy front wheel may not take the full pedal at once.
  SLRR_REQUIRE(high_side < .9);
  SLRR_REQUIRE(abs.pressure(1) > high_side);
}

SLRR_TEST(abs_lets_the_wheels_lock_below_walking_pace) {
  AbsModulator abs;
  const std::array<bool, 4> all{true, true, true, true};
  abs.update(.001, .8, 1, 1.5, 9, {0, 0, 0, 0}, {-50, -50, -50, -50}, all, all, 2);
  for (size_t i = 0; i < 4; ++i) SLRR_REQUIRE(abs.pressure(i) == .8);
  SLRR_REQUIRE(!abs.active());
}

SLRR_TEST(the_reference_speed_follows_the_accelerometer_while_every_wheel_locks) {
  ReferenceSpeed reference;
  reference.reset(30);
  const std::array<bool, 4> all{true, true, true, true}, none{};
  for (int step = 0; step < 350; ++step)
    reference.update(.001, -9, true, {0, 0, 0, 0}, all, none);
  SLRR_REQUIRE(std::abs(reference.speed() - (30 - 9 * .35)) < .1);
  // Released wheels that never spin back up: the car has stopped. An impact
  // spike on the accelerometer does not hold the estimate up either.
  ReferenceSpeed stopped;
  stopped.reset(2);
  stopped.update(.001, 5000, true, {0, 0, 0, 0}, all, none);
  SLRR_REQUIRE(stopped.speed() < 2.1);
  for (int step = 0; step < 450; ++step)
    stopped.update(.001, 0, true, {0, 0, 0, 0}, all, none);
  SLRR_REQUIRE(stopped.speed() == 0);
  // A wheel spinning back up on ice keeps the estimate.
  ReferenceSpeed ice;
  ice.reset(20);
  for (int step = 0; step < 800; ++step)
    ice.update(.001, -1, true, {step * .015, 0, 0, 0}, all, none);
  SLRR_REQUIRE(ice.speed() > 18);
  // Driving: the undriven wheels give the speed, not the spinning ones.
  ReferenceSpeed driving;
  driving.reset(10);
  const std::array<bool, 4> front{true, true, false, false};
  for (int step = 0; step < 200; ++step)
    driving.update(.001, 3, false, {10.3, 10.3, 16, 16}, all, front);
  SLRR_REQUIRE(std::abs(driving.speed() - 10.3) < .05);
}

SLRR_TEST(asr_limits_wheelspin_in_second_gear_at_speed) {
  double slip[2];
  for (int asr = 0; asr < 2; ++asr) {
    Car car;
    car.dynamics.drive.torque = [](double rpm) { return std::clamp(300 + rpm * .06, 0., 650.); };
    for (auto& wheel : car.dynamics.drive.wheels) wheel.tyre.friction *= .5;  // wet road
    car.dynamics.drive.asr_strength = asr;
    car.run(1, 0, 0, 0);
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -14});
    for (auto& wheel : car.dynamics.state) wheel.spin = 14 / .31;
    car.dynamics.gear = 2;
    car.dynamics.engine_omega = 14 / .31 * car.dynamics.drive.ratios[2];
    slip[asr] = 0;
    for (int tick = 0; tick < 120; ++tick) {
      car.run(1. / 60, 1, 0, 0);
      if (tick > 30)
        slip[asr] = std::max(slip[asr], car.dynamics.state[2].spin * .31 - car.speed());
    }
  }
  SLRR_REQUIRE(slip[0] > 5);
  SLRR_REQUIRE(slip[1] < 2.5);
}

SLRR_TEST(asr_brakes_the_one_spinning_wheel_of_an_open_axle) {
  TractionControl asr;
  const std::array<bool, 4> all{true, true, true, true}, rear{false, false, true, true};
  for (int step = 0; step < 300; ++step)
    asr.update(.001, 1, true, 5, {5, 5, 11, 5.2}, rear, all);
  SLRR_REQUIRE(asr.brake(2) > .05);
  SLRR_REQUIRE(asr.brake(3) < 1e-6 && asr.brake(0) < 1e-6);
  SLRR_REQUIRE(asr.torque_factor() < .9);
  SLRR_REQUIRE(asr.active());
  // The driver lifting or braking hands everything back.
  for (int step = 0; step < 500; ++step)
    asr.update(.001, 1, false, 5, {5, 5, 11, 5.2}, rear, all);
  SLRR_REQUIRE(asr.brake(2) < .01 && asr.torque_factor() > .98);
}

SLRR_TEST(pad_friction_has_a_cold_bite_a_working_window_and_fades) {
  const auto street = brake_hardware(0, 2, .286), race = brake_hardware(2, 2, .286),
             carbon = brake_hardware(0, 3, .286);
  SLRR_REQUIRE(pad_friction_factor(street, 25) > .85 && pad_friction_factor(street, 25) < 1);
  SLRR_REQUIRE(pad_friction_factor(street, 200) == 1);
  SLRR_REQUIRE(pad_friction_factor(street, 600) < .65);
  SLRR_REQUIRE(pad_friction_factor(race, 25) < pad_friction_factor(street, 25));
  SLRR_REQUIRE(pad_friction_factor(race, 600) == 1);
  SLRR_REQUIRE(pad_friction_factor(carbon, 600) > pad_friction_factor(street, 600));
  SLRR_REQUIRE(pad_friction_factor(BrakeHardware{}, 900) == 1);  // stock scripts
  // A vented rotor stores more heat and sheds it faster than a solid one.
  const auto solid = brake_hardware(0, 0, .286);
  SLRR_REQUIRE(rotor_heat_capacity(street) > rotor_heat_capacity(solid));
  SLRR_REQUIRE(rotor_cooling(street, 25, 300) > rotor_cooling(solid, 25, 300));
  SLRR_REQUIRE(rotor_cooling(street, 40, 300) > rotor_cooling(street, 5, 300));
}

SLRR_TEST(a_stop_heats_the_rotors_and_unspecified_brakes_stay_cold) {
  Car hot, cold;
  for (int i = 0; i < 4; ++i) {
    hot.dynamics.drive.wheels[i].brake = i < 2 ? 2400 : 1100;
    hot.dynamics.drive.wheels[i].hardware = brake_hardware(0, i < 2 ? 2 : 0, i < 2 ? .286 : .276);
  }
  hot.dynamics.drive.abs_strength = 1;
  full_stop(hot, 100);
  full_stop(cold, 100);
  // 1300 kg from 100 km/h is 500 kJ: about 40 deg C on each disc (the
  // fronts take two thirds of it, but the solid rears store half as much).
  const double front = hot.dynamics.state[0].brake_temperature;
  SLRR_REQUIRE(front > 50 && front < 150);
  SLRR_REQUIRE(hot.dynamics.state[2].brake_temperature > kAmbientTemperature + 20);
  for (const auto& wheel : cold.dynamics.state)
    SLRR_REQUIRE(wheel.brake_temperature == kAmbientTemperature);
  // Coasting on, the rotor cools.
  hot.run(1, 0, 0, 0);  // the pedal released first
  hot.dynamics.body.velocity = hot.dynamics.body.orientation.rotate({0, 0, -25});
  for (auto& wheel : hot.dynamics.state) wheel.spin = 25 / .31;
  hot.run(20, 0, 0, 0);
  SLRR_REQUIRE(hot.dynamics.state[0].brake_temperature < front - 5);
}

SLRR_TEST(tyres_warm_up_rolling_and_sliding_and_set_the_grip) {
  const auto street = tyre_thermal_spec(0, .31, .205), race = tyre_thermal_spec(2, .31, .205);
  // Ten minutes at 100 km/h on 3500 N: the hysteresis warms the carcass to a
  // road tyre's 40-70 deg C, the tread a little less.
  TyreTemperature cruise;
  for (int step = 0; step < 600000; ++step)
    cruise = step_tyre_temperature(street, cruise, 0, 3500 * .012 * 27.8, kAmbientTemperature,
                                   27.8, .001);
  SLRR_REQUIRE(cruise.carcass > 40 && cruise.carcass < 70);
  SLRR_REQUIRE(cruise.surface > 30 && cruise.surface < cruise.carcass);
  // A burnout (20 kW of sliding) heats the tread within seconds.
  TyreTemperature burnout;
  for (int step = 0; step < 4000; ++step)
    burnout = step_tyre_temperature(street, burnout, 20000, 0, kAmbientTemperature, 0, .001);
  SLRR_REQUIRE(burnout.surface > 60);
  SLRR_REQUIRE(burnout.carcass < burnout.surface);
  // #472: a slide's flash heat leaves the skin within seconds. A 4 s, 40 kW
  // wheelspin on a 275/40R19 sport tyre, then 4 s rolling at 15 m/s: the
  // rubber that sets the grip sheds most of its rise, the bulk keeps some.
  const auto rear = tyre_thermal_spec(1, .351, .275);
  TyreTemperature spin;
  for (int step = 0; step < 4000; ++step)
    spin = step_tyre_temperature(rear, spin, 40000, 0, kAmbientTemperature, 3, .001);
  const double flash = tyre_grip_temperature(spin) - kAmbientTemperature;
  SLRR_REQUIRE(flash > 40);
  for (int step = 0; step < 4000; ++step)
    spin = step_tyre_temperature(rear, spin, 0, 4500 * .012 * 15, kAmbientTemperature, 15, .001);
  SLRR_REQUIRE(tyre_grip_temperature(spin) - kAmbientTemperature < .6 * flash);
  SLRR_REQUIRE(spin.tread > kAmbientTemperature + 10);
  // Grip: cold below the window, full in it, falling past it; a racing
  // compound is weaker cold than a street one.
  SLRR_REQUIRE(tyre_grip_factor(street, 25) < 1 && tyre_grip_factor(street, 25) > .9);
  SLRR_REQUIRE(tyre_grip_factor(street, 70) == 1);
  SLRR_REQUIRE(tyre_grip_factor(street, 180) < .85);
  SLRR_REQUIRE(tyre_grip_factor(race, 25) < tyre_grip_factor(street, 25));
  SLRR_REQUIRE(tyre_grip_factor(race, 100) == 1);
  // Without the patched Tyre: ambient and the scripted grip.
  const TyreThermalSpec none;
  SLRR_REQUIRE(tyre_grip_factor(none, 300) == 1);
  SLRR_REQUIRE(step_tyre_temperature(none, {200, 200}, 1e5, 1e4, 600, 30, 1).surface ==
               kAmbientTemperature);
  // A hot brake warms the carcass through the rim.
  TyreTemperature rim;
  for (int step = 0; step < 60000; ++step)
    rim = step_tyre_temperature(street, rim, 0, 0, 500, 0, .001);
  SLRR_REQUIRE(rim.carcass > kAmbientTemperature + 2);
}

SLRR_TEST(hot_tyres_gain_pressure_and_soften) {
  // #461: 2.0 bar set cold at ambient; the air follows the carcass.
  SLRR_REQUIRE(std::abs(tyre_hot_pressure(2, kAmbientTemperature) - 2) < 1e-9);
  SLRR_REQUIRE(tyre_hot_pressure(2, 80) > 2.5 && tyre_hot_pressure(2, 80) < 2.65);
  const auto sport = tyre_thermal_spec(1, .31, .205), race = tyre_thermal_spec(2, .31, .205);
  // At the working carcass temperature the scripted tyre is unchanged.
  const auto working = tyre_pressure_effect(sport, 2, {tyre_working_tread(sport), tyre_working_carcass(sport), tyre_working_tread(sport)});
  SLRR_REQUIRE(std::abs(working.ratio - 1) < 1e-9 && std::abs(working.stiffness - 1) < 1e-9);
  SLRR_REQUIRE(working.relaxation == 1 && working.rolling == 1 && working.nominal_load == 1);
  // Cold: under its working pressure, so a lazier, softer, draggier tyre.
  const auto cold = tyre_pressure_effect(sport, 2, {});
  SLRR_REQUIRE(cold.ratio < .95 && cold.ratio > .8);
  SLRR_REQUIRE(cold.relaxation > 1 && cold.rolling > 1 && cold.nominal_load < 1);
  // The low pressure softens the carcass; cold rubber is stiffer itself, so
  // a cold tyre's stiffness stays about as scripted.
  SLRR_REQUIRE(tyre_pressure_effect(sport, 2, {tyre_working_tread(sport), 25, tyre_working_tread(sport)}).stiffness < .97);
  SLRR_REQUIRE(std::abs(cold.stiffness - 1) < .05);
  // A cold racing tyre is further from its window than a sport one.
  SLRR_REQUIRE(tyre_pressure_effect(race, 2, {}).ratio < cold.ratio);
  // Hot: over-pressured, quicker; an overheated tread loses stiffness.
  const auto hot = tyre_pressure_effect(sport, 2, {tyre_working_tread(sport), 110, tyre_working_tread(sport)});
  SLRR_REQUIRE(hot.ratio > 1.05 && hot.relaxation < 1 && hot.stiffness > 1);
  const auto cooked = tyre_pressure_effect(sport, 2, {200, 110, 200});
  SLRR_REQUIRE(cooked.stiffness < hot.stiffness * .9);
  // Without the patched Tyre or a known pressure: nothing changes.
  SLRR_REQUIRE(tyre_pressure_effect(TyreThermalSpec{}, 2, {100, 100}).stiffness == 1);
  SLRR_REQUIRE(tyre_pressure_effect(sport, 0, {100, 100}).pressure == 0);
}

SLRR_TEST(the_tread_wears_with_hot_sliding) {
  // #461: each sliding joule counts once up to 100 deg C, about 30 times at
  // 300 (a burnout tears the tread off).
  SLRR_REQUIRE(tyre_wear_weight(80) == 1);
  SLRR_REQUIRE(tyre_wear_weight(300) > 25 && tyre_wear_weight(300) < 35);
  // A ten-second burnout at 50 kW on a hot sport tyre takes about a tenth.
  const double sport_life = .708 * 2755359744.0 * kWearJoulesPerUnit;  // the script's value
  const double burnout = 10 * 50000 * tyre_wear_weight(300) / sport_life;
  SLRR_REQUIRE(burnout > .07 && burnout < .14);
  // Worn: stiffer; down to the cords, less grip.
  SLRR_REQUIRE(tyre_wear_grip(1) == 1 && tyre_wear_grip(.2) == 1);
  SLRR_REQUIRE(tyre_wear_grip(0) == .6 && tyre_wear_grip(.075) > .6);
  SLRR_REQUIRE(tyre_wear_stiffness(0) > tyre_wear_stiffness(1));
  // The solver counts the work: a spinning wheel's grows, a worn-out tyre
  // corners less.
  double lateral[2];
  for (int worn = 0; worn < 2; ++worn) {
    Car car;
    car.run(1, 0, 0, 0);
    for (auto& w : car.dynamics.drive.wheels) w.tread_wear = worn ? 0 : 1;
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -20});
    for (auto& wheel : car.dynamics.state) wheel.spin = 20 / .31;
    car.dynamics.drive.engaged = false;
    const double before = car.dynamics.state[0].wear_work;
    car.run(2, 0, 0, .1);
    SLRR_REQUIRE(car.dynamics.state[0].wear_work > before);
    double force = 0;
    for (const auto& wheel : car.dynamics.state) force += wheel.tire_force.lateral;
    lateral[worn] = std::abs(force);
  }
  SLRR_REQUIRE(lateral[1] < lateral[0] * .8);
}

SLRR_TEST(a_punctured_tyre_deflates_and_the_corner_sinks) {
  // #461: a blowout empties the tyre in a fraction of a second, a pinch over
  // a few; flat, the corner drops on the folded sidewall.
  Car car;
  car.run(3, 0, 0, 0);
  const double height = car.dynamics.body.position.y;
  const auto& front = car.dynamics.state[0];
  const double loaded = front.force;
  car.dynamics.drive.wheels[0].puncture = 1;
  car.run(1, 0, 0, 0);
  SLRR_REQUIRE(front.deflation > .1 && front.deflation < .3);
  car.dynamics.drive.wheels[0].puncture = 2;
  car.run(1, 0, 0, 0);
  SLRR_REQUIRE(front.deflation > .99);
  SLRR_REQUIRE(front.tyre_rate < .3 * 250000);
  SLRR_REQUIRE(car.dynamics.body.position.y < height - .005);
  SLRR_REQUIRE(front.force > .5 * loaded);  // it still carries its corner
  // Repaired (or a new tyre): full again at once.
  car.dynamics.drive.wheels[0].puncture = 0;
  car.run(.1, 0, 0, 0);
  SLRR_REQUIRE(front.deflation == 0);
}

SLRR_TEST(a_locked_stop_grinds_a_flat_spot_that_thumps_once_a_turn) {
  // #461: a locked stop from 100 km/h leaves about a millimetre where each
  // front tyre slid; rolling again, the spot lifts the support once a turn.
  Car car;
  for (auto& w : car.dynamics.drive.wheels) w.tyre_id = 1;
  car.run(1, 0, 0, 0);
  car.dynamics.drive.engaged = false;
  car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -27.8});
  for (auto& wheel : car.dynamics.state) wheel.spin = 27.8 / .31;
  car.run(6, 0, 1, 0);
  const auto& front = car.dynamics.state[0];
  SLRR_REQUIRE(car.speed() < .5);
  SLRR_REQUIRE(front.flat_spot > .0003 && front.flat_spot < .004);
  const double spot = front.flat_spot;
  std::printf("FLAT SPOT after a locked stop from 100 km/h: %.2f mm front, %.2f mm rear\n",
              spot * 1000, car.dynamics.state[2].flat_spot * 1000);
  // Rolling at 60 km/h: the support dips once a turn, by up to the depth.
  car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -16.7});
  for (auto& wheel : car.dynamics.state) wheel.spin = 16.7 / .31;
  double deepest = 0, shallowest = 1;
  int dips = 0;
  bool in_dip = false;
  for (int i = 0; i < 60; ++i) {
    car.run(1. / 60, 0, 0, 0);
    deepest = std::max(deepest, front.flat_drop);
    shallowest = std::min(shallowest, front.flat_drop);
    const bool dip = front.flat_drop > .5 * spot;
    dips += dip && !in_dip;
    in_dip = dip;
  }
  SLRR_REQUIRE(deepest > .5 * spot && shallowest == 0);
  SLRR_REQUIRE(front.flat_spot == spot);  // rolling does not grind it
  // A new tyre has none.
  auto& fitted = car.dynamics.drive.wheels[0];
  fitted.tyre_id = 2;
  car.run(1. / 60, 0, 0, 0);
  SLRR_REQUIRE(front.flat_spot == 0);
  // A tyre loaded from a save brings its spot; a repair (the Tyre clears it
  // and the host bumps the serial) takes it away.
  fitted.tyre_id = 3;
  fitted.flat_spot = .002;
  fitted.flat_spot_angle = 1;
  car.run(1. / 60, 0, 0, 0);
  SLRR_REQUIRE(front.flat_spot == .002 && front.flat_spot_angle == 1);
  fitted.flat_spot = 0;
  ++fitted.flat_serial;
  car.run(1. / 60, 0, 0, 0);
  SLRR_REQUIRE(front.flat_spot == 0);
}

SLRR_TEST(a_kerb_taller_than_the_sidewall_stops_the_tyre_sideways) {
  // #467: drifting at 1.5 m/s into a 13 cm kerb beside the right wheels
  // (the test body's box hangs 15 cm up, wider than the tyres). Above the
  // sidewall bulge it bears on the sidewall: the car is pushed back, not
  // thrown up by the kerb's excess over the bulge.
  for (double kerb : {.13, .08}) {
    Car car;
    for (auto& w : car.dynamics.wheels) w.half_width = .1025;
    car.run(2, 0, 0, 0);
    auto& d = car.dynamics;
    double right = -1e9;
    for (size_t i = 1; i < 4; i += 2) right = std::max(right, d.state[i].center.x);
    const double edge = right + .1025 + .02 + .05;
    const Terrain ground = [&](double x, double, double, GroundHit& hit) {
      hit = {};
      hit.height = x > edge ? kerb : 0;
      return true;
    };
    d.drive.engaged = false;
    d.body.velocity = d.body.orientation.rotate({0, 0, -8}) + Vec3{1.5, 0, 0};
    for (auto& w : d.state) w.spin = 8 / .31;
    double rise = 0, lowest_side = 1e9;
    const double y0 = d.body.position.y;
    for (int i = 0; i < 90; ++i) {
      d.advance(1. / 60, ground);
      rise = std::max(rise, d.body.position.y - y0);
      lowest_side = std::min(lowest_side, d.body.velocity.x);
    }
    std::printf("KERB %.2f m beside: body rise %.1f mm, lateral speed down to %.2f m/s%c",
                kerb, rise * 1000, lowest_side, 10);
    if (kerb > .12) {
      // The wall: pushed back out; the body only rolls on the impact (main:
      // 76 mm, the wheel jumping onto the kerb).
      SLRR_REQUIRE(lowest_side < .3);
      SLRR_REQUIRE(rise < .035);
    } else {
      // Below the bulge the tyre rolls onto it: the car climbs.
      SLRR_REQUIRE(rise > .035);
    }
  }
}

SLRR_TEST(an_angled_kerb_beside_the_tyre_pushes_square_to_itself) {
  // #467: the same 13 cm kerb, parallel or closing in at 20 degrees ahead of
  // the right wheels. Its face pushes square to itself, so the angled one
  // also slows the car (pushing across the wheel only, it did not); met at
  // 2.5 m/s across, the tyre then climbs it, as a real one would.
  double slowed[2], rise[2];
  for (int angled = 0; angled < 2; ++angled) {
    Car car;
    for (auto& w : car.dynamics.wheels) w.half_width = .1025;
    car.run(2, 0, 0, 0);
    auto& d = car.dynamics;
    double right = -1e9;
    for (size_t i = 1; i < 4; i += 2) right = std::max(right, d.state[i].center.x);
    const double edge = right + .1025 + .02 + .02;
    const double z0 = d.body.position.z, slope = angled ? std::tan(20 * std::numbers::pi / 180) : 0;
    // Ahead (z below z0) the angled kerb comes in by tan(20 deg) per metre.
    const Terrain ground = [&](double x, double z, double, GroundHit& hit) {
      hit = {};
      hit.height = x > edge - slope * std::max(0.0, z0 - 1.3 - z) ? .13 : 0;
      return true;
    };
    d.drive.engaged = false;
    d.body.velocity = d.body.orientation.rotate({0, 0, -6}) + Vec3{.3, 0, 0};
    for (auto& w : d.state) w.spin = 6 / .31;
    const double y0 = d.body.position.y;
    rise[angled] = 0;
    for (int i = 0; i < 60; ++i) {
      d.advance(1. / 60, ground);
      rise[angled] = std::max(rise[angled], d.body.position.y - y0);
    }
    slowed[angled] = 6 + d.body.velocity.z;  // forward is -z
  }
  std::printf("ANGLED KERB: speed lost %.2f m/s parallel, %.2f m/s at 20 deg; rise %.1f / %.1f mm%c",
              slowed[0], slowed[1], rise[0] * 1000, rise[1] * 1000, 10);
  SLRR_REQUIRE(slowed[1] > slowed[0] + .3);
  SLRR_REQUIRE(rise[0] < .035);
}

SLRR_TEST(an_exhaust_scrapes_over_a_hump_instead_of_passing_through) {
  // #467: an exhaust 2 cm clear of the road at rest, a 7 cm hump between
  // the wheels. Without a scrape point the pipe passes 5 cm through it; with
  // one it gives 3 cm on its hangers, then the pipe hits and the car is
  // lifted on its suspension; it drags too.
  double speed[2], deepest[2];
  for (int scraping = 0; scraping < 2; ++scraping) {
    Car car;
    car.run(2, 0, 0, 0);
    auto& d = car.dynamics;
    const auto body_point = [&](const RigidBody& b, Vec3 p) {
      return b.position + b.orientation.rotate(p - b.center);
    };
    // A point under the floor, 2 cm above the road where the car rests.
    const Vec3 local{0, 0, 0};
    const double clearance = body_point(d.body, local).y;
    const Vec3 pipe = local + Vec3{0, .02 - clearance, 0};
    if (scraping) {
      d.scrape_points[0] = pipe;
      d.scrape_count = 1;
    }
    const double z0 = d.body.position.z;
    const Terrain hump = [&](double x, double z, double, GroundHit& hit) {
      hit = {};
      // Ramped up over a metre, flat for 4 m, down over a metre.
      const double along = z0 - 2 - z;
      hit.height = std::abs(x) < .3 ? .07 * std::clamp(std::min(along, 6 - along), 0.0, 1.0) : 0;
      return true;
    };
    d.drive.engaged = false;
    d.body.velocity = d.body.orientation.rotate({0, 0, -5});
    for (auto& w : d.state) w.spin = 5 / .31;
    deepest[scraping] = 0;
    for (int i = 0; i < 120; ++i) {
      d.advance(1. / 60, hump);
      const Vec3 at = body_point(d.body, pipe);
      GroundHit hit;
      hump(at.x, at.z, 0, hit);
      deepest[scraping] = std::max(deepest[scraping], hit.height - at.y);
    }
    speed[scraping] = car.speed();
  }
  std::printf("SCRAPE over a 7 cm hump: %.2f m/s (pipe %.1f cm into it) without, %.2f m/s (%.1f cm) with%c",
              speed[0], deepest[0] * 100, speed[1], deepest[1] * 100, 10);
  SLRR_REQUIRE(deepest[0] > .045);  // passed through
  // The hangers' 3 cm, then the pipe stops and the suspension lifts the car.
  SLRR_REQUIRE(deepest[1] < .045 && deepest[1] < deepest[0] - .007);
  SLRR_REQUIRE(speed[1] < speed[0] - .05);  // and dragged
}

SLRR_TEST(an_exhaust_in_the_road_at_rest_is_raised_clear) {
  // #467: a mesh the static ride height puts 1 cm into level ground. The host
  // raises it to 5 mm clear (scrape_lift, from the parked car): a straight is
  // then not slowed, while the same point left in the road drags.
  double speed[3];
  for (int scraping = 0; scraping < 3; ++scraping) {
    Car car;
    car.run(2, 0, 0, 0);
    auto& d = car.dynamics;
    const Vec3 local{0, 0, 0};
    const double clearance = (d.body.position + d.body.orientation.rotate(local - d.body.center)).y;
    if (scraping) {
      d.scrape_points[0] = local + Vec3{0, -.01 - clearance, 0};
      d.scrape_count = 1;
      d.scrape_lift[0] = scraping == 2 ? .015 : 0;
    }
    d.drive.engaged = false;
    d.body.velocity = d.body.orientation.rotate({0, 0, -10});
    for (auto& w : d.state) w.spin = 10 / .31;
    car.run(2, 0, 0, 0);
    speed[scraping] = car.speed();
  }
  SLRR_REQUIRE(speed[1] < speed[0] - .05);           // left in the road: drags
  SLRR_REQUIRE(std::abs(speed[2] - speed[0]) < .01);  // raised clear: does not
}

SLRR_TEST(anti_dive_and_anti_squat_carry_part_of_the_load_transfer) {
  // #469: braking from 20 m/s and launching, with no anti geometry and with
  // the typical strut / independent arm shares. The arms take part of the
  // load transfer: the car dives and squats less, and stops about as short.
  const auto pitch = [](const Car& car) {
    const Vec3 forward = car.dynamics.body.orientation.rotate({0, 0, -1});
    return std::asin(std::clamp(forward.y, -1.0, 1.0));  // nose up positive
  };
  double dive[2], squat[2], stop[2];
  for (int anti = 0; anti < 2; ++anti) {
    const auto set = [&](Car& car) {
      for (size_t i = 0; i < 4; ++i) {
        car.dynamics.wheels[i].anti_brake = anti ? (i < 2 ? .15 : .30) : 0;
        car.dynamics.wheels[i].anti_drive = anti ? (i < 2 ? .10 : .40) : 0;
      }
    };
    Car braking;
    set(braking);
    braking.run(2, 0, 0, 0);
    const double level = pitch(braking);
    braking.dynamics.drive.engaged = false;
    braking.dynamics.body.velocity = braking.dynamics.body.orientation.rotate({0, 0, -20});
    for (auto& w : braking.dynamics.state) w.spin = 20 / .31;
    const Vec3 start = braking.dynamics.body.position;
    dive[anti] = 0;
    for (int i = 0; i < 240 && braking.speed() > .1; ++i) {
      braking.run(1. / 60, 0, 1, 0);
      dive[anti] = std::max(dive[anti], level - pitch(braking));
    }
    stop[anti] = (braking.dynamics.body.position - start).length();
    Car launching;
    set(launching);
    launching.run(2, 0, 0, 0);
    squat[anti] = 0;
    for (int i = 0; i < 120; ++i) {
      launching.run(1. / 60, 1, 0, 0);
      squat[anti] = std::max(squat[anti], pitch(launching) - level);
    }
  }
  std::printf("ANTI dive %.2f -> %.2f deg, squat %.2f -> %.2f deg, stop %.1f -> %.1f m%c",
              dive[0] * 57.3, dive[1] * 57.3, squat[0] * 57.3, squat[1] * 57.3, stop[0], stop[1], 10);
  // About 15 % front and 30 % rear of the dive; the rear's 40 % of the squat
  // (the undriven front still rises).
  SLRR_REQUIRE(dive[1] < dive[0] * .85 && dive[1] > dive[0] * .6);
  SLRR_REQUIRE(squat[1] < squat[0] * .9);
  SLRR_REQUIRE(std::abs(stop[1] - stop[0]) < .05 * stop[0]);
}

SLRR_TEST(camber_gain_follows_the_curve_about_the_static_height) {
  // #481: the patched Nonus GT II strut, its camber curve set like a
  // short-long arm's: -1 degree per 25 mm of bump at the static height,
  // growing by 0.15 per 25 mm. The static camber stays the geometry's.
  constexpr double degree = std::numbers::pi / 180, per_25mm = degree / .025;
  auto w = caster_strut();
  w.spring = 30000;
  const double load = 3500, level = w.rest - load / w.spring;
  const auto camber = [&](double length) {
    // Left wheel: the axle rises towards its inboard end (+x).
    const Vec3 axle = wheel_pose(w, length).hub_rotation.rotate({1, 0, 0});
    return std::asin(axle.y) / degree;
  };
  const double geometry[] = {camber(level), camber(level - .025), camber(level - .05),
                             camber(level + .025)};
  // The strut alone: about -0.4 degrees for the first 25 mm, fading.
  SLRR_REQUIRE(std::abs(geometry[0] - geometry_camber(w, level) / degree) < 1e-9);
  SLRR_REQUIRE(geometry[1] - geometry[0] < -.2 && geometry[1] - geometry[0] > -.7);
  SLRR_REQUIRE(geometry[2] - geometry[1] > geometry[1] - geometry[0]);
  w.camber_curve = true;
  w.camber_gain = -1 * per_25mm;
  w.camber_progression = -.15 * per_25mm / .025;
  // No reference yet (no weight): the geometry alone.
  SLRR_REQUIRE(std::abs(camber(level - .025) - geometry[1]) < 1e-9);
  update_static_reference(w, load);
  SLRR_REQUIRE(std::abs(w.static_length - level) < 1e-9);
  SLRR_REQUIRE(std::abs(camber(level) - geometry[0]) < 1e-6);
  SLRR_REQUIRE(std::abs(camber(level - .025) - geometry[0] - (-1 - .075)) < .01);
  SLRR_REQUIRE(std::abs(camber(level - .05) - geometry[0] - (-2 - .3)) < .01);
  SLRR_REQUIRE(std::abs(camber(level + .025) - geometry[0] - (1 - .075)) < .01);
  // Progressive: the second 25 mm gains more than the first.
  SLRR_REQUIRE(camber(level - .05) - camber(level - .025) < camber(level - .025) - camber(level));
  // The right wheel mirrors it.
  auto right = w;
  right.center.x = -w.center.x;
  right.arm_pivot.x = -w.arm_pivot.x;
  right.hub[1] = -w.hub[1];
  right.hub[4] = -w.hub[4];
  right.hub[7] = -w.hub[7];
  update_static_reference(right, load);
  const auto right_camber = [&](double length) {
    const Vec3 axle = wheel_pose(right, length).hub_rotation.rotate({1, 0, 0});
    return -std::asin(axle.y) / degree;
  };
  SLRR_REQUIRE(std::abs(right_camber(level - .05) - camber(level - .05)) < .05);
  // Without a gain of its own the curve takes the geometry's at rest.
  w.camber_gain = std::numeric_limits<double>::quiet_NaN();
  w.camber_progression = 0;
  update_static_reference(w, load);
  SLRR_REQUIRE(std::abs(w.reference_gain * .025 / degree - (geometry[1] - geometry[3]) / 2) < .05);
  // No spring, no curve.
  w.spring = 0;
  update_static_reference(w, load);
  SLRR_REQUIRE(w.static_length == 0);
}

SLRR_TEST(camber_gain_keeps_the_outer_wheel_leaning_into_a_turn) {
  // #481: the test saloon on 1.6 m swing arms, in a steady left turn. With
  // no gain the outer (right) front wheel leans out with the body; a
  // short-long arm's curve keeps it leaning into the turn against the road.
  constexpr double degree = std::numbers::pi / 180, per_25mm = degree / .025;
  double outer[2], roll[2];
  for (int gain = 0; gain < 2; ++gain) {
    Car car;
    for (size_t i = 0; i < 4; ++i) {
      auto& w = car.dynamics.wheels[i];
      const double side = i % 2 ? -1 : 1;
      w.arm_length = 1.6;
      w.arm_pivot = w.center + Vec3{side * 1.6, 0, 0};
      w.arm_axis = {0, 0, 1};
      w.camber_curve = true;
      w.camber_gain = gain ? (i < 2 ? -1 : -.7) * per_25mm : 0;
      w.camber_progression = gain && i < 2 ? -.15 * per_25mm / .025 : 0;
    }
    car.run(2, 0, 0, 0);
    car.dynamics.drive.engaged = false;
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -16});
    for (auto& s : car.dynamics.state) s.spin = 16 / .31;
    car.run(2.5, 0, 0, .09);
    const auto& body = car.dynamics.body;
    const Vec3 up = body.orientation.rotate(car.dynamics.state[1].hub_rotation.rotate({0, 1, 0}));
    const Vec3 right = body.orientation.rotate({1, 0, 0});
    // Against the road: the top towards -right (inboard) is negative camber.
    outer[gain] = std::asin(std::clamp(up.dot(Vec3{right.x, 0, right.z}.normalized()), -1.0, 1.0)) / degree;
    roll[gain] = car.roll() / degree;
  }
  std::printf("CAMBER outer front %.2f -> %.2f deg against the road, roll %.2f / %.2f deg%c",
              outer[0], outer[1], roll[0], roll[1], 10);
  SLRR_REQUIRE(std::abs(roll[0]) > 1);
  SLRR_REQUIRE(outer[0] > .5);
  SLRR_REQUIRE(outer[1] < outer[0] - 1);
}

SLRR_TEST(bump_steer_follows_the_toe_curve_about_the_static_height) {
  // #484: the patched Nonus GT II strut with 0.2 degrees of toe-in per
  // 25 mm of bump, growing by 0.1 per 25 mm. The static toe stays the
  // geometry's, and the camber does not move.
  constexpr double degree = std::numbers::pi / 180, per_25mm = degree / .025;
  auto w = caster_strut();
  w.spring = 30000;
  const double load = 3500, level = w.rest - load / w.spring;
  // From the axle (the hub's pitch for its caster leaves it alone): toe its
  // sweep back, camber its rise towards the inboard end.
  const auto toe = [](const WheelConfig& wheel, double length) {
    const Vec3 axle = wheel_pose(wheel, length).hub_rotation.rotate({1, 0, 0});
    return std::asin(axle.z * (wheel.center.x < 0 ? 1 : -1)) / degree;
  };
  const auto camber = [](const WheelConfig& wheel, double length) {
    const Vec3 axle = wheel_pose(wheel, length).hub_rotation.rotate({1, 0, 0});
    return std::asin(axle.y * (wheel.center.x < 0 ? 1 : -1)) / degree;
  };
  const double geometry = toe(w, level), bumped_camber = camber(w, level - .05);
  SLRR_REQUIRE(std::abs(geometry - geometry_toe(w, level) / degree) < 1e-9);
  w.toe_curve = true;
  w.bump_steer = .2 * per_25mm;
  w.bump_steer_progression = .1 * per_25mm / .025;
  update_static_reference(w, load);
  SLRR_REQUIRE(std::abs(w.static_length - level) < 1e-9);
  SLRR_REQUIRE(std::abs(toe(w, level) - geometry) < 1e-6);
  SLRR_REQUIRE(std::abs(toe(w, level - .025) - geometry - (.2 + .05)) < .005);
  SLRR_REQUIRE(std::abs(toe(w, level - .05) - geometry - (.4 + .2)) < .005);
  SLRR_REQUIRE(std::abs(toe(w, level + .025) - geometry - (-.2 + .05)) < .005);
  SLRR_REQUIRE(std::abs(camber(w, level - .05) - bumped_camber) < .01);
  // The right wheel mirrors it.
  auto right = w;
  right.center.x = -w.center.x;
  right.arm_pivot.x = -w.arm_pivot.x;
  right.hub[1] = -w.hub[1];
  right.hub[4] = -w.hub[4];
  right.hub[7] = -w.hub[7];
  update_static_reference(right, load);
  SLRR_REQUIRE(std::abs(toe(right, level - .05) - toe(w, level - .05)) < .01);
}

SLRR_TEST(rear_toe_in_under_bump_steers_a_rolling_car_out_of_its_turn) {
  // #484: the test saloon on 1.6 m swing arms in a steady left turn. Rear
  // wheels that toe in as they compress steer the loaded outer one out of
  // the turn: the car yaws less than with rears that toe out.
  constexpr double degree = std::numbers::pi / 180, per_25mm = degree / .025;
  double yaw[2], outer_toe[2];
  for (int in = 0; in < 2; ++in) {
    Car car;
    for (size_t i = 0; i < 4; ++i) {
      auto& w = car.dynamics.wheels[i];
      const double side = i % 2 ? -1 : 1;
      w.arm_length = 1.6;
      w.arm_pivot = w.center + Vec3{side * 1.6, 0, 0};
      w.arm_axis = {0, 0, 1};
      w.toe_curve = true;
      w.bump_steer = i < 2 ? 0 : (in ? .3 : -.3) * per_25mm;
    }
    car.run(2, 0, 0, 0);
    car.dynamics.drive.engaged = false;
    car.dynamics.body.velocity = car.dynamics.body.orientation.rotate({0, 0, -16});
    for (auto& s : car.dynamics.state) s.spin = 16 / .31;
    car.run(2.5, 0, 0, .09);
    yaw[in] = car.dynamics.body.omega().y;
    const Vec3 forward = car.dynamics.state[3].hub_rotation.rotate({0, 0, -1});
    outer_toe[in] = std::atan2(-forward.x, -forward.z) / degree;  // right wheel: inboard is -x
  }
  std::printf("BUMPSTEER yaw %.3f (toe-out) / %.3f (toe-in) rad/s, outer rear toe %.2f / %.2f deg%c",
              yaw[0], yaw[1], outer_toe[0], outer_toe[1], 10);
  SLRR_REQUIRE(yaw[0] > .1);
  SLRR_REQUIRE(outer_toe[1] > .3 && outer_toe[0] < -.3);
  SLRR_REQUIRE(yaw[1] < yaw[0] * .97);
}

SLRR_TEST(hot_brake_fluid_boils_and_the_pressure_falls) {
  const auto brake = brake_hardware(0, 2, .286);
  double fluid = kAmbientTemperature;
  SLRR_REQUIRE(fluid_pressure_factor(brake, fluid) == 1);
  for (int step = 0; step < 120000; ++step) fluid = step_fluid_temperature(brake, fluid, 700, .001);
  SLRR_REQUIRE(fluid > 230);
  SLRR_REQUIRE(fluid_pressure_factor(brake, fluid) < 1 && fluid_pressure_factor(brake, fluid) >= .4);
  for (int step = 0; step < 300000; ++step) fluid = step_fluid_temperature(brake, fluid, 100, .001);
  SLRR_REQUIRE(fluid_pressure_factor(brake, fluid) == 1);
  SLRR_REQUIRE(fluid_pressure_factor(BrakeHardware{}, 400) == 1);
}

int main() {
  const slrr::standalone::Test tests[] = {
      hot_tyres_gain_pressure_and_soften, the_tread_wears_with_hot_sliding,
      a_punctured_tyre_deflates_and_the_corner_sinks,
      a_locked_stop_grinds_a_flat_spot_that_thumps_once_a_turn,
      a_kerb_taller_than_the_sidewall_stops_the_tyre_sideways,
      an_angled_kerb_beside_the_tyre_pushes_square_to_itself,
      an_exhaust_scrapes_over_a_hump_instead_of_passing_through,
      an_exhaust_in_the_road_at_rest_is_raised_clear,
      anti_dive_and_anti_squat_carry_part_of_the_load_transfer,
      camber_gain_follows_the_curve_about_the_static_height,
      camber_gain_keeps_the_outer_wheel_leaning_into_a_turn,
      bump_steer_follows_the_toe_curve_about_the_static_height,
      rear_toe_in_under_bump_steers_a_rolling_car_out_of_its_turn,
      hard_braking_declutches_so_the_engine_never_pushes_the_rear_wheels,
      coasting_engine_braking_fades_with_the_speed,
      releasing_a_hard_brake_at_speed_takes_up_without_a_jolt,
      tyres_warm_up_rolling_and_sliding_and_set_the_grip,
      hot_brake_fluid_boils_and_the_pressure_falls,
      abs_keeps_the_wheels_turning_and_stops_shorter_than_locked,
      abs_cycles_one_wheel_holds_the_rear_axle_together_and_delays_the_yaw,
      abs_lets_the_wheels_lock_below_walking_pace,
      the_reference_speed_follows_the_accelerometer_while_every_wheel_locks,
      asr_limits_wheelspin_in_second_gear_at_speed,
      asr_brakes_the_one_spinning_wheel_of_an_open_axle,
      pad_friction_has_a_cold_bite_a_working_window_and_fades,
      a_stop_heats_the_rotors_and_unspecified_brakes_stay_cold,
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
      camber_leaning_into_the_force_adds_grip,
      aligning_torque_turns_the_tyre_towards_its_travel, caster_trail_reads_the_strut_axis,
      a_strut_with_caster_steers_about_its_kingpin,
      steering_feedback_centres_a_rolling_wheel_from_its_kingpin,
      parked_car_settles_without_vibration,
      a_sideways_slide_stops_without_swinging_back,
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
