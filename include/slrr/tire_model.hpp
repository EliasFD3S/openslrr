#pragma once

namespace slrr::physics {

// The values a tyre script writes through WheelRef (stock Tyre.java), in the
// units that script uses. Defaults are the stock Baiern 205/55 sport tyre.
struct TireParameters {
  // setFriction: compound/width input, divided by the reference tyre's
  // default and clamped to scale grip; not itself the resulting peak mu.
  double friction = 1.76863;
  // setStiffness: reference-scaled Magic Formula B multiplier (the script
  // derives it from profile, rim fit and inflation), not an exact slope.
  double stiffness = 17.6839;
  // setSliction: reference-scaled grip multiplier as combined slip grows.
  double sliction = .814;
  // setMaxLoad / setLoadSmooth: reference load and lateral smoothing.
  // Load efficiency decreases above half capacity; zero keeps load linear.
  double maximum_load = 0, load_smoothing = .4;
  // setFrictn_x: longitudinal friction factor.
  double longitudinal_grip = 1;
  // Stored setCPatch tolerance in radians. Not consumed by the 8ac2ba9
  // force law; alignment is supplied separately as a load factor.
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

// Reference Magic Formula curves, scaled by the script inputs and host
// tuning, with progressive load efficiency and a combined friction ellipse.
// Positive longitudinal pushes along the wheel heading; lateral opposes
// lateral_speed. alignment scales the load before the curve is evaluated.
TireForces tire_forces(double load, double alignment, double wheel_speed,
                       double road_speed, double lateral_speed,
                       const TireParameters& tire);

} // namespace slrr::physics
