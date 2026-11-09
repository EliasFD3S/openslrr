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
  using GroundContact = std::function<bool(Vec3 position, Vec3& point, Vec3& normal)>;
  bool advance(unsigned steps, const GroundContact& ground = {});
  Vec3 displacement(Vec3 point) const;
  Matrix carry_frame(const Matrix& original) const;
  bool active() const { return active_; }
  bool released() const { return released_; }
  float condition() const;

private:
  struct Node {
    Vec3 rest, position, velocity, previous, lambda;
    bool fixed = false;
    Vec3 floor_point, floor_normal;
  };
  struct Beam { int a, b; double original, rest, compliance, lambda = 0; };
  Vec3 minimum_, extent_;
  double inverse_mass_;
  double yield_ = .003, plastic_rate_ = 120;
  double mount_strength_ = 0, impact_load_ = 0;
  unsigned age_ = 0, quiet_ = 0;
  bool active_ = false, released_ = false;
  std::vector<Node> nodes_;
  std::vector<Beam> beams_;
  std::array<int, 2> hood_stops_{-1, -1};
  void yield_beams();
  double step(const GroundContact& ground);
};

}  // namespace slrr::physics
