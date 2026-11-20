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

// Spike-only input/airflow prototype, fed UNFILTERED pedal samples. The host
// Controller already filters pedals; adopting this must bypass that stage,
// not run a second keyboard filter. Crank/clutch/idle stay in VehicleDynamics.
struct ThrottlePrototype {
  double shaped = 0, plate = 0, load = 0;
  bool rev_cut = false, bov_latched = false, closure_reported = false;
  void step(double dt, double raw, bool digital, double rpm, double boost_ratio) {
    raw = std::clamp(raw, 0.0, 1.0);
    shaped = digital ? shaped + std::clamp(raw - shaped, -3.5 * dt, 3.5 * dt) : raw;
    const double previous = plate;
    plate += -std::expm1(-dt / .04) * (shaped - plate);
    if (shaped > plate + 1e-6) closure_reported = false;
    // Below the 3.5/s digital release rate, so keyboard lifts can vent too.
    if (!closure_reported && (plate - previous) / dt < -2.5 && boost_ratio > 1.15) {
      bov_latched = closure_reported = true;
    }
    const double normalized = std::clamp((rpm - kIdle) / (kLimit - kIdle), 0.0, 1.0);
    load = std::pow(std::clamp(plate, 0.0, 1.0), 1.4 - .4 * normalized);
    if (rpm >= kLimit) rev_cut = true;
    else if (rpm <= kLimit - 150) rev_cut = false;
  }
  bool consume_bov() {
    const bool event = bov_latched;
    bov_latched = false;
    return event;
  }
};

// Normal WOT integrates the sampled net crank torque, with no second gain
// or friction subtraction. Idle is inactive in the limiter region.
double max_rpm_per_step(double inertia) {
  return kPeakTorque / inertia * VehicleDynamics::timestep * kRpmPerRad;
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
  SLRR_REQUIRE(std::isfinite(at6) && at6 > at3 && at3 > 0 && distance > 0);

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

SLRR_TEST(wot_uses_net_curve_once_and_fuel_cut_still_decelerates) {
  for (double pedal : {0.0, .25, .5, 1.0}) {
    Car car(.28);
    car.dynamics.drive.engaged = false;
    car.dynamics.drive.throttle = pedal;
    car.dynamics.drive.torque = [](double) { return kPeakTorque; };
    constexpr double rpm = 2500;
    car.dynamics.engine_omega = rpm * slrr::engine_rpm_to_rad;
    const double before = car.dynamics.engine_omega;
    const double friction = slrr::engine_friction_torque(rpm);
    const double pumping = 14 + (rpm - kIdle) * .012;
    const double expected_net = pedal * kPeakTorque -
        (1 - pedal) * (friction + pumping);
    car.run(VehicleDynamics::timestep);
    const double actual_net = (car.dynamics.engine_omega - before) *
        car.dynamics.drive.inertia / VehicleDynamics::timestep;
    SLRR_REQUIRE(std::abs(actual_net - expected_net) < 1e-7);
  }
  Car limited;
  limited.dynamics.drive.engaged = false;
  limited.dynamics.drive.throttle = 1;
  limited.dynamics.engine_omega = (kLimit + 100) * slrr::engine_rpm_to_rad;
  const double before = limited.dynamics.engine_omega;
  limited.run(VehicleDynamics::timestep);
  SLRR_REQUIRE(limited.dynamics.engine_omega < before);
}

SLRR_TEST(starter_and_engineless_states_survive_the_loss_change) {
  SLRR_REQUIRE(slrr::engine_friction_torque(0) == 0);
  Car stopped;
  stopped.dynamics.drive.engaged = false;
  stopped.dynamics.drive.throttle = 1;
  stopped.dynamics.engine_running = false;
  stopped.dynamics.engine_omega = 0;
  stopped.run(.5);
  SLRR_REQUIRE(!stopped.dynamics.engine_running && stopped.rpm() == 0);
  // configure_drive represents a torn-off engine by a zero curve, no
  // starter and neutral. It must never acquire idle combustion.
  stopped.dynamics.drive.torque = [](double) { return 0.0; };
  stopped.run(.5);
  SLRR_REQUIRE(!stopped.dynamics.engine_running && stopped.rpm() == 0);
  Car starting;
  starting.dynamics.drive.engaged = false;
  starting.dynamics.drive.throttle = 1;
  starting.dynamics.drive.starter = 15;
  starting.dynamics.engine_running = false;
  starting.dynamics.engine_omega = 0;
  starting.run(1);
  SLRR_REQUIRE(starting.dynamics.engine_running && starting.rpm() > kIdle);
  starting.dynamics.drive.throttle = 0;
  starting.run(7);
  SLRR_REQUIRE(starting.dynamics.engine_running && std::abs(starting.rpm() - kIdle) < 30);
}

SLRR_TEST(sampled_power_follows_torque_between_nodes_and_with_nitrous) {
  int key = 0;
  slrr::DynoStats stats;
  SLRR_REQUIRE(slrr::dyno_calculate(&key,
      {{"maxRPM", 7000.f}, {"nitro_H", 1.2f}, {"nitro_cooling", .9f}}, 28.f, &stats));
  double peak = 0;
  float peak_rpm = 0;
  for (int node = 1; node < 28; ++node) {
    const float rpm = node * stats.rpm_step;
    const double torque = slrr::dyno_sample(&key, rpm, 0, false);
    if (torque > peak) { peak = torque; peak_rpm = rpm; }
  }
  SLRR_REQUIRE(std::abs(peak - stats.max_torque) < 1e-5);
  SLRR_REQUIRE(peak_rpm == stats.torque_rpm);
  for (float nitro : {0.f, .35f, 1.f}) {
    for (float rpm : {0.f, 437.f, 1234.5f, 3375.f, 6750.f, 7000.f}) {
      const double torque = slrr::dyno_sample(&key, rpm, nitro, false);
      const double watts = slrr::dyno_sample(&key, rpm, nitro, true);
      const double expected = torque * rpm * slrr::engine_rpm_to_rad;
      SLRR_REQUIRE(std::abs(watts - expected) < 1e-6 * std::max(1.0, std::abs(expected)));
    }
  }
  slrr::dyno_forget(&key);
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

SLRR_TEST(throttle_prototype_runs_inside_the_actual_solver_tick) {
  const double dt = VehicleDynamics::timestep;
  ThrottlePrototype analog, keyboard;
  analog.step(dt, 1, false, kIdle, 1);
  keyboard.step(dt, 1, true, kIdle, 1);
  SLRR_REQUIRE(analog.shaped == 1 && keyboard.shaped < 1);
  SLRR_REQUIRE(analog.plate > keyboard.plate && analog.plate < 1);
  SLRR_REQUIRE(analog.load < analog.plate); // nonlinear flow near idle
  ThrottlePrototype na_lift;
  na_lift.shaped = na_lift.plate = 1;
  na_lift.step(dt, 0, false, kIdle, 1);
  SLRR_REQUIRE(!na_lift.consume_bov());
  for (bool digital : {false, true}) {
    for (double inertia : {.10, .22, .28, .38, .60}) {
      Car car(inertia);
      car.dynamics.drive.engaged = false;
      ThrottlePrototype response;
      // A cut engine's net curve is motoring friction only. Recovering the
      // indicated curve inside step_drive then gives zero combustion, while
      // the actual plate/load (and hence pumping) stays open during fuel cut.
      car.dynamics.drive.torque = [&](double rpm) {
        return response.rev_cut ? -slrr::engine_friction_torque(rpm) : kPeakTorque;
      };
      const Terrain flat = [](double, double, double, GroundHit& hit) {
        hit = {};
        return true;
      };
      bool cut_seen = false;
      unsigned bov_events = 0, host_frame = 0;
      double peak = 0;
      const long steps = 9 * VehicleDynamics::frequency;
      for (long i = 0; i < steps; ++i) {
        const double time = double(i) / VehicleDynamics::frequency;
        const double pedal = time >= 1 && time < 5 ? 1 : 0;
        response.step(dt, pedal, digital, car.rpm(), 1.4);
        car.dynamics.drive.throttle = response.load;
        car.dynamics.advance(dt, flat); // one actual high-frequency crank step
        peak = std::max(peak, car.rpm());
        cut_seen = cut_seen || response.rev_cut;
        SLRR_REQUIRE(car.dynamics.engine_running && std::isfinite(car.rpm()));
        const unsigned frame = unsigned((i + 1.) * 60 / VehicleDynamics::frequency);
        if (frame != host_frame) {
          bov_events += response.consume_bov();
          host_frame = frame;
        }
      }
      bov_events += response.consume_bov();
      SLRR_REQUIRE(cut_seen && peak <= kLimit + max_rpm_per_step(inertia) * 1.05 + 1);
      SLRR_REQUIRE(bov_events == 1); // latched across substeps, not a repeated pulse
      SLRR_REQUIRE(response.plate < .001 && response.load < .001);
      std::printf("[throttle_prototype] %u Hz %s I=%.2f peak=%.0f rpm, one BOV event\n",
                  VehicleDynamics::frequency, digital ? "digital" : "analog", inertia, peak);
    }
  }
  // The loaded prototype uses the real clutch/gears/wheels, not a second
  // crank integrator or an invented external-load scalar.
  Car loaded;
  ThrottlePrototype response;
  const Terrain flat = [](double, double, double, GroundHit& hit) {
    hit = {};
    return true;
  };
  for (unsigned i = 0; i < 3 * VehicleDynamics::frequency; ++i) {
    response.step(dt, 1, true, loaded.rpm(), 1);
    loaded.dynamics.drive.throttle = response.load;
    loaded.dynamics.advance(dt, flat);
  }
  SLRR_REQUIRE(loaded.dynamics.engine_running && loaded.dynamics.gear > 0);
  SLRR_REQUIRE(std::isfinite(loaded.rpm()) && loaded.dynamics.body.velocity.z < -1);
}

int main() {
  const slrr::standalone::Test tests[] = {
      accumulator_ticks_split_into_whole_substeps,
      shared_world_clock_coverage,
      idle_and_limiter_stay_bounded_for_every_flywheel,
      launch_and_solver_cost,
      dyno_heat_and_power_audit,
      wot_uses_net_curve_once_and_fuel_cut_still_decelerates,
      starter_and_engineless_states_survive_the_loss_change,
      sampled_power_follows_torque_between_nodes_and_with_nitrous,
      throttle_prototype_runs_inside_the_actual_solver_tick,
  };
  return slrr::standalone::run("physics_tick_rate", tests);
}
