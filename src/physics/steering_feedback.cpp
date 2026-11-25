#include "slrr/steering_feedback.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace slrr::physics {
namespace {
constexpr double kProbeSpeed = 20;  // m/s; the curve is in slip, not speed
constexpr double kMaxProbe = .6;    // tan(slip angle), about 31 degrees
constexpr int kProbes = 120;

double lateral_force(double load, double tan_alpha, const TireParameters& tyre) {
  return std::abs(tire_forces(load, 1, kProbeSpeed, kProbeSpeed, kProbeSpeed * tan_alpha, tyre)
                      .lateral);
}
}  // namespace

double SteeringFeedback::peak_slip_angle(double load, const TireParameters& tyre) {
  if (!(load > 0)) return .14;
  double best = 0, best_tan = .14;
  for (int i = 1; i <= kProbes; ++i) {
    const double t = kMaxProbe * i / kProbes;
    const double f = lateral_force(load, t, tyre);
    if (f > best) {
      best = f;
      best_tan = t;
    }
  }
  return std::atan(best_tan);
}

double SteeringFeedback::trail(double alpha, double peak) {
  // The pneumatic trail is gone once the whole patch slides, about half again
  // past the force peak: the aligning torque peaks before the grip does.
  const double pneumatic = kPneumaticTrail * std::max(0.0, 1 - std::abs(alpha) / (1.5 * peak));
  return pneumatic + kMechanicalTrail;
}

SteeringFeedbackOutput SteeringFeedback::update(const std::array<SteeringFeedbackWheel, 2>& front,
                                                double speed, double dt, double jolt) {
  SteeringFeedbackOutput out;
  speed = std::abs(speed);
  out.damper = static_cast<float>(.1 + .5 * std::exp(-speed / 4));
  const double fade = std::clamp(speed / kFadeSpeed, 0.0, 1.0);
  double moment = 0, load = 0, excess = 0;
  int touching = 0;
  const TireParameters* tyre = nullptr;
  for (const auto& w : front) {
    if (!w.contact || !(w.load > 0)) continue;
    const double peak = peak_slip_angle(w.load, w.tyre);
    const double alpha = std::atan(std::abs(w.slip_angle));
    moment += trail(alpha, peak) * w.lateral;
    excess = std::max(excess, alpha / peak - 1);
    load += w.load;
    ++touching;
    tyre = &w.tyre;
  }
  double target = 0;
  if (touching) {
    const double per_wheel = load / touching;
    // The resting front load, not this instant's: a bump or a brake dive
    // must change the torque, not its scale.
    if (reference_load_ <= 0) reference_load_ = per_wheel;
    else reference_load_ += (per_wheel - reference_load_) * (1 - std::exp(-dt / 3.0));
    const double peak = peak_slip_angle(reference_load_, *tyre);
    double full = 0;
    for (int i = 1; i <= kProbes; ++i) {
      const double t = kMaxProbe * i / kProbes;
      full = std::max(full, trail(std::atan(t), peak) * lateral_force(reference_load_, t, *tyre));
    }
    if (full > 0) target = std::clamp(moment / (2 * full) * fade, -1.0, 1.0);
  }
  // A light low-pass: the solver's relaxation already smooths the slip.
  const float alpha = static_cast<float>(1 - std::exp(-dt / .02));
  torque_ += (static_cast<float>(target) - torque_) * alpha;
  out.torque = torque_;

  // Detail. Index 0 is the left wheel (the solver's side axis points right).
  // A bump under the left wheel shortens its suspension and drags that
  // wheel back: the steering turns left (positive), and the other way round.
  const double moving = std::clamp(speed / 5, 0.0, 1.0);
  double kick = 0;
  if (front[0].contact && front[1].contact)
    kick = std::clamp(kBumpGain * (front[1].suspension_speed - front[0].suspension_speed),
                      -kBumpLimit, kBumpLimit) * moving;
  // An impact rings the wheel at 12 Hz and dies out in about 0.1 s.
  if (std::abs(jolt) > std::abs(jolt_) * std::exp(-jolt_time_ / .06)) {
    jolt_ = std::clamp(jolt, -1.0, 1.0);
    jolt_time_ = 0;
  } else {
    jolt_time_ += dt;
  }
  const double ring = jolt_ * std::exp(-jolt_time_ / .06) *
                      std::cos(2 * std::numbers::pi * 12 * jolt_time_);
  out.detail = static_cast<float>(std::clamp(kick + ring, -1.0, 1.0));

  // Vibration: the front tyres scrubbing past their peak, or a rough road.
  const double activity = touching ? std::abs(front[0].suspension_speed) +
                                         std::abs(front[1].suspension_speed)
                                   : 0;
  road_ += (activity - road_) * (1 - std::exp(-dt / .1));
  const double scrub = std::clamp(excess * .4, 0.0, .3) * fade;
  const double rough = std::clamp(road_ * .5, 0.0, .3) * moving;
  if (scrub >= rough) {
    out.vibration = static_cast<float>(scrub);
    out.vibration_hz = static_cast<float>(20 + 10 * std::min(excess, 1.0));
  } else {
    out.vibration = static_cast<float>(rough);
    out.vibration_hz = static_cast<float>(std::min(60.0, 35 + speed * .3));
  }
  return out;
}

}  // namespace slrr::physics
