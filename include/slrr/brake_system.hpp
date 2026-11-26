#pragma once
// Brakes as hardware (#446): rotor temperature and pad friction, and the two
// driving aids that work through them, ABS and ASR.
//
// The patched DiscBrake script sends each wheel its maximum torque at full
// pedal (100 bar line pressure x piston area x 2 pad faces x the pad's
// nominal friction x the effective disc radius) and, through
// WheelRef.setBrakeSpec, its pad compound, rotor type and diameter. The host
// turns the last three into a rotor heat capacity, a cooling law and a pad
// friction curve against temperature. Without the patched scripts no wheel
// has a spec: the brakes stay cold and keep their scripted torque.
//
// ABS and ASR only see what a real controller sees: the four wheel speeds,
// the longitudinal accelerometer and the pedals. Neither reads the true
// ground speed at a tyre.
#include <array>
#include <cstddef>

namespace slrr::physics {

enum class PadCompound { street = 0, fast_road = 1, race = 2 };
enum class RotorType { solid = 0, grooved = 1, vented = 2, carbon = 3 };

struct BrakeHardware {
  bool specified = false;  // false: no thermal model, friction factor 1
  PadCompound pad = PadCompound::street;
  RotorType rotor = RotorType::solid;
  double diameter = .28;  // m
};

// Hardware from the setBrakeSpec arguments (pad 0-2, rotor 0-3, diameter m).
// Out-of-range values are clamped; a non-positive diameter leaves the
// hardware unspecified.
BrakeHardware brake_hardware(double pad, double rotor, double diameter);

constexpr double kAmbientTemperature = 25;  // deg C

// Heat capacity of the rotor (and the share of the pads and hub that heats
// with it), J/K.
double rotor_heat_capacity(const BrakeHardware& brake);
// Heat the rotor gives off at `temperature` (deg C) with the car moving at
// `speed` (m/s), W: forced convection over its faces (and vanes) plus
// radiation.
double rotor_cooling(const BrakeHardware& brake, double speed, double temperature);
// Pad friction at `temperature` relative to its nominal (working) value,
// which the scripted torque already contains: below 1 when cold and when
// fading. Carbon rotors move the fade to higher temperatures.
double pad_friction_factor(const BrakeHardware& brake, double temperature);

// One substep's rotor temperature: `heat` J of brake work in, cooling out.
double step_rotor_temperature(const BrakeHardware& brake, double temperature, double heat,
                              double speed, double dt);

// Brake fluid in the caliper (deg C): it follows 45 % of the rotor's rise
// above ambient with a 40 s lag. DOT 4 boils at 230 deg C: past it the
// vapour takes up pedal travel and the pressure that reaches the pads falls
// (to 0.4 at 290 deg C). The fluid cools back and the brakes recover.
double step_fluid_temperature(const BrakeHardware& brake, double fluid, double rotor, double dt);
double fluid_pressure_factor(const BrakeHardware& brake, double fluid);

// Reference speed (m/s, along the direction of travel) as an ABS/ASR
// controller estimates it: the accelerometer integrated between samples,
// pulled to the fastest wheel while braking (the wheels can only be slower
// than the car) and to the slowest trusted wheel while driving.
class ReferenceSpeed {
 public:
  void reset(double speed = 0) {
    speed_ = speed;
    stalled_ = last_fastest_ = 0;
  }
  // `wheels` are wheel circumferential speeds along the direction of travel;
  // `trusted` marks the wheels to pull towards while driving (the undriven
  // ones, or all of them on a four-wheel drive). `contact` excludes wheels in
  // the air.
  double update(double dt, double acceleration, bool braking,
                const std::array<double, 4>& wheels, const std::array<bool, 4>& contact,
                const std::array<bool, 4>& trusted);
  double speed() const { return speed_; }

 private:
  double speed_ = 0, stalled_ = 0, last_fastest_ = 0;
};

// The per-wheel brake pressure of an ABS hydraulic unit, as a fraction of
// the pressure the pedal gives at full travel. Each channel follows the
// driver's pressure until its wheel decelerates too hard or slips past the
// threshold, then dumps, holds while the wheel spins back up and rebuilds
// in steps, a few cycles a second. The rear axle is select-low.
class AbsModulator {
 public:
  enum class Phase { follow, dump, hold, rebuild };
  struct Channel {
    Phase phase = Phase::follow;
    double pressure = 0, lock_pressure = 0, timer = 0;
  };
  void reset();
  // driver: the pedal's pressure (fraction). strength 0 turns ABS off.
  // `wheels` circumferential speeds and `acceleration` their rates (m/s²),
  // along the direction of travel. front: number of front wheels.
  void update(double dt, double driver, double strength, double reference,
              double vehicle_deceleration, const std::array<double, 4>& wheels,
              const std::array<double, 4>& wheel_acceleration,
              const std::array<bool, 4>& enabled, const std::array<bool, 4>& contact,
              std::size_t front);
  double pressure(std::size_t i) const { return output_[i]; }
  const Channel& channel(std::size_t i) const { return channels_[i]; }
  bool active() const;  // any channel modulating

 private:
  std::array<Channel, 4> channels_{};
  std::array<double, 4> output_{};
  double asymmetry_ = 0;  // 0-1: time since one front wheel alone modulates
};

// Traction control. Cuts the engine torque when the driven wheels spin past
// the target slip, at any speed and in any gear, and brakes a single
// spinning driven wheel (an open differential then sends the torque to the
// other one).
class TractionControl {
 public:
  void reset();
  // Returns the factor (0-1) for the accelerator. `driven` marks the wheels
  // with drive weight. strength 0 turns ASR off. Pairs (0,1) and (2,3) are
  // the axles.
  double update(double dt, double strength, bool allowed, double reference,
                const std::array<double, 4>& wheels, const std::array<bool, 4>& driven,
                const std::array<bool, 4>& contact);
  // Brake pressure (fraction) the ASR asks for on wheel i.
  double brake(std::size_t i) const { return brake_[i]; }
  double torque_factor() const { return factor_; }
  bool active() const { return active_; }

 private:
  double factor_ = 1, integral_ = 0;
  std::array<double, 4> brake_{}, brake_integral_{};
  bool active_ = false;
};

}  // namespace slrr::physics
