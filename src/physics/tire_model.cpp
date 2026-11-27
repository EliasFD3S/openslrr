#include "slrr/tire_model.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace slrr::physics {
namespace {
// Pacejka Magic Formula tyre (#418), after the MF 5.2 structure: separate
// pure lateral and longitudinal curves, load-dependent friction and cornering
// stiffness, camber thrust and camber-dependent grip, and combined slip by
// normalised slip. The script's WheelRef values set the coefficients:
// - friction (setFriction): the peak friction at the nominal load, 1.15 for
//   the stock tyre, scaled by the ratio to the stock value;
// - sliction (setSliction): the share of the peak a full slide keeps (C);
// - stiffness (setStiffness): the slope at zero slip per unit load at the
//   nominal load (B C D = Fz x stiffness);
// - maximum load (setMaxLoad): the nominal load Fz0 is half of it;
// - load smoothing (setLoadSmooth): how fast the friction falls with load;
// - frictn_x: the longitudinal friction factor.
// The MF coefficients below are typical passenger/sport tyre values
// (OpenSLRR tuning, not recovered from the original exe).
constexpr double kPeakFriction = 1.15;
// E of both curves, the original's setPacejka(8) value.
constexpr double kCurvature = -1;
// Nominal load when the script gives no setMaxLoad (N).
constexpr double kDefaultNominalLoad = 4000;
// Friction change per unit of normalised load change dfz = (Fz - Fz0) / Fz0
// (MF pDy2/pDx2) at the stock load smoothing 0.4; a 20 % heavier wheel grips
// about 2.4 % less per newton.
constexpr double kLoadSensitivity = -.12;
constexpr double kStockLoadSmoothing = .4;
// Cornering stiffness against load (MF pKy2): Kya = pKy1 Fz0 sin(2 atan(Fz /
// (pKy2 Fz0))), normalised so that the slope per unit load is the script's
// stiffness at Fz0. It saturates: twice the nominal load gives 0.625 x the
// slope per newton.
constexpr double kStiffnessLoad = 2;
// Camber, per radian of inclination against the road (MF pDy3, pHy3, pVy3).
// The total camber stiffness is about one load per radian (about 70 N/deg at
// 4 kN), shared between a slip-angle shift and a force shift; the friction
// falls with the square of the inclination.
constexpr double kCamberFriction = 3;
constexpr double kCamberShift = .6;  // camber stiffness share as a slip shift
constexpr double kCamberThrust = .4; // and as a force shift, per load
constexpr double kCamberStiffness = 1;
// The script's stiffness enters as a slope around the stock value; above it,
// the square root keeps a low-profile tyre's clamp of 40 from peaking at
// 4 degrees (stock tyre 17.68: unchanged; 40: 26.6).
constexpr double kStockStiffness = 17.6839;
// Pneumatic trail (#458), after MF 5.2's Dt0 = Fz (R0 / Fz0) (qDz1 + qDz2
// dfz), t = Dt cos(Ct atan(Bt alpha)) cos(alpha). 0.09 R0 is 2.8 cm on the
// stock 0.31 m wheel at its nominal load, growing with the load like the
// patch. Bt puts the trail's zero at 1.5 times the lateral peak's slip
// (about 10 per radian on the stock tyre, MF's typical qBz1), so the torque
// peaks at about half the peak slip; past it the trail turns negative, as a
// real tyre's does (-0.2 Dt at 20 degrees with Ct 1.4).
constexpr double kTrailLoad = .09, kTrailLoadChange = -.01, kTrailShape = 1.4;
constexpr double kTrailZero = 1.5;
constexpr double kDefaultRadius = .31;
// The trail scales with the patch length, which the pressure sets; the host
// gets the pressure only through the patched Tyre script's relaxation length
// (0.3 sqrt(profile height / (113 mm x pressure / optimum))), 0.3 m for a
// 205/55 at its optimum and without the patch.
constexpr double kReferenceRelaxation = .3;
// The patch's lateral offset under a lateral force: the carcass' lateral
// stiffness is the cornering stiffness over the relaxation length
// (stretched string, sigma = C_alpha / C_y), about 200 kN/m on the stock
// tyre. Capped at 5 cm.
constexpr double kMaximumOffset = .05;

double magic_formula(double slip, double b, double c, double d, double e) {
  const double bx = b * slip;
  return d * std::sin(c * std::atan(bx - e * (bx - std::atan(bx))));
}

// The slip of the curve's peak: C atan(Bx - E(Bx - atan Bx)) = pi/2.
double peak_slip(double b, double c, double e) {
  if (!(c > 1) || !(b > 0)) return 1e3;  // no peak: the curve only rises
  const double target = std::tan(std::numbers::pi / (2 * c));
  double lo = 0, hi = 50;
  for (int i = 0; i < 40; ++i) {
    const double x = .5 * (lo + hi);
    (x - e * (x - std::atan(x)) < target ? lo : hi) = x;
  }
  return .5 * (lo + hi) / b;
}

double nominal_load(const TireParameters& tire) {
  return tire.maximum_load > 0 ? .5 * tire.maximum_load : kDefaultNominalLoad;
}

double peak_friction(const TireParameters& tire) {
  constexpr TireParameters stock;
  return kPeakFriction * std::clamp(tire.friction / stock.friction, 0.2, 2.5);
}

// C such that the curve ends at `slide` times its peak (1: no drop).
double shape(const TireParameters& tire) {
  const double slide = std::clamp(tire.sliction, 0.4, 1.0);
  return 2 - 2 / std::numbers::pi * std::asin(slide);
}

double script_stiffness(const TireParameters& tire) {
  const double s = std::clamp(tire.stiffness, 4.0, 60.0);
  return s <= kStockStiffness ? s : std::sqrt(s * kStockStiffness);
}

// Everything the curves need at one load and camber.
struct Curves {
  double fz = 0, dfz = 0;
  double mu_y = 0, mu_x = 0, c = 0;
  double by = 0, bx = 0, dy = 0, dx = 0;
  double shift = 0, thrust = 0;  // camber: slip-angle shift, force shift
  double alpha_peak = 0, kappa_peak = 0;
  double trail = 0;  // Dt (m)
  double kya = 0;    // cornering stiffness (N/rad)
};

Curves curves(double load, double camber, const TireParameters& tire) {
  const auto& tuning = tire_tuning();
  Curves k;
  k.fz = load;
  const double fz0 = nominal_load(tire);
  k.dfz = std::clamp((load - fz0) / fz0, -1.0, 3.0);
  const double sensitivity = kLoadSensitivity *
      std::clamp(tire.load_smoothing / kStockLoadSmoothing, 0.0, 2.5) *
      std::max(0.0, double(tuning.load_sensitivity));
  const double gamma = std::clamp(camber, -1.2, 1.2);
  const double camber_grip = std::max(.25, 1 - kCamberFriction * gamma * gamma);
  const double load_grip = std::max(.5, 1 + sensitivity * k.dfz);
  const double mu = peak_friction(tire) * load_grip * camber_grip;
  k.mu_y = mu;
  k.mu_x = mu * std::max(0.0, tire.longitudinal_grip);
  k.c = shape(tire);
  k.dy = load * k.mu_y;
  k.dx = load * k.mu_x;

  // Slope per unit load at Fz0 is the script's stiffness; it saturates with load.
  const double slope = script_stiffness(tire) * std::max(0.1, double(tuning.lateral_response));
  const double norm = std::sin(2 * std::atan(1 / kStiffnessLoad));
  // The same slope serves both directions, as in the original's single curve.
  const double kya = slope * fz0 * std::sin(2 * std::atan(load / (kStiffnessLoad * fz0))) / norm;
  k.by = k.dy > 0 ? kya / (k.c * k.dy) : 0;
  k.bx = k.dx > 0 ? kya / (k.c * k.dx) : 0;
  k.alpha_peak = peak_slip(k.by, k.c, kCurvature);
  k.kappa_peak = peak_slip(k.bx, k.c, kCurvature);
  const double radius = tire.radius > 0 ? tire.radius : kDefaultRadius;
  const double relaxation = std::clamp(tire.relaxation_length, .05, 2.0);
  k.trail = std::clamp(radius * (load / fz0) * (kTrailLoad + kTrailLoadChange * k.dfz), 0.0,
                       .2 * radius) *
            std::clamp(relaxation / kReferenceRelaxation, .5, 1.5);
  k.kya = kya;

  // Camber thrust towards the side the tyre's top leans to.
  const double camber_force = kCamberStiffness * load * gamma;
  k.shift = kya > 0 ? kCamberShift * camber_force / kya : 0;
  k.thrust = kCamberThrust * camber_force * camber_grip;
  return k;
}
} // namespace

double relaxation_load_factor(double load, const TireParameters& tire) {
  const double fz0 = nominal_load(tire);
  if (!(fz0 > 0) || !(load > 0)) return .3;
  return std::clamp(std::sin(2 * std::atan(load / (kStiffnessLoad * fz0))) /
                        std::sin(2 * std::atan(1 / kStiffnessLoad)),
                    .3, 1.5);
}

TireTuning &tire_tuning() {
  static TireTuning tuning;
  return tuning;
}

TireForces tire_friction_limits(double load, double alignment,
                                const TireParameters& tire, double camber) {
  const double aligned_load = std::max(0.0, load * alignment);
  if (aligned_load <= 0) return {};
  const auto& tuning = tire_tuning();
  const Curves k = curves(aligned_load, camber, tire);
  return {k.dx * std::max(0.0, double(tuning.longitudinal_grip)),
          k.dy * std::max(0.0, double(tuning.lateral_grip))};
}

TireForces tire_forces(double load, double alignment, double wheel_speed,
                       double road_speed, double lateral_speed,
                       const TireParameters &tire, double camber,
                       double sliding_speed, double slipping_speed) {
  const double aligned_load = std::max(0.0, load * alignment);
  if (aligned_load <= 0.0)
    return {0.0, 0.0};
  const auto& tuning = tire_tuning();

  // Slip ratio and angle against the road speed; below 1.5 m/s that floor
  // keeps them finite, and the low-speed caps below take over.
  const double slip_speed = wheel_speed - road_speed;
  const double reference = std::max(1.5, std::abs(road_speed));
  const double kappa = slip_speed / reference;
  const double alpha = std::atan2(lateral_speed, reference);

  Curves k = curves(aligned_load, camber, tire);
  // Camber thrust needs a rolling tyre: it fades in over the first 3 m/s so a
  // parked car is not pushed sideways.
  const double rolling = std::clamp(std::abs(road_speed) / 3, 0.0, 1.0);
  k.shift *= rolling;
  k.thrust *= rolling;
  // Pure-slip lateral curve on the camber-shifted slip angle (positive slip
  // angle: the force acts along -lateral_speed); the camber force shift adds
  // its share in proportion to how much of the grip is used sideways.
  const double alpha_y = alpha - k.shift;

  // Combined slip by normalised slip: each slip is measured against its own
  // peak, the resultant runs along each pure curve and splits by direction.
  const double sy = std::tan(alpha_y) / std::tan(std::min(1.4, k.alpha_peak));
  const double sx = kappa / k.kappa_peak;
  const double rho = std::hypot(sx, sy);
  double lateral = 0, longitudinal = 0, trail = 0;
  if (rho > 1e-12) {
    // The combined slip as one equivalent slip angle on the lateral curve.
    const double alpha_eq = std::atan(rho * std::tan(std::min(1.4, k.alpha_peak)));
    const double fy = magic_formula(alpha_eq, k.by, k.c, k.dy, kCurvature);
    const double fx = magic_formula(rho * k.kappa_peak, k.bx, k.c, k.dx, kCurvature);
    lateral = -fy * sy / rho;
    longitudinal = fx * sx / rho;
    const double bt = std::tan(std::numbers::pi / (2 * kTrailShape)) /
                      std::max(1e-3, kTrailZero * std::min(1.4, k.alpha_peak));
    trail = k.trail * std::cos(kTrailShape * std::atan(bt * alpha_eq)) * std::cos(alpha);
  } else {
    trail = k.trail;
  }
  // The slip force's share of the lateral force acts behind the patch; the
  // camber thrust, at its centre, adds no moment.
  const double slip_lateral = lateral * tuning.lateral_grip;
  // Camber thrust fades as wheelspin or locking takes the grip.
  const double share = rho > 1e-12 ? std::abs(sy) / rho : 1;
  lateral += k.thrust * share;
  lateral *= tuning.lateral_grip;
  longitudinal *= tuning.longitudinal_grip;

  // Low speed: the force grows with the sliding speed, not the slip, so a
  // car at rest is not pushed around by a finite slip of nearly nothing.
  // That is the patch's actual sliding speed, not the relaxed one: bounded by
  // the lagged value, a car that stops sliding was still pushed for about
  // 0.2 s (relaxation / 1.5 m/s) and swung back and forth like a spring.
  // Below 3 m/s the tyre also never pushes along the way it slides.
  // The camber thrust is part of the sideways force and capped the same way.
  // The cap per m/s of sliding grows with the road speed above 2 m/s: a
  // fixed cap would also bound the cornering stiffness at speed (0.95 x
  // 16.7 m/s = 16 per radian at 60 km/h, softer than the stock tyre; #418).
  const double cap_gain = std::max(1.0, reference / 2);
  const double sliding = std::isnan(sliding_speed) ? lateral_speed : sliding_speed;
  const double lateral_cap = (std::abs(sliding) + std::abs(k.shift) * reference) *
                             aligned_load * 0.95 * cap_gain;
  const double uncapped = lateral;
  lateral = std::copysign(std::min(std::abs(lateral), lateral_cap), lateral);
  if (lateral * sliding > 0) lateral *= rolling;
  const double slipping = std::isnan(slipping_speed) ? slip_speed : slipping_speed;
  const double longitudinal_cap = std::abs(slipping) * aligned_load * 0.85 * cap_gain;
  longitudinal = std::copysign(std::min(std::abs(longitudinal), longitudinal_cap), longitudinal);
  // The low-speed caps scale the moment with the force.
  const double kept = std::abs(uncapped) > 1e-12 ? lateral / uncapped : 1;
  // The lateral force pulls the patch its way; the longitudinal force,
  // acting there, adds offset x Fx (#458): braking in a turn turns the wheel
  // into the turn and lightens the steering, traction adds to it.
  // The carcass' lateral compliance (relaxation / cornering stiffness)
  // does not change with the load: the length grows with the stiffness
  // (#498), so a light wheel's patch is not pushed further than a loaded
  // one's by the same force.
  const double relaxation = std::clamp(tire.relaxation_length * relaxation_load_factor(aligned_load, tire), .05, 2.0);
  const double offset =
      k.kya > 0 ? std::clamp(lateral * relaxation / k.kya, -kMaximumOffset, kMaximumOffset) : 0;
  return {longitudinal, lateral, trail * slip_lateral * kept + offset * longitudinal, offset};
}

} // namespace slrr::physics
