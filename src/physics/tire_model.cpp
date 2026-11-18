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

TireTuning &tire_tuning() {
  static TireTuning tuning;
  return tuning;
}

TireForces tire_forces(double load, double alignment, double wheel_speed,
                       double road_speed, double lateral_speed,
                       const TireParameters &tire) {
  constexpr TireParameters stock;
  const double aligned_load = std::max(0.0, load * alignment);
  if (aligned_load <= 0.0)
    return {0.0, 0.0};

  const double slip_speed = wheel_speed - road_speed;
  const double reference =
      std::max({1.5, std::abs(road_speed), std::abs(wheel_speed)});
  const double kappa = slip_speed / reference;
  const double alpha =
      std::atan2(lateral_speed, std::max(1.5, std::abs(road_speed)));

  const double grip = std::clamp(tire.friction / stock.friction, 0.2, 2.5);
  const double stiffness =
      std::clamp(tire.stiffness / stock.stiffness, 0.4, 2.5);
  const double slide_ratio =
      std::clamp(tire.sliction / stock.sliction, 0.3, 1.5);

  // Sensibilité à la charge progressive et continue (rendement décroissant
  // lors des transferts de masse sans plafond dur à maximum_load).
  double load_efficiency = 1.0;
  if (tire.maximum_load > 0.0) {
    const double normalized_load = aligned_load / tire.maximum_load;
    load_efficiency =
        1.0 / (1.0 + 0.18 * std::max(0.0, normalized_load - 0.5));
  }

  const double slide = std::min(1.0, std::hypot(kappa * 1.5, alpha * 2.5));
  const double effective_grip =
      grip * load_efficiency * std::lerp(1.0, slide_ratio, slide);

  const auto &tuning = tire_tuning();
  double lateral_load_factor = 1.0;
  if (tire.maximum_load > 0.0) {
    const double capped = std::min(aligned_load, tire.maximum_load);
    constexpr double half_pi = std::numbers::pi / 2.0;
    const double progressive =
        std::atan(aligned_load * half_pi / tire.maximum_load) *
        tire.maximum_load / half_pi;
    lateral_load_factor =
        std::lerp(1.0, progressive / std::max(1.0, capped),
                  std::clamp(tire.load_smoothing * tuning.load_sensitivity,
                             0.0, 1.0));
  }

  // Pic latéral : rigidité de dérive assouplie (B = 11.5) pour placer le pic
  // vers 7-9° et garder une dérive contrôlable après décrochage.
  constexpr double reference_lateral_grip = 1.42;
  const double lateral_peak = aligned_load * reference_lateral_grip *
                              effective_grip * lateral_load_factor *
                              tuning.lateral_grip;

  // Slip combiné : le patinage longitudinal (kappa) réduit la rigidité
  // de dérive avant même d'atteindre la limite de l'ellipse de friction.
  const double longitudinal_saturation =
      std::clamp(std::abs(kappa) / 0.35, 0.0, 0.75);
  const double effective_lateral_b =
      11.5 * stiffness * tuning.lateral_response *
      (1.0 - 0.55 * longitudinal_saturation);

  const double lateral_curve =
      -magic_formula(alpha, effective_lateral_b, 1.32, lateral_peak, 0.35);

  // Amortissement basse vitesse proportionnel à la charge verticale sur la roue
  const double low_speed_lateral_cap = aligned_load * 0.95;
  double lateral = std::copysign(
      std::min(std::abs(lateral_curve),
               std::abs(lateral_speed) * low_speed_lateral_cap),
      lateral_curve);

  // Pic longitudinal : C = 1.55 crée une vraie chute de traction quand la roue
  // bloque ou patine excessivement par rapport au slip optimal (~10-12%).
  const double longitudinal_peak =
      aligned_load * 0.85 * effective_grip * stock.friction *
      std::max(0.0, tire.longitudinal_grip) * tuning.longitudinal_grip;

  const double longitudinal_curve =
      magic_formula(kappa, 14.0 * stiffness, 1.55, longitudinal_peak, 0.45);

  const double low_speed_longitudinal_cap = aligned_load * 0.85;
  double longitudinal = std::copysign(
      std::min(std::abs(longitudinal_curve),
               std::abs(slip_speed) * low_speed_longitudinal_cap),
      longitudinal_curve);

  // Ellipse de friction combinée
  if (lateral_peak > 0.0 && longitudinal_peak > 0.0) {
    const double demand = std::hypot(longitudinal / longitudinal_peak,
                                     lateral / lateral_peak);
    if (demand > 1.0) {
      longitudinal /= demand;
      lateral /= demand;
    }
  }

  return {longitudinal, lateral};
}

} // namespace slrr::physics