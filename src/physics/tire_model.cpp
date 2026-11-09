#include "slrr/tire_model.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace slrr::physics {
namespace {
double magic_formula(double slip, double b, double c, double d, double e) {
  const double bx = b * slip;
  return d * std::sin(c * std::atan(bx - e * (bx - std::atan(bx))));
}
} // namespace

TireTuning& tire_tuning() {
  static TireTuning tuning;
  return tuning;
}

// Based on the SLRR_rev Pacejka model, with OpenSLRR grip calibration.
TireForces tire_forces(double load, double alignment, double wheel_speed,
                       double road_speed, double lateral_speed,
                       const TireParameters& tire) {
  constexpr TireParameters stock;
  const double slip_speed = wheel_speed - road_speed;
  const double reference = std::max({2., std::abs(road_speed), std::abs(wheel_speed)});
  const double kappa = slip_speed / reference;
  const double alpha = std::atan2(lateral_speed, std::max(2., std::abs(road_speed)));
  const double grip = std::clamp(tire.friction / stock.friction, .2, 2.5);
  const double stiffness = std::clamp(tire.stiffness / stock.stiffness, .4, 2.5);
  const double load_ratio = tire.maximum_load > 0
      ? tire.maximum_load / std::max(tire.maximum_load, load) : 1;
  const double slide_ratio = std::clamp(tire.sliction / stock.sliction, .3, 1.5);
  const double slide = std::min(1., std::hypot(kappa, alpha) * 2);
  const double effective_grip = grip * load_ratio * std::lerp(1., slide_ratio, slide);

  const double aligned_load = std::max(0., load * alignment);
  const auto& tuning = tire_tuning();
  // The recovered SL2 load blend makes the outside tyre progressively less
  // efficient as the chassis rolls. Keep the existing longitudinal curve.
  double lateral_load_factor = 1.;
  if (tire.maximum_load > 0 && load > 0) {
    const double capped = std::min(load, tire.maximum_load);
    constexpr double half_pi = std::numbers::pi / 2;
    const double progressive = std::atan(load * half_pi / tire.maximum_load) *
                               tire.maximum_load / half_pi;
    lateral_load_factor = std::lerp(1., progressive / capped,
                                    std::clamp(tire.load_smoothing * tuning.load_sensitivity, 0., 1.));
  }
  // Calibration for the host's sprung chassis. The gain still requires
  // normal load at the patch and shares the envelope with drive and brake.
  constexpr double reference_lateral_grip = 1.48;
  const double lateral_peak = aligned_load * reference_lateral_grip *
                              effective_grip * lateral_load_factor * tuning.lateral_grip;
  // Keep more lateral force after the peak so a sliding tyre recovers
  // progressively as the driver unwinds the steering.
  const double lateral_curve = -magic_formula(
      alpha, 30 * stiffness * tuning.lateral_response, 1.15, lateral_peak, .5);
  double lateral = std::copysign(
      std::min(std::abs(lateral_curve), std::abs(lateral_speed) * 4200), lateral_curve);
  // Scale the longitudinal envelope by the game's absolute friction units.
  const double longitudinal_peak =
      aligned_load * .83 * effective_grip * stock.friction *
      std::max(0., tire.longitudinal_grip) * tuning.longitudinal_grip;
  const double longitudinal_curve =
      magic_formula(kappa, 30 * stiffness, 1, longitudinal_peak, .97);
  double longitudinal = std::copysign(
      std::min(std::abs(longitudinal_curve), std::abs(slip_speed) * 3400),
      longitudinal_curve);
  // Both slips share the contact patch. Keep the calibrated pure-slip
  // curves and scale both demands if they exceed the combined envelope.
  if (lateral_peak > 0 && longitudinal_peak > 0) {
    const double demand = std::hypot(longitudinal / longitudinal_peak,
                                     lateral / lateral_peak);
    if (demand > 1) {
      longitudinal /= demand;
      lateral /= demand;
    }
  }
  return {longitudinal, lateral};
}
} // namespace slrr::physics
