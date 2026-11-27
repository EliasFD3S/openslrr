#pragma once

#include <limits>

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
  // WheelRef.setRelaxation (patched Tyre.SetInflation): the distance the tyre
  // rolls for its lateral force to build, in metres. It grows as the pressure
  // falls; 0.3 m, a typical road tyre, without the patched script.
  double relaxation_length = .3;
  // Longitudinal relaxation length (m, #498): WheelRef.setLongitudinalRelaxation;
  // zero takes half the lateral one.
  double longitudinal_relaxation = 0;
  // Stored setCPatch tolerance in radians. Not consumed by the 8ac2ba9
  // force law; alignment is supplied separately as a load factor.
  double patch_angle = 0;
  // Loaded radius (m), the scale of the pneumatic trail (#458); zero is
  // 0.31 m, the stock Baiern wheel.
  double radius = 0;
};

struct TireForces {
  double longitudinal = 0, lateral = 0;
  // Self-aligning torque (N m, #458) about the contact normal: the lateral
  // force acting a pneumatic trail behind the patch centre. Positive turns
  // the heading towards -lateral, the solver's left: trail x lateral.
  double aligning = 0;
  // The patch's lateral offset from the wheel plane (m, along +lateral) that
  // the lateral force's carcass deflection makes; aligning includes
  // offset x longitudinal.
  double offset = 0;
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
// aligning is the tyre's moment about the wheel's vertical (#458): the slip
// force behind an MF pneumatic trail that grows with the load and the
// patch length (the relaxation length, which the pressure sets) and is gone
// a little past the lateral peak of the combined slip, plus the
// longitudinal force at the patch's lateral offset.
// sliding_speed is the contact patch's actual lateral speed when
// lateral_speed is a relaxed (lagged) value; it bounds the low-speed force
// (NaN: the same as lateral_speed).
// slipping_speed likewise is the patch's actual longitudinal slip speed when
// wheel_speed carries a relaxed one (#498).
TireForces tire_forces(double load, double alignment, double wheel_speed,
                       double road_speed, double lateral_speed,
                       const TireParameters& tire, double camber = 0,
                       double sliding_speed = std::numeric_limits<double>::quiet_NaN(),
                       double slipping_speed = std::numeric_limits<double>::quiet_NaN());
// How the relaxation lengths change with the load (#498): as the cornering
// stiffness at that load against the nominal one (the carcass' own
// stiffness stays), 0.3 to 1.5.
double relaxation_load_factor(double load, const TireParameters& tire);

} // namespace slrr::physics
