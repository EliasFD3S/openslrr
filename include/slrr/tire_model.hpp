#pragma once

namespace slrr::physics {

struct TireParameters {
  double friction = 1.76863, stiffness = 17.6839, sliction = .814;
  double maximum_load = 0, longitudinal_grip = 1;
};

struct TireForces {
  double longitudinal, lateral;
};

TireForces tire_forces(double load, double alignment, double wheel_speed,
                       double road_speed, double lateral_speed,
                       const TireParameters& tire);

} // namespace slrr::physics
