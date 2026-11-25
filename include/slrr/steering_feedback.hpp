#pragma once
// Force feedback: the torque the front tyres put into the steering (#395).
//
// A tyre's lateral force acts behind the centre of its contact patch, so it
// turns the wheel back towards its direction of travel: the self-aligning
// torque. Its lever is the pneumatic trail, which shrinks as the tyre slides
// (the wheel goes light past the grip peak), plus the caster's mechanical
// trail, which does not. With the solver's axes (side = forward x up, steer
// positive to the left) the torque on the steering axis is trail x lateral
// force, in the steer's own sign.
//
// The torque is normalised by what the front tyres give at their grip peak
// under the car's resting front load, so a car pulls the same share of the
// wheel's range whatever its weight; the player's strength scales it.
//
// Detail (#399) comes on top: a bump under one front wheel kicks the wheel
// (the two suspensions moving apart), an impact knocks it, and a light
// vibration plays while the front tyres scrub past their peak. Road texture
// goes through the kicks, a torque a wheel can render, not through a
// periodic effect: a sine on every rough road made a belt or gear wheel
// buzz like a pad's rumble motor (#405).
#include "slrr/tire_model.hpp"

#include <array>

namespace slrr::physics {

struct SteeringFeedbackWheel {
  bool contact = false;
  double load = 0;        // tyre load (N), WheelState::force
  double slip_angle = 0;  // relaxed tan(slip angle), WheelState::slip_angle
  double lateral = 0;     // lateral force (N), WheelState::tire_force.lateral
  TireParameters tyre;
  double suspension_speed = 0;  // m/s, WheelState::speed (length rate)
};

struct SteeringFeedbackOutput {
  // Steering torque in logical steer units, -1..1: positive turns the wheel
  // to the left. Zero while nothing steers the front axle.
  float torque = 0;
  // Bump kicks (road feel), same units, scaled by FFB ROAD FEEL.
  float detail = 0;
  // Impact knocks, same units, scaled by FFB IMPACTS.
  float impact = 0;
  // Damper coefficient, 0..1: heavy when parked, light at speed.
  float damper = 0;
  // A vibration on the wheel: amplitude 0..1 and frequency (Hz).
  float vibration = 0, vibration_hz = 0;
};

class SteeringFeedback {
 public:
  // Pneumatic trail at zero slip and the caster trail (m). Only their ratio
  // matters after normalisation: the torque keeps 30 % past the peak.
  static constexpr double kPneumaticTrail = .035, kMechanicalTrail = .015;
  // Below this road speed the slip angle is noise: the torque fades out.
  static constexpr double kFadeSpeed = 3.0;
  // Kick per m/s of the two front suspensions moving apart, past a dead
  // band that drops the solver's chatter, and its limit.
  static constexpr double kBumpGain = .5, kBumpDeadband = .05, kBumpLimit = .35;
  // Low-pass on the suspension rates (s): a kick follows a bump, not a step.
  static constexpr double kBumpFilter = .025;
  // An impact's knock: its limit and decay (s).
  static constexpr double kImpactLimit = .6, kImpactDecay = .05;

  // jolt: a new impact this frame, -1..1 (positive turns the wheel left),
  // zero for none. It decays over about 0.1 s.
  SteeringFeedbackOutput update(const std::array<SteeringFeedbackWheel, 2>& front,
                                double speed, double dt, double jolt = 0);
  void reset() { *this = {}; }

  // Slip angle (rad) where this tyre's lateral force peaks under `load`.
  static double peak_slip_angle(double load, const TireParameters& tyre);
  // trail(alpha): the lever of the lateral force at slip angle alpha (rad).
  static double trail(double alpha, double peak);

 private:
  double reference_load_ = 0;  // resting front load per wheel, smoothed
  float torque_ = 0;
  double jolt_ = 0, jolt_time_ = 0;
  double bump_ = 0;  // filtered right minus left suspension rate (m/s)
};

}  // namespace slrr::physics
