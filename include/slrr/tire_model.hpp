#pragma once

namespace slrr::physics {

struct TireParameters {
  double friction = 1.76863, stiffness = 17.6839, sliction = .814;
  double maximum_load = 0, longitudinal_grip = 1;
  double load_smoothing = .4;
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

TireForces tire_forces(double load, double alignment, double wheel_speed,
                       double road_speed, double lateral_speed,
                       const TireParameters& tire);

} // namespace slrr::physics
