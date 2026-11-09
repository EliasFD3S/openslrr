#include "slrr/deformation_cage.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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
        nodes_.push_back({point, point, {}, {}, {}, mount, {}, {}});
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
        // The longer flex beams in the upstream cage have 30% stiffness.
        for (const auto& [dx, dz] : {std::pair{2, 0}, std::pair{0, 2}}) {
          if (x + dx >= nx || z + dz >= nz) continue;
          const int b = index(x + dx, y, z + dz);
          const double length = (nodes_[a].rest - nodes_[b].rest).length();
          beams_.push_back({a, b, length, length, 1.0 / (900000 * .3)});
        }
      }
}

DeformationCage::DeformationCage(Vec3 minimum, Vec3 maximum, double mass,
                                 Vec3 mount, bool hood)
    : DeformationCage(minimum, maximum, mass) {
  const double stiffness = hood ? 500000 : 750000;
  yield_ = hood ? .012 : .018;
  plastic_rate_ = hood ? 1 : 120;
  mount_strength_ = 1.1 * (hood ? 60000 : std::max(12000.0, mass * 2000));
  inverse_mass_ = static_cast<double>(nodes_.size()) / std::max(2.0, mass);
  for (auto& beam : beams_) beam.compliance *= 900000 / stiffness;
  for (auto& node : nodes_) node.fixed = false;
  auto anchor = [&](Vec3 target) {
    for (int y = 0; y < ny; ++y) {
      int nearest = -1;
      double distance = std::numeric_limits<double>::infinity();
      for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x) {
          const int i = index(x, y, z);
          const Vec3 offset = nodes_[i].rest - target;
          const double squared = offset.dot(offset);
          if (!nodes_[i].fixed && squared < distance) {
            nearest = i;
            distance = squared;
          }
        }
      if (nearest >= 0) nodes_[nearest].fixed = true;
    }
  };
  for (double side : {-1., 1.})
    anchor(mount + Vec3{extent_.x * .32 * side, 0, 0});
  const int front = mount.z - minimum_.z > extent_.z * .5 ? 0 : nz - 1;
  anchor({minimum_.x + extent_.x * .5, mount.y, front == 0 ? minimum_.z : maximum.z});
  if (hood) hood_stops_ = {index(0, 0, front), index(nx - 1, 0, front)};
}

void DeformationCage::impact(Vec3 point, Vec3 normal, double speed, double radius) {
  if (speed < 2.0) return;
  const double excess = speed - 2;
  const double velocity = std::min(16.0, excess * .65);
  const double crush = std::min(radius * .45, excess * excess * .0018);
  if (mount_strength_ > 0) {
    const double mass = static_cast<double>(nodes_.size()) / inverse_mass_;
    // The chassis sweep resolves the wall before XPBD sees the contact.
    // Transfer its short load pulse over the estimated mount travel.
    constexpr double mount_travel = .045;
    impact_load_ = std::max(impact_load_, mass * speed * speed / (2 * mount_travel));
  }
  for (auto& node : nodes_) {
    if (node.fixed) continue;
    const double distance = (node.position - point).length() / radius;
    if (distance >= 1) continue;
    const double weight = (1 - distance * distance) * (1 - distance * distance);
    node.position += normal * (crush * weight);
    node.velocity += normal * (velocity * weight);
  }
  // The chassis sweep resolves world contact before the local XPBD step.
  // Apply its local crush before the constraints can spring back.
  yield_beams();
  active_ = true;
  age_ = quiet_ = 0;
}

double DeformationCage::step(const GroundContact& ground) {
  constexpr double dt2 = dt * dt;
  const double damping = std::exp(-14 * dt);
  for (auto& n : nodes_) {
    n.previous = n.position;
    n.position += n.velocity * (dt * damping);
    n.lambda = {};
    n.floor_normal = {};
    if (ground && !n.fixed) ground(n.position, n.floor_point, n.floor_normal);
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
    for (int i : hood_stops_) {
      if (i < 0) continue;
      auto& n = nodes_[i];
      const Vec3 delta = n.position - n.rest;
      if (delta.x * delta.x + delta.z * delta.z < .08 * .08)
        n.position.y = std::max(n.position.y, n.rest.y);
    }
    if (ground) {
      for (auto& n : nodes_) {
        const double depth = (n.floor_point - n.position).dot(n.floor_normal);
        if (depth > 0) n.position += n.floor_normal * depth;
      }
    }
  }
  if (mount_strength_ > 0 && !released_) {
    double load = impact_load_;
    for (const auto& node : nodes_)
      if (node.fixed) load += node.lambda.length() / dt2;
    // The reference's two overloaded 240 Hz ticks equal one driving step.
    released_ = load > mount_strength_;
    impact_load_ = 0;
  }
  yield_beams();
  double peak_speed = 0;
  for (auto& n : nodes_) {
    n.velocity = (n.position - n.previous) / dt;
    peak_speed = std::max(peak_speed, n.velocity.length());
  }
  return peak_speed;
}

void DeformationCage::yield_beams() {
  for (auto& b : beams_) {
    const double length = (nodes_[b.a].position - nodes_[b.b].position).length();
    const double strain = (length - b.rest) / b.original;
    if (std::abs(strain) > yield_)
      b.rest = std::clamp(b.rest + std::copysign((std::abs(strain) - yield_) *
                                   b.original * std::min(1., plastic_rate_ * dt), strain),
                          b.original * .5, b.original * 1.5);
  }
}

bool DeformationCage::advance(unsigned steps, const GroundContact& ground) {
  if (!steps || !active_) return false;
  for (unsigned i = 0; i < steps && active_; ++i) {
    const double speed = step(ground);
    ++age_;
    quiet_ = speed < .03 ? quiet_ + 1 : 0;
    if (released_ || (age_ >= VehicleDynamics::frequency / 2 &&
                      quiet_ >= VehicleDynamics::frequency / 5)) {
      active_ = false;
      for (auto& node : nodes_) node.velocity = {};
    }
  }
  return true;
}

float DeformationCage::condition() const {
  double strain2 = 0;
  for (const auto& beam : beams_) {
    const double strain = (beam.rest - beam.original) / beam.original;
    strain2 += strain * strain;
  }
  const double scaled = std::sqrt(strain2 / static_cast<double>(beams_.size())) / .02;
  return static_cast<float>(1 / (1 + scaled * scaled));
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

// SLRR_rev deformed-frame.ts. Displacement preserves the material probe's
// original offset when its binding lies outside the cage.
Matrix DeformationCage::carry_frame(const Matrix& original) const {
  const Vec3 origin{original[12], original[13], original[14]};
  const Vec3 moved = origin + displacement(origin);
  const auto probe = [&](int column) {
    const Vec3 axis{original[column], original[column + 1], original[column + 2]};
    const Vec3 point = origin + axis * .1;
    return point + displacement(point) - moved;
  };
  const Vec3 x = probe(0).normalized();
  const Vec3 up = probe(4);
  const Vec3 y = (up - x * up.dot(x)).normalized();
  const Vec3 z = x.cross(y);
  if (z.dot(z) < .5) return original;
  return {float(x.x), float(x.y), float(x.z), 0,
          float(y.x), float(y.y), float(y.z), 0,
          float(z.x), float(z.y), float(z.z), 0,
          float(moved.x), float(moved.y), float(moved.z), 1};
}

}  // namespace slrr::physics
