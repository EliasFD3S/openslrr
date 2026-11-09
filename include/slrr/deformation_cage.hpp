#pragma once

#include "slrr/vehicle_dynamics.hpp"
#include <vector>

namespace slrr::physics {

// Attached volume cage, adapted from the SLRR_rev XPBD panel solver.
class DeformationCage {
public:
  DeformationCage(Vec3 minimum, Vec3 maximum, double mass);
  DeformationCage(Vec3 minimum, Vec3 maximum, double mass, Vec3 mount, bool hood);
  void impact(Vec3 point, Vec3 normal, double speed, double radius);
  bool advance(unsigned steps);
  Vec3 displacement(Vec3 point) const;
  bool active() const { return active_; }
  bool released() const { return released_; }
  float condition() const;

private:
  struct Node { Vec3 rest, position, velocity, previous, lambda; bool fixed = false; };
  struct Beam { int a, b; double original, rest, compliance, lambda = 0; };
  Vec3 minimum_, extent_;
  double inverse_mass_;
  double yield_ = .003, plastic_rate_ = 120;
  double mount_strength_ = 0, impact_load_ = 0;
  unsigned age_ = 0, quiet_ = 0;
  bool active_ = false, released_ = false;
  std::vector<Node> nodes_;
  std::vector<Beam> beams_;
  void yield_beams();
  double step();
};

}  // namespace slrr::physics
