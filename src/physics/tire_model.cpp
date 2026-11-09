#include "slrr/tire_model.hpp"

#include <algorithm>
#include <cmath>

namespace slrr::physics {
namespace {
double magic_formula(double slip, double b, double c, double d, double e) {
  const double bx = b * slip;
  return d * std::sin(c * std::atan(bx - e * (bx - std::atan(bx))));
}
} // namespace

// SLRR_rev aa27742, src/engine/tire-model.ts, default Pacejka mode.
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
  const double lateral_peak = aligned_load * 1.2 * effective_grip;
  const double lateral_curve = -magic_formula(alpha, 30 * stiffness, 1.2, lateral_peak, .5);
  const double lateral = std::copysign(
      std::min(std::abs(lateral_curve), std::abs(lateral_speed) * 4200), lateral_curve);
  const double lateral_use = lateral_peak > 0 ? std::min(1., std::abs(lateral) / lateral_peak) : 0;
  // Scale the longitudinal envelope by the game's absolute friction units.
  const double longitudinal_peak =
      aligned_load * .83 * effective_grip * stock.friction * tire.longitudinal_grip;
  const double longitudinal_curve = magic_formula(kappa, 30 * stiffness, 1, longitudinal_peak, .97)
                                  * std::sqrt(std::max(0., 1 - lateral_use * lateral_use));
  return {std::copysign(std::min(std::abs(longitudinal_curve), std::abs(slip_speed) * 3400),
                        longitudinal_curve), lateral};
}
} // namespace slrr::physics
