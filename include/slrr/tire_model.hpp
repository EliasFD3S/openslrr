#pragma once

namespace slrr::physics {

// The values a tyre script writes through WheelRef (stock Tyre.java), in the
// units that script uses. Defaults are the stock Baiern 205/55 sport tyre.
struct TireParameters {
  // setFriction: static friction coefficient of the compound, already scaled
  // by the tyre width. Divided by kFrictionUnits to give the peak mu.
  double friction = 1.76863;
  // setStiffness: slope of the force curve at zero slip, per unit slip and
  // per newton of load (the script derives it from profile, rim fit and
  // inflation).
  double stiffness = 17.6839;
  // setSliction: sliding friction over static friction.
  double sliction = .814;
  // setMaxLoad / setLoadSmooth: load capacity and how progressively the
  // tyre saturates towards it. Zero capacity keeps the load linear.
  double maximum_load = 0, load_smoothing = .4;
  // setFrictn_x: longitudinal friction factor.
  double longitudinal_grip = 1;
  // setCPatch: largest camber, in radians, at which the full patch stays on
  // the road. Zero means unknown.
  double patch_angle = 0;
};

struct TireForces {
  double longitudinal, lateral;
};

struct TireTuning {
  float lateral_grip = 1.f;
  float longitudinal_grip = 1.f;
  float lateral_response = 1.f;
  float load_sensitivity = 1.f;
};
TireTuning& tire_tuning();

// Script friction is in the original solver's units: 1.77 for the stock
// sport tyre. This divisor turns it into a road-tyre peak mu (about 1.05).
inline constexpr double kFrictionUnits = 1.68;

// SL2 load blend (FUN_0043ba80): below capacity the load is used as is;
// load_smoothing blends towards an arctangent that saturates at capacity.
double tire_effective_load(double load, const TireParameters& tire);

// camber: angle between the wheel plane and the road normal, in radians.
double tire_camber_factor(double camber, const TireParameters& tire);

// Combined-slip Magic Formula built from the script values: peak
// friction * load / kFrictionUnits, initial slope stiffness * load,
// sliding ratio sliction. Positive longitudinal pushes along the wheel
// heading; lateral opposes lateral_speed.
TireForces tire_forces(double load, double grip_factor, double wheel_speed,
                       double road_speed, double lateral_speed,
                       const TireParameters& tire);

} // namespace slrr::physics
