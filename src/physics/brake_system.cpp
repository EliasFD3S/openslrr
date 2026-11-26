#include "slrr/brake_system.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace slrr::physics {
namespace {

struct CurvePoint {
  double temperature, factor;
};

template <size_t N>
double interpolate(const CurvePoint (&curve)[N], double t) {
  if (t <= curve[0].temperature) return curve[0].factor;
  for (size_t i = 1; i < N; ++i)
    if (t <= curve[i].temperature) {
      const auto& a = curve[i - 1];
      const auto& b = curve[i];
      return a.factor + (b.factor - a.factor) * (t - a.temperature) / (b.temperature - a.temperature);
    }
  return curve[N - 1].factor;
}

// Pad friction against rotor temperature, relative to the working value.
// Typical published shapes (street, fast-road and race compounds), not
// measurements of one product: a street pad works from cold and fades past
// about 400 deg C, a race pad is weak cold and holds to about 650.
constexpr CurvePoint kStreetPad[] = {{0, .88}, {100, 1}, {350, 1}, {450, .85},
                                     {550, .65}, {700, .45}, {900, .35}};
constexpr CurvePoint kFastRoadPad[] = {{0, .82}, {150, 1}, {450, 1}, {550, .85},
                                       {650, .65}, {800, .45}, {1000, .35}};
constexpr CurvePoint kRacePad[] = {{0, .75}, {100, .85}, {250, 1}, {650, 1},
                                   {750, .85}, {900, .6}, {1100, .45}};

constexpr double kSigma = 5.670374419e-8;

// One face of the rotor's swept ring (the pads' track), m²: inner radius
// 0.55 of the outer one.
double face_area(const BrakeHardware& b) {
  const double r = std::max(.05, b.diameter * .5);
  return 3.141592653589793 * r * r * (1 - .55 * .55);
}

}  // namespace

BrakeHardware brake_hardware(double pad, double rotor, double diameter) {
  BrakeHardware b;
  if (!(diameter > 0) || !std::isfinite(pad) || !std::isfinite(rotor)) return b;
  b.specified = true;
  b.pad = static_cast<PadCompound>(std::clamp(static_cast<int>(std::lround(pad)), 0, 2));
  b.rotor = static_cast<RotorType>(std::clamp(static_cast<int>(std::lround(rotor)), 0, 3));
  b.diameter = std::clamp(diameter, .15, .5);
  return b;
}

double rotor_heat_capacity(const BrakeHardware& b) {
  // Equivalent solid thickness of the ring: a 12-13 mm solid disc, a 28 mm
  // vented iron disc (about 22 mm of metal), a 34 mm carbon-ceramic one.
  // Iron 7200 kg/m³ and 500 J/(kg K); carbon-ceramic 2450 kg/m³ and 1100.
  // The pads, hat and hub heat with the ring: +15 %.
  const bool carbon = b.rotor == RotorType::carbon;
  const double thickness = carbon ? .026 : b.rotor == RotorType::vented ? .022 : .013;
  const double density = carbon ? 2450 : 7200, specific = carbon ? 1100 : 500;
  return 1.15 * face_area(b) * thickness * density * specific;
}

double rotor_cooling(const BrakeHardware& b, double speed, double temperature) {
  const double face = face_area(b);
  // Vanes add about 1.8 faces of swept area; grooves a little.
  const double vanes = b.rotor == RotorType::vented || b.rotor == RotorType::carbon ? 1.8
                       : b.rotor == RotorType::grooved                              ? .2
                                                                                    : 0;
  const double h = 12 + 18 * std::pow(std::abs(speed), .8);  // W/(m² K)
  const double excess = temperature - kAmbientTemperature;
  const double emissivity = b.rotor == RotorType::carbon ? .8 : .6;
  const double kelvin = temperature + 273.15, ambient = kAmbientTemperature + 273.15;
  const double radiation = emissivity * kSigma * 2 * face *
                           (kelvin * kelvin * kelvin * kelvin - ambient * ambient * ambient * ambient);
  return h * (2 + vanes) * face * excess + radiation;
}

double pad_friction_factor(const BrakeHardware& b, double temperature) {
  if (!b.specified) return 1;
  double t = temperature;
  // Carbon-ceramic carries the heat away from the pad face and keeps its
  // friction far hotter; grooves vent the pad gases.
  if (b.rotor == RotorType::carbon && t > 300) t = 300 + (t - 300) * .65;
  else if (b.rotor == RotorType::grooved && t > 300) t = 300 + (t - 300) * .92;
  switch (b.pad) {
    case PadCompound::fast_road: return interpolate(kFastRoadPad, t);
    case PadCompound::race: return interpolate(kRacePad, t);
    default: return interpolate(kStreetPad, t);
  }
}

double step_rotor_temperature(const BrakeHardware& b, double temperature, double heat,
                              double speed, double dt) {
  if (!b.specified) return kAmbientTemperature;
  const double capacity = rotor_heat_capacity(b);
  const double next =
      temperature + (std::max(0.0, heat) - rotor_cooling(b, speed, temperature) * dt) / capacity;
  return std::clamp(next, kAmbientTemperature, 1500.0);
}

double ReferenceSpeed::update(double dt, double acceleration, bool braking,
                              const std::array<double, 4>& wheels,
                              const std::array<bool, 4>& contact,
                              const std::array<bool, 4>& trusted) {
  constexpr double inf = std::numeric_limits<double>::infinity();
  double fastest = -inf, slowest = inf, slowest_trusted = inf;
  for (size_t i = 0; i < wheels.size(); ++i) {
    if (!contact[i]) continue;
    fastest = std::max(fastest, wheels[i]);
    slowest = std::min(slowest, wheels[i]);
    if (trusted[i]) slowest_trusted = std::min(slowest_trusted, wheels[i]);
  }
  // The sensor saturates at 1.5 g: an impact must not throw the estimate.
  double v = speed_ + std::clamp(acceleration, -15.0, 15.0) * dt;
  if (fastest > -inf) {
    if (braking) {
      // Every wheel is slower than the car: the fastest one bounds it from
      // below. Drift above a rolling wheel fades over 0.6 s; with every
      // wheel far below the estimate (all locking) only the accelerometer
      // counts, until 0.4 s pass without a wheel spinning back up: released
      // wheels that stay still mean the car is slower than estimated.
      const bool spinning_up = fastest > last_fastest_ + .5 * dt;
      if (fastest > .7 * v || spinning_up) stalled_ = 0;
      else stalled_ += dt;
      if (v < fastest || stalled_ > .4) {
        v = std::max(fastest, 0.0);
        stalled_ = 0;
      } else if (fastest > .7 * v) {
        v += (fastest - v) * std::min(1.0, dt / .6);
      }
    } else {
      stalled_ = 0;
      // The undriven wheels roll at the car's speed. With every wheel driven
      // the slowest one is only a weak hint.
      const bool undriven = slowest_trusted < inf;
      const double target = undriven ? slowest_trusted : slowest;
      if (v > target) v = target;
      else v += (target - v) * std::min(1.0, dt / (undriven ? .1 : 1.0));
    }
  }
  last_fastest_ = fastest > -inf ? fastest : 0;
  speed_ = std::max(0.0, v);
  return speed_;
}

void AbsModulator::reset() {
  channels_ = {};
  output_ = {};
  asymmetry_ = 0;
}

bool AbsModulator::active() const {
  return std::any_of(channels_.begin(), channels_.end(),
                     [](const Channel& c) { return c.phase != Phase::follow; });
}

void AbsModulator::update(double dt, double driver, double strength, double reference,
                          double vehicle_deceleration, const std::array<double, 4>& wheels,
                          const std::array<double, 4>& wheel_acceleration,
                          const std::array<bool, 4>& enabled,
                          const std::array<bool, 4>& contact, std::size_t front) {
  driver = std::clamp(driver, 0.0, 1.0);
  const double s = std::clamp(strength, 0.0, 1.0);
  // Strength 1 opens the valves at 16 % slip, just past a road tyre's peak
  // (10-15 %); lower settings let the wheels slip further.
  const double threshold = .16 + .10 * (1 - s);
  const double deceleration_trigger = -(std::max(0.0, vehicle_deceleration) + 6);
  for (size_t i = 0; i < channels_.size(); ++i) {
    auto& c = channels_[i];
    if (!enabled[i] || s <= 0 || reference < 2 || driver < .02) {
      // Off, or below walking pace (where a locked wheel is the stop), or
      // the pedal released: the pressure follows the pedal.
      c = {Phase::follow, driver, 0, 0};
      continue;
    }
    const double slip = (reference - wheels[i]) / std::max(reference, 1.0);
    const double acceleration = wheel_acceleration[i];
    // Electronic brake distribution: the rear channels open earlier, so the
    // rears always slip less than the fronts (no proportioning valve).
    const double limit = i < front ? threshold : .7 * threshold;
    const bool locking = contact[i] && (slip > limit ||
                                        (acceleration < deceleration_trigger && slip > .6 * limit));
    switch (c.phase) {
      case Phase::follow:
        c.pressure = driver;
        if (locking) {
          c.lock_pressure = c.pressure;
          c.phase = Phase::dump;
        }
        break;
      case Phase::dump:
        // The outlet valve opens: the pressure falls towards the return
        // pump's reservoir, about 35 ms per e-fold.
        c.pressure *= std::exp(-dt / .035);
        if (acceleration > 0 || c.pressure < .02) {
          c.phase = Phase::hold;
          c.timer = 0;
        }
        break;
      case Phase::hold:
        c.timer += dt;
        if (slip > 1.5 * limit && acceleration < 0) {
          c.phase = Phase::dump;
        } else if ((acceleration < 3 && slip < .6 * limit) || c.timer > .12) {
          // Spun back up: a quick first pulse, then a slow ramp.
          c.phase = Phase::rebuild;
          c.timer = 0;
          c.pressure = std::max(c.pressure, .8 * c.lock_pressure);
        }
        break;
      case Phase::rebuild:
        c.timer += dt;
        // Steps of the inlet valve: quick while the wheel is well below the
        // threshold, slow near it.
        c.pressure += dt * (slip < .5 * limit ? 4 : 1.2);
        if (locking) {
          c.lock_pressure = std::min(c.pressure, driver);
          c.phase = Phase::dump;
        } else if (c.pressure >= driver && c.timer > .3) {
          c.phase = Phase::follow;
        }
        break;
    }
    c.pressure = std::clamp(c.pressure, 0.0, driver);
  }
  for (size_t i = 0; i < output_.size(); ++i) output_[i] = channels_[i].pressure;
  // Yaw moment build-up delay on the front axle: once one front wheel is
  // modulating (split friction), the other's pressure may only exceed it by
  // a margin that grows over about 0.8 s, so the driver has time to steer
  // against the pull.
  if (front == 2 && enabled[0] && enabled[1]) {
    const bool modulating[2] = {channels_[0].phase != Phase::follow,
                                channels_[1].phase != Phase::follow};
    if (modulating[0] != modulating[1]) {
      asymmetry_ = std::min(1.0, asymmetry_ + dt / .8);
      const size_t low = modulating[0] ? 0 : 1, high = 1 - low;
      output_[high] = std::min(output_[high], output_[low] + .1 + .9 * asymmetry_);
    } else if (!modulating[0]) {
      asymmetry_ = 0;
    }
  }
  // Select-low rear axle: both rear wheels take the lower pressure, so the
  // wheel on the slippery side never pulls the car round.
  double rear = std::numeric_limits<double>::infinity();
  for (size_t i = front; i < output_.size(); ++i)
    if (enabled[i] && channels_[i].phase != Phase::follow) rear = std::min(rear, output_[i]);
  if (rear < std::numeric_limits<double>::infinity())
    for (size_t i = front; i < output_.size(); ++i)
      if (enabled[i]) output_[i] = std::min(output_[i], rear);
}

void TractionControl::reset() {
  factor_ = 1;
  integral_ = 0;
  brake_ = brake_integral_ = {};
  active_ = false;
}

double TractionControl::update(double dt, double strength, bool allowed, double reference,
                               const std::array<double, 4>& wheels,
                               const std::array<bool, 4>& driven,
                               const std::array<bool, 4>& contact) {
  const double s = std::clamp(strength, 0.0, 1.0);
  active_ = false;
  const auto relax = [dt](double value, double target, double tau) {
    return value + (target - value) * -std::expm1(-dt / tau);
  };
  if (s <= 0 || !allowed) {
    factor_ = relax(factor_, 1, .1);
    integral_ = 0;
    for (size_t i = 0; i < brake_.size(); ++i) {
      brake_[i] = relax(brake_[i], 0, .03);
      brake_integral_[i] = 0;
    }
    return factor_;
  }
  // Allowed wheelspin: at strength 1, 8 % or 0.8 m/s (what a launch needs),
  // up to 20 % or 2 m/s at the lowest setting.
  const double allowance = std::max(.8 + 1.2 * (1 - s), (.08 + .12 * (1 - s)) * reference);
  std::array<double, 4> excess{};
  std::array<bool, 4> spinning{};
  for (size_t i = 0; i < wheels.size(); ++i) {
    spinning[i] = driven[i] && contact[i];
    excess[i] = spinning[i] ? wheels[i] - reference - allowance : 0;
  }
  // Engine: an open differential feeds each axle the mean of its wheels.
  double engine = -std::numeric_limits<double>::infinity();
  for (size_t axle = 0; axle + 1 < wheels.size(); axle += 2) {
    const int n = spinning[axle] + spinning[axle + 1];
    if (n) engine = std::max(engine, (excess[axle] + excess[axle + 1]) / n);
  }
  if (engine == -std::numeric_limits<double>::infinity()) engine = 0;
  integral_ = std::clamp(integral_ + engine * dt, 0.0, .5);
  const double cut = std::clamp(.25 * std::max(0.0, engine) + 2 * integral_, 0.0, .95);
  factor_ = relax(factor_, 1 - cut, 1 - cut < factor_ ? .015 : .12);
  active_ = engine > 0 || factor_ < .98;
  // Brakes: one wheel of an axle spinning faster than the other, at town
  // speeds.
  for (size_t axle = 0; axle + 1 < wheels.size(); axle += 2) {
    for (size_t side = 0; side < 2; ++side) {
      const size_t i = axle + side, other = axle + 1 - side;
      const double difference = excess[i] - excess[other];
      double target = 0;
      if (spinning[i] && spinning[other] && reference < 22 && excess[i] > 0 && difference > .6) {
        brake_integral_[i] = std::min(.5, brake_integral_[i] + (difference - .6) * dt);
        target = std::clamp(.06 * (difference - .6) + .5 * brake_integral_[i], 0.0, .4);
      } else {
        brake_integral_[i] = std::max(0.0, brake_integral_[i] - dt);
      }
      brake_[i] = relax(brake_[i], target, .03);
      if (brake_[i] > .01) active_ = true;
    }
  }
  return factor_;
}

}  // namespace slrr::physics
