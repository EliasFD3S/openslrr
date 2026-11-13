#pragma once

#include "slrr/vehicle_dynamics.hpp"

namespace slrr::physics {

// Wheel mount failure, ported from SLRR_rev 77b0fb6 (vehicle-3d.ts
// releaseFailedWheels, moveMounts and damageImpact; suspension-3d.ts deform).
// SLRR_rev's own tuning, not recovered stock behaviour.
//
// A mount condition starts at 1. The wheel breaks off at 0 or below.
struct WheelMountTuning {
  // Chassis-cage displacement of the hardpoint that ends the mount
  // (SLRR_rev: mountCondition = 1 - shift / 0.3 m).
  double travel = .3;
  // Impact energy the corner hardware absorbs before it fails, J (SLRR_rev
  // damageImpact suspension threshold).
  double energy = 55000;
  // Horizontal reach of an impact's load, m: weight exp(-(d / reach)^2).
  double reach = .65;
};

// Energy SLRR_rev charges a contact with, |J|^2 / 2M, for the impulse that
// stops `closing_speed` along `normal` at `point` (world frame) with the given
// restitution, through the body's effective mass at that point.
double contact_impulse_energy(const RigidBody& body, Vec3 point, Vec3 normal,
                              double closing_speed, double restitution = .12);

// Condition after one impact. `shift` is the current chassis-cage
// displacement of the hardpoint (m), `energy` the impact energy (J) and
// `distance` the horizontal distance from the impact to the corner (m).
float wheel_mount_condition(float condition, double shift, double energy, double distance,
                            const WheelMountTuning& tuning = {});

}  // namespace slrr::physics
