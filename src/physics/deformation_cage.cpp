#include "slrr/deformation_cage.hpp"

#include <algorithm>
#include <cmath>

namespace slrr::physics {
namespace {
constexpr int nx = 5, ny = 2, nz = 7;
constexpr int index(int x, int y, int z) { return (z * ny + y) * nx + x; }
constexpr double dt = VehicleDynamics::timestep;
}

DeformationCage::DeformationCage(Vec3 minimum, Vec3 maximum, double mass)
    : minimum_(minimum), extent_(maximum - minimum),
      inverse_mass_(nx * ny * nz / std::max(15.0, mass)) {
  extent_.x = std::max(.02, extent_.x);
  extent_.y = std::max(.02, extent_.y);
  extent_.z = std::max(.02, extent_.z);
  for (int z = 0; z < nz; ++z)
    for (int y = 0; y < ny; ++y)
      for (int x = 0; x < nx; ++x) {
        const Vec3 point = minimum_ + Vec3{extent_.x * x / (nx - 1),
                                         extent_.y * y / (ny - 1),
                                         extent_.z * z / (nz - 1)};
        const bool mount = (z == 2 || z == 4) && (x == 1 || x == 3);
        nodes_.push_back({point, point, {}, {}, {}, mount});
      }
  for (int z = 0; z < nz; ++z)
    for (int y = 0; y < ny; ++y)
      for (int x = 0; x < nx; ++x) {
        const int a = index(x, y, z);
        for (int dz = 0; dz <= 1; ++dz)
          for (int dy = dz ? -1 : 0; dy <= 1; ++dy)
            for (int dx = (dz || dy) ? -1 : 1; dx <= 1; ++dx) {
              if (x + dx < 0 || x + dx >= nx || y + dy < 0 ||
                  y + dy >= ny || z + dz >= nz) continue;
              const int b = index(x + dx, y + dy, z + dz);
              const double length = (nodes_[a].rest - nodes_[b].rest).length();
              beams_.push_back({a, b, length, length, 1.0 / 900000});
            }
      }
}

void DeformationCage::impact(Vec3 point, Vec3 normal, double speed) {
  if (speed < 2.0) return;
  const double velocity = std::min(18.0, (speed - 2) * 1.25);
  const double crush = std::min(.45, (speed - 2) * .022);
  for (auto& node : nodes_) {
    if (node.fixed) continue;
    const double distance = (node.position - point).length() / .85;
    const double weight = std::exp(-distance * distance * 2);
    node.position += normal * (crush * weight);
    node.velocity += normal * (velocity * weight);
  }
  // The chassis sweep resolves world contact before the local XPBD step.
  // Apply its local crush before the constraints can spring back.
  yield_beams();
  active_ = VehicleDynamics::frequency * 3 / 4;
}

void DeformationCage::step() {
  constexpr double dt2 = dt * dt;
  const double damping = std::exp(-7 * dt);
  for (auto& n : nodes_) {
    n.previous = n.position;
    n.position += n.velocity * (dt * damping);
    n.lambda = {};
  }
  for (auto& b : beams_) b.lambda = 0;
  for (int iteration = 0; iteration < 12; ++iteration) {
    for (auto& beam : beams_) {
      auto& a = nodes_[beam.a];
      auto& b = nodes_[beam.b];
      const Vec3 delta = a.position - b.position;
      const double length = delta.length();
      if (length <= 1e-10) continue;
      const double alpha = beam.compliance / dt2;
      const double dl = (-(length - beam.rest) - alpha * beam.lambda) /
                        (2 * inverse_mass_ + alpha);
      beam.lambda += dl;
      const Vec3 correction = delta * (inverse_mass_ * dl / length);
      a.position += correction;
      b.position += -correction;
    }
    for (auto& n : nodes_) {
      if (!n.fixed) continue;
      constexpr double alpha = 1e-7 / dt2;
      const Vec3 dl = (n.rest - n.position - n.lambda * alpha) /
                      (inverse_mass_ + alpha);
      n.lambda += dl;
      n.position += dl * inverse_mass_;
    }
  }
  yield_beams();
  for (auto& n : nodes_) n.velocity = (n.position - n.previous) / dt;
}

void DeformationCage::yield_beams() {
  for (auto& b : beams_) {
    const double length = (nodes_[b.a].position - nodes_[b.b].position).length();
    const double strain = (length - b.rest) / b.original;
    constexpr double yield = .003;
    if (std::abs(strain) > yield)
      b.rest = std::clamp(b.rest + std::copysign((std::abs(strain) - yield) *
                                   b.original * std::min(1., 120 * dt), strain),
                          b.original * .5, b.original * 1.5);
  }
}

bool DeformationCage::advance(unsigned steps) {
  const unsigned count = std::min(steps, active_);
  for (unsigned i = 0; i < count; ++i) step();
  active_ -= count;
  return count != 0;
}

Vec3 DeformationCage::displacement(Vec3 point) const {
  const Vec3 relative = point - minimum_;
  const auto coordinate = [](double t, int count) {
    return std::clamp(t * (count - 1), 0., double(count - 1));
  };
  const double px = coordinate(relative.x / extent_.x, nx);
  const double py = coordinate(relative.y / extent_.y, ny);
  const double pz = coordinate(relative.z / extent_.z, nz);
  const int x = std::min(int(px), nx - 2), z = std::min(int(pz), nz - 2);
  Vec3 sum;
  for (int dz = 0; dz <= 1; ++dz)
    for (int dy = 0; dy <= 1; ++dy)
      for (int dx = 0; dx <= 1; ++dx) {
        const auto& node = nodes_[index(x + dx, dy, z + dz)];
        const double weight = (dx ? px - x : 1 - px + x) * (dy ? py : 1 - py) *
                              (dz ? pz - z : 1 - pz + z);
        sum += (node.position - node.rest) * weight;
      }
  return sum;
}

}  // namespace slrr::physics
