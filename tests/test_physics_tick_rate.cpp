// Solver-rate checks for issue #203: engine, limiter and launch behaviour of
// VehicleDynamics at the compiled substep frequency, plus its cost per
// simulated second on flat ground. Build once per rate from the repository
// root (omit the define for the shipped 120 Hz):
//   cl /std:c++20 /EHsc /MD /O2 /utf-8 /Iinclude /Itests
//      /DSLRR_VEHICLE_PHYSICS_HZ=960 tests/test_physics_tick_rate.cpp
//      src/physics/vehicle_dynamics.cpp src/physics/tire_model.cpp
//      src/physics/suspension_geometry.cpp src/physics/dyno_model.cpp
//      /Fe:build/tick_960.exe /Fo:build/
// Exercise 120, 240, 480, 960 and 1000 Hz; the printed lines are the
// measurements that docs/PHYSICS_TICK_SPIKE.md compares across rates.
#include "standalone_test.hpp"

#include "slrr/vehicle_dynamics.hpp"
#include "slrr/dyno_model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace {
using namespace slrr::physics;
constexpr double kRpmPerRad = 9.54929658551372;
constexpr double kIdle = 800, kLimit = 7000, kPeakTorque = 235;

TireParameters tyre() {
  TireParameters t;
  t.friction = 1.559 * (.5 + .5 * 205 / 275.) * 1.3;
  t.maximum_load = 6500;
  t.load_smoothing = .4;
  return t;
}

// The 1300 kg rear-drive saloon of test_vehicle_dynamics.cpp.
struct Car {
  VehicleDynamics dynamics;
  explicit Car(double engine_inertia = .38) {
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
      w.front_instant_center = w.side_instant_center = true;
      w.front_center_offset = {side * 1.446, -.025, 0};
      w.side_center_offset = {0, .1, i < 2 ? 1.5 : -1.5};
      auto& d = dynamics.drive.wheels[i];
      d.weight = i < 2 ? 0 : 1;
      d.brake = 2000;
      d.handbrake = i < 2 ? 0 : 2000;
      d.tyre = tyre();
    }
    auto& drive = dynamics.drive;
    drive.gears = 5;
    const double ratios[] = {0, 12.9, 7.9, 5.6, 4.4, 3.6, 0, -12};
    std::copy(std::begin(ratios), std::end(ratios), drive.ratios.begin());
    drive.idle = kIdle;
    drive.limit = kLimit;
    drive.inertia = engine_inertia;
    drive.brake_scale = drive.handbrake_scale = 1;
    drive.engaged = drive.automatic_shift = drive.auto_clutch = true;
    drive.torque = [](double rpm) { return std::clamp(150 + rpm * .02, 0., kPeakTorque); };
    dynamics.engine_running = true;
    dynamics.engine_omega = kIdle / kRpmPerRad;
    Matrix frame{};
    frame[0] = frame[5] = frame[10] = frame[15] = 1;
    frame[13] = .05f;
    dynamics.body.set_frame(frame);
  }
  // One solver step per call, so every step can be sampled.
  template <class Each>
  void run(double seconds, Each&& each) {
    const Terrain flat = [](double, double, double, GroundHit& hit) {
      hit = {};
      return true;
    };
    const long steps = std::lround(seconds * VehicleDynamics::frequency);
    for (long i = 0; i < steps; ++i) {
      dynamics.advance(VehicleDynamics::timestep, flat);
      each(double(i + 1) / VehicleDynamics::frequency);
    }
  }
  void run(double seconds) {
    run(seconds, [](double) {});
  }
  double rpm() const { return dynamics.engine_omega * kRpmPerRad; }
};

// The most one step can add to the engine speed: step_drive applies twice
// the dyno torque at full throttle against at least 14 N m of drag.
double max_rpm_per_step(double inertia) {
  return (2 * kPeakTorque - 14) / inertia * VehicleDynamics::timestep * kRpmPerRad;
}
}  // namespace

SLRR_TEST(accumulator_ticks_split_into_whole_substeps) {
  // The kernel accumulator accepts 1/60 s directly, unlike the shared-world
  // caller below. Non-multiples of 60 alternate step counts here.
  Car car;
  car.run(.5);
  const Terrain flat = [](double, double, double, GroundHit& hit) {
    hit = {};
    return true;
  };
  unsigned fewest = ~0u, most = 0, total = 0;
  for (int tick = 0; tick < 60; ++tick) {
    const unsigned steps = car.dynamics.advance(1. / VehicleDynamics::update_frequency, flat);
    fewest = std::min(fewest, steps);
    most = std::max(most, steps);
    total += steps;
  }
  std::printf("[tick_rate] %u Hz: %u..%u substeps per 60 Hz host tick, %u per second\n",
              VehicleDynamics::frequency, fewest, most, total);
  SLRR_REQUIRE(std::abs(int(total) - int(VehicleDynamics::frequency)) <= 1);
  SLRR_REQUIRE(most - fewest == (VehicleDynamics::frequency % 60 ? 1u : 0u));
}

SLRR_TEST(shared_world_clock_coverage) {
  // Mirror advance_chassis_tick in part_runtime.cpp at d60b907: every body
  // receives one timestep in each iteration of an integer-divided loop.
  // This deliberately measures the 1000 Hz shortfall, not its suitability
  // for adoption. The kernel accumulator test alone would hide that loss.
  Car car;
  const Terrain flat = [](double, double, double, GroundHit& hit) {
    hit = {};
    return true;
  };
  unsigned total = 0;
  for (unsigned tick = 0; tick < VehicleDynamics::update_frequency; ++tick)
    for (unsigned substep = 0;
         substep < VehicleDynamics::frequency / VehicleDynamics::update_frequency; ++substep)
      total += car.dynamics.advance(VehicleDynamics::timestep, flat);
  const unsigned expected = VehicleDynamics::update_frequency *
      (VehicleDynamics::frequency / VehicleDynamics::update_frequency);
  const double covered = total * VehicleDynamics::timestep;
  std::printf("[tick_rate] %u Hz shared-world caller: %u steps, %.6f s per host second\n",
              VehicleDynamics::frequency, total, covered);
  SLRR_REQUIRE(total == expected);
  SLRR_REQUIRE(car.dynamics.backlog() == 0);
  if (VehicleDynamics::frequency % VehicleDynamics::update_frequency == 0)
    SLRR_REQUIRE(std::abs(covered - 1) < 1e-12);
  else
    SLRR_REQUIRE(covered < 1);
}

SLRR_TEST(idle_and_limiter_stay_bounded_for_every_flywheel) {
  // Neutral: idle for three seconds, full throttle against the limiter for
  // three, then lift. Inertias below the host's 0.28 kg m2 floor included.
  for (double inertia : {.10, .22, .28, .38, .60}) {
    Car car(inertia);
    car.dynamics.drive.engaged = false;
    double idle_low = 1e9, idle_high = 0, idle_sum = 0;
    long idle_samples = 0;
    car.run(3, [&](double t) {
      if (t <= 2) return;
      idle_low = std::min(idle_low, car.rpm());
      idle_high = std::max(idle_high, car.rpm());
      idle_sum += car.rpm();
      ++idle_samples;
    });
    car.dynamics.drive.throttle = 1;
    double peak = 0, band_low = 1e9, band_high = 0, reach = -1;
    car.run(3, [&](double t) {
      peak = std::max(peak, car.rpm());
      if (reach < 0 && car.rpm() >= .95 * kLimit) reach = t;
      if (t <= 2) return;
      band_low = std::min(band_low, car.rpm());
      band_high = std::max(band_high, car.rpm());
    });
    car.dynamics.drive.throttle = 0;
    double fall = -1;
    car.run(7, [&](double t) {
      if (fall < 0 && car.rpm() <= 2000) fall = t;
    });
    const double idle_mean = idle_sum / idle_samples;
    std::printf(
        "[tick_rate] %u Hz, I=%.2f: idle %.1f rpm (+-%.1f), 95%% of limit in %.3f s, "
        "peak %.0f rpm, limiter band %.0f..%.0f rpm, back to 2000 rpm in %.2f s\n",
        VehicleDynamics::frequency, inertia, idle_mean, (idle_high - idle_low) / 2, reach,
        peak, band_low, band_high, fall);
    const double step = max_rpm_per_step(inertia);
    SLRR_REQUIRE(std::isfinite(peak) && car.dynamics.engine_running);
    SLRR_REQUIRE(std::abs(idle_mean - kIdle) < kIdle * .02);
    SLRR_REQUIRE(idle_high - idle_low < 30);
    // The predictive limiter overshoots by at most one step of acceleration.
    SLRR_REQUIRE(peak <= kLimit + step * 1.05);
    SLRR_REQUIRE(band_low >= kLimit - step * 1.05);
    SLRR_REQUIRE(reach > 0 && fall > 0);
  }
}

SLRR_TEST(launch_and_solver_cost) {
  Car car;
  car.run(1);
  car.dynamics.drive.throttle = 1;
  const auto start = car.dynamics.body.position;
  double at3 = 0;
  car.run(6, [&](double t) {
    if (std::abs(t - 3) < .5 / VehicleDynamics::frequency) at3 = car.dynamics.body.velocity.length();
  });
  const double at6 = car.dynamics.body.velocity.length();
  const double distance = (car.dynamics.body.position - start).length();
  std::printf("[tick_rate] %u Hz launch: %.2f km/h at 3 s, %.2f km/h and %.2f m at 6 s, gear %d\n",
              VehicleDynamics::frequency, at3 * 3.6, at6 * 3.6, distance, car.dynamics.gear);
  SLRR_REQUIRE(at6 * 3.6 > 100 && at6 * 3.6 < 140);

  Car timed;
  timed.run(1);
  timed.dynamics.drive.throttle = 1;
  constexpr double seconds = 20;
  const auto begin = std::chrono::steady_clock::now();
  timed.run(seconds);
  const auto end = std::chrono::steady_clock::now();
  const double micros = std::chrono::duration<double, std::micro>(end - begin).count();
  std::printf("[tick_rate] %u Hz cost: %.0f us per simulated second (%.2f us per step), "
              "flat analytic ground, one car\n",
              VehicleDynamics::frequency, micros / seconds,
              micros / (seconds * VehicleDynamics::frequency));
}

SLRR_TEST(dyno_heat_and_power_audit) {
  // Synthetic DynoData defaults, not a calibrated stock-car engine. Keep the
  // AFR consistent when comparing stock optimal-95 heat with a fuel LHV.
  char witness[256]{};
  SLRR_REQUIRE(slrr::dyno_self_check(witness, sizeof(witness)));
  std::printf("[dyno_audit] %s\n", witness);
  int key = 0;
  for (float heat : {1000000.f, 1099500.f * 1.55f, 43400000.f / 13.5f}) {
    slrr::DynoStats stats;
    SLRR_REQUIRE(slrr::dyno_calculate(&key,
        {{"maxRPM", 7000.f}, {"mixture_ratio", 12.5f}, {"mixture_H", heat}}, 28.f, &stats));
    const double rpm = stats.hp_rpm;
    const double torque = slrr::dyno_sample(&key, float(rpm), 0, false);
    const double watts = slrr::dyno_sample(&key, float(rpm), 0, true);
    const double ratio = watts / (torque * rpm / kRpmPerRad);
    std::printf("[dyno_audit] H=%.0f J/kg, AFR=12.5: peak %.2f Nm at %.0f rpm, "
                "%.2f kW at %.0f rpm; power/(T*omega)=%.4f\n",
                heat, stats.max_torque, stats.torque_rpm, watts / 1000, rpm, ratio);
    SLRR_REQUIRE(stats.ok && std::isfinite(ratio) && torque > 0 && watts > 0);
    slrr::dyno_forget(&key);
  }
}

int main() {
  const slrr::standalone::Test tests[] = {
      accumulator_ticks_split_into_whole_substeps,
      shared_world_clock_coverage,
      idle_and_limiter_stay_bounded_for_every_flywheel,
      launch_and_solver_cost,
      dyno_heat_and_power_audit,
  };
  return slrr::standalone::run("physics_tick_rate", tests);
}
