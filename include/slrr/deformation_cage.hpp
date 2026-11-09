#pragma once

#include "slrr/vehicle_dynamics.hpp"
#include <vector>

namespace slrr::physics {

// Attached volume cage, adapted from the SLRR_rev XPBD panel solver.
class DeformationCage {
public:
  DeformationCage(Vec3 minimum, Vec3 maximum, double mass);
  void impact(Vec3 point, Vec3 normal, double speed);
  bool advance(unsigned steps);
  Vec3 displacement(Vec3 point) const;

private:
  struct Node { Vec3 rest, position, velocity, previous, lambda; bool fixed = false; };
  struct Beam { int a, b; double original, rest, compliance, lambda = 0; };
  Vec3 minimum_, extent_;
  double inverse_mass_;
  unsigned active_ = 0;
  std::vector<Node> nodes_;
  std::vector<Beam> beams_;
  void yield_beams();
  void step();
};

}  // namespace slrr::physics
