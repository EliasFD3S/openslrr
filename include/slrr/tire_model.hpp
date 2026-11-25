#pragma once

namespace slrr::physics {

// The values a tyre script writes through WheelRef (stock Tyre.java), in the
// units that script uses. Defaults are the stock Baiern 205/55 sport tyre.
struct TireParameters {
  // setFriction: compound/width input. The peak friction is 1.15 times its
  // ratio to this stock value (#383).
  double friction = 1.76863;
  // setStiffness: the curve's slope at zero slip per unit load (B C D =
  // load x stiffness); the script derives it from profile, rim fit and
  // inflation.
  double stiffness = 17.6839;
  // setSliction: slide/grip ratio, the share of the peak a full slide keeps.
  double sliction = .814;
  // setMaxLoad / setLoadSmooth: the nominal load is half of maximum_load
  // (4000 N when zero); load_smoothing sets how fast friction falls with load.
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

// Peak force along and across the tyre (N): the curves' D, with the load
// sensitivity, camber and host grip tuning. Static holding uses these bounds
// without requiring motion.
TireForces tire_friction_limits(double load, double alignment,
                                const TireParameters& tire, double camber = 0);

// Pacejka Magic Formula tyre (#418): pure lateral and longitudinal curves
// with load-dependent friction and stiffness, camber, and combined slip by
// normalised slip, shaped by the script inputs and host tuning. Positive
// longitudinal pushes along the wheel heading; lateral opposes lateral_speed.
// alignment scales the load before the curves are evaluated. camber is the
// tyre's inclination against the road in radians, positive when its top
// leans towards +lateral (the thrust then pushes towards +lateral).
TireForces tire_forces(double load, double alignment, double wheel_speed,
                       double road_speed, double lateral_speed,
                       const TireParameters& tire, double camber = 0);

} // namespace slrr::physics
