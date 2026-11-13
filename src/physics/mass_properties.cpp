#include "slrr/mass_properties.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace slrr::physics {
namespace {

// Inertia of a point mass `mass` at offset d: m (|d|^2 E - d d^T).
std::array<double, 9> offset_inertia(double mass, Vec3 d) {
  const double r2 = d.dot(d);
  return {mass * (r2 - d.x * d.x), -mass * d.x * d.y, -mass * d.x * d.z,
          -mass * d.y * d.x, mass * (r2 - d.y * d.y), -mass * d.y * d.z,
          -mass * d.z * d.x, -mass * d.z * d.y, mass * (r2 - d.z * d.z)};
}

bool finite(const MassProperties& p) {
  if (!std::isfinite(p.mass) || !p.center.finite()) return false;
  for (double value : p.inertia)
    if (!std::isfinite(value)) return false;
  return true;
}

}  // namespace

MassProperties point_mass(double mass, Vec3 at) {
  MassProperties out;
  if (!(mass > 0) || !std::isfinite(mass) || !at.finite()) return out;
  out.mass = mass;
  out.center = at;
  return out;
}

MassProperties box_mass(double mass, Vec3 center, Vec3 half) {
  MassProperties out = point_mass(mass, center);
  if (out.empty() || !half.finite()) return out;
  const double x = 4 * half.x * half.x, y = 4 * half.y * half.y, z = 4 * half.z * half.z;
  out.inertia[0] = mass * (y + z) / 12;
  out.inertia[4] = mass * (x + z) / 12;
  out.inertia[8] = mass * (x + y) / 12;
  return out;
}

MassProperties sphere_mass(double mass, Vec3 center, double radius) {
  MassProperties out = point_mass(mass, center);
  if (out.empty() || !std::isfinite(radius)) return out;
  out.inertia[0] = out.inertia[4] = out.inertia[8] = .4 * mass * radius * radius;
  return out;
}

// Signed tetrahedra from a local origin (divergence theorem). For the
// tetrahedron (0, a, b, c) of signed volume v:
//   integral of x_i x_j dV = v / 20 * (a_i a_j + b_i b_j + c_i c_j + S_i S_j),
// with S = a + b + c. Vertices need not be shared: a triangle soup that
// closes geometrically (SCX splits vertices by normal and UV) integrates too.
bool mesh_mass(double mass, const float* positions, size_t floats, double scale,
               const uint32_t* indices, size_t index_count, MassProperties& out) {
  out = {};
  if (!(mass > 0) || !std::isfinite(mass) || !positions || !indices ||
      index_count < 12 || !(scale > 0))
    return false;
  const size_t vertices = floats / 3;
  const auto point = [&](uint32_t i) {
    return Vec3{positions[i * 3] * scale, positions[i * 3 + 1] * scale,
                positions[i * 3 + 2] * scale};
  };
  if (indices[0] >= vertices) return false;
  // Closed and consistently wound: after welding equal positions, every edge
  // is crossed as often in one direction as in the other.
  {
    std::map<std::array<long long, 3>, uint32_t> welded;
    std::vector<uint32_t> id(vertices);
    for (size_t v = 0; v < vertices; ++v) {
      const Vec3 p = point(static_cast<uint32_t>(v));
      const std::array<long long, 3> key{std::llround(p.x * 1e4), std::llround(p.y * 1e4),
                                         std::llround(p.z * 1e4)};
      id[v] = welded.emplace(key, static_cast<uint32_t>(welded.size())).first->second;
    }
    std::map<std::pair<uint32_t, uint32_t>, int> balance;
    for (size_t t = 0; t + 2 < index_count; t += 3)
      for (int k = 0; k < 3; ++k) {
        if (indices[t + k] >= vertices || indices[t + (k + 1) % 3] >= vertices) return false;
        const uint32_t a = id[indices[t + k]], b = id[indices[t + (k + 1) % 3]];
        if (a == b) continue;
        balance[{std::min(a, b), std::max(a, b)}] += a < b ? 1 : -1;
      }
    for (const auto& edge : balance)
      if (edge.second) return false;
  }
  const Vec3 origin = point(indices[0]);
  Vec3 lo{1e30, 1e30, 1e30}, hi = -lo;
  double volume = 0;
  Vec3 first;
  double second[6] = {};  // xx, yy, zz, xy, xz, yz
  for (size_t t = 0; t + 2 < index_count; t += 3) {
    if (indices[t] >= vertices || indices[t + 1] >= vertices || indices[t + 2] >= vertices)
      return false;
    const Vec3 a = point(indices[t]) - origin, b = point(indices[t + 1]) - origin,
               c = point(indices[t + 2]) - origin;
    for (const Vec3& p : {a, b, c}) {
      lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const double v = a.dot(b.cross(c)) / 6;
    const Vec3 s = a + b + c;
    volume += v;
    first += s * (v / 4);
    const double k = v / 20;
    second[0] += k * (a.x * a.x + b.x * b.x + c.x * c.x + s.x * s.x);
    second[1] += k * (a.y * a.y + b.y * b.y + c.y * c.y + s.y * s.y);
    second[2] += k * (a.z * a.z + b.z * b.z + c.z * c.z + s.z * s.z);
    second[3] += k * (a.x * a.y + b.x * b.y + c.x * c.y + s.x * s.y);
    second[4] += k * (a.x * a.z + b.x * b.z + c.x * c.z + s.x * s.z);
    second[5] += k * (a.y * a.z + b.y * b.z + c.y * c.z + s.y * s.z);
  }
  // Inward winding gives the same integrals with the opposite sign.
  const double sign = volume < 0 ? -1 : 1;
  volume *= sign;
  first = first * sign;
  for (double& value : second) value *= sign;
  const Vec3 extent = hi - lo;
  const double bounds = extent.x * extent.y * extent.z;
  // A closed solid lies inside its bounds and cannot exceed their volume. An
  // open shell or a self-cancelling soup fails one of these tests.
  if (!(volume > 1e-9) || !(volume <= bounds * 1.001)) return false;
  const Vec3 center = first / volume;
  const double tolerance = 1e-4 + 1e-3 * extent.length();
  if (center.x < lo.x - tolerance || center.y < lo.y - tolerance ||
      center.z < lo.z - tolerance || center.x > hi.x + tolerance ||
      center.y > hi.y + tolerance || center.z > hi.z + tolerance)
    return false;
  const double density = mass / volume;
  const double trace = second[0] + second[1] + second[2];
  // About the local origin, then moved to the centre of mass.
  std::array<double, 9> inertia{
      density * (trace - second[0]), -density * second[3], -density * second[4],
      -density * second[3], density * (trace - second[1]), -density * second[5],
      -density * second[4], -density * second[5], density * (trace - second[2])};
  const auto shift = offset_inertia(mass, center);
  for (size_t i = 0; i < inertia.size(); ++i) inertia[i] -= shift[i];
  // A physical inertia has positive moments obeying the triangle inequality.
  const double ixx = inertia[0], iyy = inertia[4], izz = inertia[8];
  const double slack = 1e-6 * (ixx + iyy + izz);
  if (!(ixx > 0 && iyy > 0 && izz > 0) || ixx > iyy + izz + slack ||
      iyy > ixx + izz + slack || izz > ixx + iyy + slack)
    return false;
  out.mass = mass;
  out.center = origin + center;
  out.inertia = inertia;
  return finite(out);
}

MassProperties transformed(const MassProperties& local, const Matrix& frame) {
  if (local.empty()) return {};
  MassProperties out;
  out.mass = local.mass;
  const double r[9] = {frame[0], frame[4], frame[8],
                       frame[1], frame[5], frame[9],
                       frame[2], frame[6], frame[10]};  // row-major R
  const Vec3 c = local.center;
  out.center = {r[0] * c.x + r[1] * c.y + r[2] * c.z + frame[12],
                r[3] * c.x + r[4] * c.y + r[5] * c.z + frame[13],
                r[6] * c.x + r[7] * c.y + r[8] * c.z + frame[14]};
  // I' = R I R^T
  double ri[9];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double sum = 0;
      for (int k = 0; k < 3; ++k) sum += r[i * 3 + k] * local.inertia[size_t(k * 3 + j)];
      ri[i * 3 + j] = sum;
    }
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double sum = 0;
      for (int k = 0; k < 3; ++k) sum += ri[i * 3 + k] * r[j * 3 + k];
      out.inertia[size_t(i * 3 + j)] = sum;
    }
  return out;
}

void accumulate(MassProperties& sum, const MassProperties& part) {
  if (part.empty() || !finite(part)) return;
  if (sum.empty()) {
    sum = part;
    return;
  }
  const double mass = sum.mass + part.mass;
  const Vec3 center = (sum.center * sum.mass + part.center * part.mass) / mass;
  const auto a = offset_inertia(sum.mass, sum.center - center);
  const auto b = offset_inertia(part.mass, part.center - center);
  for (size_t i = 0; i < sum.inertia.size(); ++i)
    sum.inertia[i] += a[i] + part.inertia[i] + b[i];
  sum.mass = mass;
  sum.center = center;
}

}  // namespace slrr::physics
