#include "slrr/tire_model.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace slrr::physics {

TireTuning& tire_tuning() {
  static TireTuning tuning;
  return tuning;
}

double tire_effective_load(double load, const TireParameters& tire) {
  load = std::max(0., load);
  if (!(tire.maximum_load > 0)) return load;
  const double smoothing =
      std::clamp(tire.load_smoothing * tire_tuning().load_sensitivity, 0., 1.);
  constexpr double half_pi = std::numbers::pi / 2;
  const double progressive =
      std::atan(load * half_pi / tire.maximum_load) * tire.maximum_load / half_pi;
  return std::lerp(std::min(load, tire.maximum_load), progressive, smoothing);
}

double tire_camber_factor(double camber, const TireParameters& tire) {
  camber = std::abs(camber);
  if (!(tire.patch_angle > 0)) {
    // No contact-patch data: the previous tilt-squared fall-off.
    const double tilt = std::cos(camber);
    return std::max(.25, tilt * tilt);
  }
  // Within the scripted patch angle the whole tread stays on the road. Past
  // it the tyre rolls onto its shoulder and the patch shrinks.
  const double excess = std::max(0., camber - tire.patch_angle);
  return std::max(.25, std::cos(std::min(std::numbers::pi / 2, 2 * excess)));
}

// Magic Formula with the coefficients taken from the tyre script:
//   D = mu * load, B * C * D = stiffness * load, sin(C * pi / 2) = sliction.
// Slip is combined (|kappa, tan alpha|) so that wheelspin or locking uses up
// cornering grip and a sliding tyre loses force along both axes.
TireForces tire_forces(double load, double grip_factor, double wheel_speed,
                       double road_speed, double lateral_speed,
                       const TireParameters& tire) {
  const double effective = tire_effective_load(load, tire) * std::max(0., grip_factor);
  if (!(effective > 0)) return {0, 0};
  const auto& tuning = tire_tuning();
  // Below walking pace the slip ratios are ill-conditioned; this floor turns
  // the curve's initial slope into a bounded viscous response there.
  constexpr double speed_floor = 2;
  const double slip_speed = wheel_speed - road_speed;
  const double kappa =
      slip_speed / std::max({speed_floor, std::abs(road_speed), std::abs(wheel_speed)});
  const double tan_alpha = lateral_speed / std::max(speed_floor, std::abs(road_speed));
  const double slip = std::hypot(kappa, tan_alpha);
  if (slip < 1e-9) return {0, 0};

  const double mu = std::max(.05, tire.friction / kFrictionUnits);
  const double sliding = std::clamp(tire.sliction, .3, 1.);
  const double shape = 2 - 2 / std::numbers::pi * std::asin(sliding);
  const double stiffness = std::max(1., tire.stiffness);
  const double b = stiffness / (shape * mu);
  const double magnitude = std::sin(shape * std::atan(b * slip)) * effective * mu;

  const double longitudinal = magnitude * kappa / slip *
                              std::max(0., tire.longitudinal_grip) * tuning.longitudinal_grip;
  // Lateral response stiffens or softens the cornering slope only.
  const double lateral_magnitude =
      tuning.lateral_response == 1.f
          ? magnitude
          : std::sin(shape * std::atan(b * tuning.lateral_response * slip)) * effective * mu;
  const double lateral = -lateral_magnitude * tan_alpha / slip * tuning.lateral_grip;
  return {longitudinal, lateral};
}
} // namespace slrr::physics
