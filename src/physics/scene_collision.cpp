#include "slrr/scene_collision.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace slrr::collision {
namespace {

using physics::Vec3;

constexpr double kFrameTolerance = 2e-3;
constexpr double kGeometryEpsilon = 1e-10;
constexpr std::size_t kMaxCellsPerShape = 256;
constexpr std::size_t kMaxCellsPerQuery = 4096;

bool finite(Vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

double component(Vec3 value, int axis) {
  return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
}

bool valid_dimensions(Vec3 half_extents) {
  return finite(half_extents) && half_extents.x > 0 && half_extents.y > 0 &&
         half_extents.z > 0;
}

bool valid_axes(const std::array<Vec3, 3>& axes) {
  for (const Vec3& axis : axes) {
    if (!finite(axis) || std::abs(axis.dot(axis) - 1.0) > kFrameTolerance)
      return false;
  }
  for (int a = 0; a < 3; ++a)
    for (int b = a + 1; b < 3; ++b)
      if (std::abs(axes[a].dot(axes[b])) > kFrameTolerance) return false;
  return true;
}

bool valid_box(const Box& box) {
  return finite(box.center) && valid_dimensions(box.half_extents) &&
         valid_axes(box.axes);
}

bool valid_sphere(const Sphere& sphere) {
  return finite(sphere.center) && std::isfinite(sphere.radius) &&
         sphere.radius > 0;
}

bool frame_parts(const PartMatrix& frame, Vec3& center,
                 std::array<Vec3, 3>& axes) {
  for (float value : frame)
    if (!std::isfinite(value)) return false;
  if (std::abs(frame[3]) > kFrameTolerance ||
      std::abs(frame[7]) > kFrameTolerance ||
      std::abs(frame[11]) > kFrameTolerance ||
      std::abs(frame[15] - 1.0f) > kFrameTolerance)
    return false;
  for (int axis = 0; axis < 3; ++axis)
    axes[axis] = {frame[axis * 4], frame[axis * 4 + 1],
                  frame[axis * 4 + 2]};
  center = {frame[12], frame[13], frame[14]};
  return finite(center) && valid_axes(axes);
}

struct Aabb {
  Vec3 minimum{};
  Vec3 maximum{};
};

Aabb box_bounds(const Box& box) {
  double extent[3] = {};
  for (int axis = 0; axis < 3; ++axis)
    for (int local = 0; local < 3; ++local)
      extent[axis] += std::abs(component(box.axes[local], axis)) *
                      component(box.half_extents, local);
  const Vec3 radius{extent[0], extent[1], extent[2]};
  return {box.center - radius, box.center + radius};
}

Aabb sphere_bounds(const Sphere& sphere) {
  const Vec3 extent{sphere.radius, sphere.radius, sphere.radius};
  return {sphere.center - extent, sphere.center + extent};
}

bool overlaps(const Aabb& a, const Aabb& b) {
  for (int axis = 0; axis < 3; ++axis) {
    const double tolerance =
        kGeometryEpsilon *
        (1.0 + std::abs(component(a.minimum, axis)) +
         std::abs(component(a.maximum, axis)) +
         std::abs(component(b.minimum, axis)) +
         std::abs(component(b.maximum, axis)));
    if (component(a.maximum, axis) < component(b.minimum, axis) - tolerance ||
        component(b.maximum, axis) < component(a.minimum, axis) - tolerance)
      return false;
  }
  return true;
}

Vec3 closest_point(const Box& box, Vec3 point) {
  const Vec3 offset = point - box.center;
  Vec3 result = box.center;
  for (int axis = 0; axis < 3; ++axis) {
    const double half = component(box.half_extents, axis);
    const double projection = std::clamp(offset.dot(box.axes[axis]), -half, half);
    result += box.axes[axis] * projection;
  }
  return result;
}

bool contains_center(const Box& box, Vec3 point) {
  const Vec3 offset = point - box.center;
  for (int axis = 0; axis < 3; ++axis)
    if (std::abs(offset.dot(box.axes[axis])) >=
        component(box.half_extents, axis))
      return false;
  return true;
}

// The SAT normal is also a useful exit direction for the ambiguous case where
// the moving centre is inside the obstacle. It keeps the point on the obstacle
// instead of averaging opposing support vertices into its interior.
Vec3 box_contact_point(const Box& moving, const Box& obstacle, Vec3 normal) {
  if (!contains_center(obstacle, moving.center))
    return closest_point(obstacle, moving.center);

  double exit_distance = std::numeric_limits<double>::infinity();
  for (int axis = 0; axis < 3; ++axis) {
    const double origin = (moving.center - obstacle.center).dot(obstacle.axes[axis]);
    const double direction = normal.dot(obstacle.axes[axis]);
    if (std::abs(direction) <= kGeometryEpsilon) continue;
    const double face = direction > 0 ? component(obstacle.half_extents, axis)
                                      : -component(obstacle.half_extents, axis);
    const double distance = (face - origin) / direction;
    if (distance >= 0) exit_distance = std::min(exit_distance, distance);
  }
  return std::isfinite(exit_distance) ? moving.center + normal * exit_distance
                                      : closest_point(obstacle, moving.center);
}

bool intersect(const Box& moving, const Box& obstacle, Contact& contact) {
  const Vec3 displacement = moving.center - obstacle.center;
  double penetration = std::numeric_limits<double>::infinity();
  Vec3 normal{0, 1, 0};

  const auto test_axis = [&](Vec3 axis) {
    const double length2 = axis.dot(axis);
    if (length2 <= kGeometryEpsilon * kGeometryEpsilon) return true;
    axis = axis / std::sqrt(length2);
    double moving_radius = 0;
    double obstacle_radius = 0;
    for (int local = 0; local < 3; ++local) {
      moving_radius += component(moving.half_extents, local) *
                       std::abs(moving.axes[local].dot(axis));
      obstacle_radius += component(obstacle.half_extents, local) *
                         std::abs(obstacle.axes[local].dot(axis));
    }
    const double distance = std::abs(displacement.dot(axis));
    const double sum = moving_radius + obstacle_radius;
    const double tolerance = kGeometryEpsilon * (1.0 + sum + distance);
    if (distance > sum + tolerance) return false;
    const double depth = std::max(0.0, sum - distance);
    if (depth < penetration) {
      penetration = depth;
      normal = displacement.dot(axis) < 0 ? -axis : axis;
    }
    return true;
  };

  for (int axis = 0; axis < 3; ++axis)
    if (!test_axis(moving.axes[axis])) return false;
  for (int axis = 0; axis < 3; ++axis)
    if (!test_axis(obstacle.axes[axis])) return false;
  for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b) {
      const Vec3 cross = moving.axes[a].cross(obstacle.axes[b]);
      // Parallel and nearly parallel face pairs are already covered by their
      // face axes; normalising numerical noise there is not a stable SAT test.
      if (cross.dot(cross) <= 1e-18) continue;
      if (!test_axis(cross)) return false;
    }

  contact.normal = normal;
  contact.penetration = penetration;
  contact.point = box_contact_point(moving, obstacle, normal);
  return true;
}

bool intersect(const Box& moving, const Sphere& obstacle, Contact& contact) {
  const Vec3 offset = obstacle.center - moving.center;
  double local[3];
  Vec3 nearest = moving.center;
  for (int axis = 0; axis < 3; ++axis) {
    const double half = component(moving.half_extents, axis);
    local[axis] = offset.dot(moving.axes[axis]);
    nearest += moving.axes[axis] * std::clamp(local[axis], -half, half);
  }

  const Vec3 from_sphere = nearest - obstacle.center;
  const double distance2 = from_sphere.dot(from_sphere);
  if (distance2 > kGeometryEpsilon * kGeometryEpsilon) {
    const double distance = std::sqrt(distance2);
    const double tolerance = kGeometryEpsilon * (1.0 + obstacle.radius + distance);
    if (distance > obstacle.radius + tolerance) return false;
    const Vec3 normal = from_sphere / distance;
    contact.normal = normal;
    contact.penetration = std::max(0.0, obstacle.radius - distance);
    contact.point = obstacle.center + normal * obstacle.radius;
    return true;
  }

  // The sphere centre is on or inside the box, so the closest-point direction
  // is zero. The minimum translation is through the nearest box face, with the
  // direction reversed because the box (not the sphere) is the moving shape.
  int nearest_axis = 0;
  double nearest_distance = std::numeric_limits<double>::infinity();
  for (int axis = 0; axis < 3; ++axis) {
    const double distance = std::max(
        0.0, component(moving.half_extents, axis) - std::abs(local[axis]));
    if (distance < nearest_distance) {
      nearest_distance = distance;
      nearest_axis = axis;
    }
  }
  const double centre_projection =
      (moving.center - obstacle.center).dot(moving.axes[nearest_axis]);
  const Vec3 normal = moving.axes[nearest_axis] * (centre_projection < 0 ? -1.0 : 1.0);
  contact.normal = normal;
  contact.penetration = obstacle.radius + nearest_distance;
  contact.point = obstacle.center - normal * obstacle.radius;
  return true;
}

ScxGeometry invalid_mesh(const char* error) {
  ScxGeometry mesh;
  mesh.position_scale = 1.0f;
  mesh.info.error = error;
  return mesh;
}

void append_vertex(ScxGeometry& mesh, Vec3 position, Vec3 normal) {
  mesh.positions.insert(mesh.positions.end(),
                        {static_cast<float>(position.x), static_cast<float>(position.y),
                         static_cast<float>(position.z)});
  mesh.normals.insert(mesh.normals.end(),
                      {static_cast<float>(normal.x), static_cast<float>(normal.y),
                       static_cast<float>(normal.z)});
}

Vec3 mesh_position(const ScxGeometry& mesh, uint32_t index) {
  const std::size_t offset = static_cast<std::size_t>(index) * 3;
  return {mesh.positions[offset], mesh.positions[offset + 1],
          mesh.positions[offset + 2]};
}

Vec3 mesh_normal(const ScxGeometry& mesh, uint32_t index) {
  const std::size_t offset = static_cast<std::size_t>(index) * 3;
  return {mesh.normals[offset], mesh.normals[offset + 1], mesh.normals[offset + 2]};
}

void append_oriented_triangle(ScxGeometry& mesh, uint32_t a, uint32_t b, uint32_t c) {
  const Vec3 ab = mesh_position(mesh, b) - mesh_position(mesh, a);
  const Vec3 ac = mesh_position(mesh, c) - mesh_position(mesh, a);
  const Vec3 expected = (mesh_normal(mesh, a) + mesh_normal(mesh, b) +
                         mesh_normal(mesh, c)) /
                        3.0;
  if (ab.cross(ac).dot(expected) < 0) std::swap(b, c);
  mesh.indices.insert(mesh.indices.end(), {a, b, c});
}

void finish_mesh(ScxGeometry& mesh) {
  for (int axis = 0; axis < 3; ++axis) {
    mesh.info.bbox_min[axis] = mesh.info.bbox_max[axis] = mesh.positions[axis];
    for (std::size_t i = axis + 3; i < mesh.positions.size(); i += 3) {
      mesh.info.bbox_min[axis] = std::min(mesh.info.bbox_min[axis], mesh.positions[i]);
      mesh.info.bbox_max[axis] = std::max(mesh.info.bbox_max[axis], mesh.positions[i]);
    }
  }
  ScxGroup group;
  group.index_count = static_cast<uint32_t>(mesh.indices.size());
  mesh.groups.push_back(group);
  mesh.info.ok = true;
  mesh.info.version = 4;
  mesh.info.groups = 1;
  mesh.info.vertices = static_cast<int32_t>(mesh.positions.size() / 3);
  mesh.info.triangles = static_cast<int32_t>(mesh.indices.size() / 3);
  mesh.info.error.clear();
}

struct CellKey {
  std::int64_t x = 0;
  std::int64_t z = 0;
  bool operator==(const CellKey&) const = default;
};

struct CellHash {
  std::size_t operator()(CellKey key) const noexcept {
    std::uint64_t x = static_cast<std::uint64_t>(key.x);
    std::uint64_t z = static_cast<std::uint64_t>(key.z);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    z ^= z >> 27;
    z *= 0x94d049bb133111ebULL;
    return static_cast<std::size_t>(x ^ (z + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2)));
  }
};

struct CellRange {
  std::int64_t x0 = 0, x1 = 0, z0 = 0, z1 = 0;
};

bool cell_coordinate(double value, double cell_size, std::int64_t& output) {
  const double coordinate = std::floor(value / cell_size);
  constexpr double kLimit = 9.0e18;  // Inside int64_t, including after +/- 1.
  if (!std::isfinite(coordinate) || coordinate < -kLimit || coordinate > kLimit)
    return false;
  output = static_cast<std::int64_t>(coordinate);
  return true;
}

bool cell_range(const Aabb& bounds, double cell_size, CellRange& range) {
  return cell_coordinate(bounds.minimum.x, cell_size, range.x0) &&
         cell_coordinate(bounds.maximum.x, cell_size, range.x1) &&
         cell_coordinate(bounds.minimum.z, cell_size, range.z0) &&
         cell_coordinate(bounds.maximum.z, cell_size, range.z1);
}

long double cell_count(const CellRange& range) {
  const long double width = static_cast<long double>(range.x1) - range.x0 + 1;
  const long double depth = static_cast<long double>(range.z1) - range.z0 + 1;
  return width * depth;
}

struct Shape {
  Owner owner = nullptr;
  Aabb bounds{};
  std::variant<Box, Sphere> geometry;
};

}  // namespace

bool make_box(const PartMatrix& frame, Vec3 half_extents, Box& output) noexcept {
  if (!valid_dimensions(half_extents)) return false;
  Box candidate;
  candidate.half_extents = half_extents;
  if (!frame_parts(frame, candidate.center, candidate.axes)) return false;
  output = candidate;
  return true;
}

bool make_sphere(const PartMatrix& frame, double radius, Sphere& output) noexcept {
  if (!std::isfinite(radius) || radius <= 0) return false;
  Vec3 center;
  std::array<Vec3, 3> axes;
  if (!frame_parts(frame, center, axes)) return false;
  output = {center, radius};
  return true;
}

ScxGeometry make_box_mesh(const PartMatrix& frame, Vec3 half_extents) {
  Box box;
  if (!make_box(frame, half_extents, box))
    return invalid_mesh("invalid oriented box frame or half extents");

  ScxGeometry mesh;
  mesh.position_scale = 1.0f;
  mesh.positions.reserve(24 * 3);
  mesh.normals.reserve(24 * 3);
  mesh.uvs.reserve(24 * 2);
  mesh.indices.reserve(36);
  for (int axis = 0; axis < 3; ++axis) {
    const Vec3& u = box.axes[(axis + 1) % 3];
    const Vec3& v = box.axes[(axis + 2) % 3];
    const double hu = component(box.half_extents, (axis + 1) % 3);
    const double hv = component(box.half_extents, (axis + 2) % 3);
    for (int sign : {-1, 1}) {
      const uint32_t base = static_cast<uint32_t>(mesh.positions.size() / 3);
      const Vec3 face = box.axes[axis] *
                        (sign * component(box.half_extents, axis));
      const Vec3 normal = box.axes[axis] * static_cast<double>(sign);
      const std::array<std::pair<double, double>, 4> corners = {
          std::pair{-1.0, -1.0}, std::pair{1.0, -1.0},
          std::pair{1.0, 1.0}, std::pair{-1.0, 1.0}};
      for (const auto& [su, sv] : corners) {
        append_vertex(mesh, box.center + face + u * (su * hu) + v * (sv * hv),
                      normal);
        mesh.uvs.insert(mesh.uvs.end(),
                        {static_cast<float>((su + 1) * 0.5),
                         static_cast<float>((sv + 1) * 0.5)});
      }
      append_oriented_triangle(mesh, base, base + 1, base + 2);
      append_oriented_triangle(mesh, base, base + 2, base + 3);
    }
  }
  finish_mesh(mesh);
  return mesh;
}

ScxGeometry make_sphere_mesh(const PartMatrix& frame, double radius,
                             uint32_t segments, uint32_t rings) {
  Sphere sphere;
  if (!make_sphere(frame, radius, sphere))
    return invalid_mesh("invalid sphere frame or radius");
  if (segments < 3 || rings < 3)
    return invalid_mesh("sphere requires at least 3 segments and 3 rings");
  const uint64_t vertex_count = 2ULL + static_cast<uint64_t>(rings - 1) * segments;
  if (vertex_count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
    return invalid_mesh("sphere subdivision is too large");
  // vertex_count bounds the multiplication above, so this cannot overflow.
  const uint64_t triangle_count = 2ULL * (rings - 1) * segments;
  if (triangle_count > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
      triangle_count * 3 > std::numeric_limits<uint32_t>::max())
    return invalid_mesh("sphere subdivision is too large");

  ScxGeometry mesh;
  mesh.position_scale = 1.0f;
  mesh.positions.reserve(static_cast<std::size_t>(vertex_count) * 3);
  mesh.normals.reserve(static_cast<std::size_t>(vertex_count) * 3);
  mesh.indices.reserve(static_cast<std::size_t>(triangle_count) * 3);

  std::array<Vec3, 3> axes;
  frame_parts(frame, sphere.center, axes);  // Already validated by make_sphere.
  const auto append_sphere_vertex = [&](Vec3 unit) {
    const Vec3 normal = axes[0] * unit.x + axes[1] * unit.y + axes[2] * unit.z;
    append_vertex(mesh, sphere.center + normal * radius, normal);
  };
  append_sphere_vertex({0, 1, 0});
  constexpr double kPi = 3.141592653589793238462643383279502884;
  for (uint32_t ring = 1; ring < rings; ++ring) {
    const double theta = kPi * static_cast<double>(ring) / rings;
    const double y = std::cos(theta);
    const double radial = std::sin(theta);
    for (uint32_t segment = 0; segment < segments; ++segment) {
      const double phi = 2.0 * kPi * static_cast<double>(segment) / segments;
      append_sphere_vertex(
          {radial * std::cos(phi), y, radial * std::sin(phi)});
    }
  }
  const uint32_t south = static_cast<uint32_t>(vertex_count - 1);
  append_sphere_vertex({0, -1, 0});

  const auto ring_vertex = [&](uint32_t ring, uint32_t segment) {
    return 1u + (ring - 1u) * segments + segment;
  };
  for (uint32_t segment = 0; segment < segments; ++segment) {
    const uint32_t next = (segment + 1) % segments;
    append_oriented_triangle(mesh, 0, ring_vertex(1, segment), ring_vertex(1, next));
  }
  for (uint32_t ring = 1; ring + 1 < rings; ++ring) {
    for (uint32_t segment = 0; segment < segments; ++segment) {
      const uint32_t next = (segment + 1) % segments;
      const uint32_t a = ring_vertex(ring, segment);
      const uint32_t b = ring_vertex(ring, next);
      const uint32_t c = ring_vertex(ring + 1, segment);
      const uint32_t d = ring_vertex(ring + 1, next);
      append_oriented_triangle(mesh, a, c, b);
      append_oriented_triangle(mesh, b, c, d);
    }
  }
  for (uint32_t segment = 0; segment < segments; ++segment) {
    const uint32_t next = (segment + 1) % segments;
    append_oriented_triangle(mesh, ring_vertex(rings - 1, segment), south,
                             ring_vertex(rings - 1, next));
  }
  finish_mesh(mesh);
  return mesh;
}

struct StaticIndex::Impl {
  explicit Impl(double size) : cell_size(size) {}

  double cell_size;
  std::vector<Shape> shapes;
  std::unordered_map<CellKey, std::vector<std::size_t>, CellHash> cells;
  std::vector<std::size_t> oversized;

  bool add_shape(Shape shape) {
    const std::size_t index = shapes.size();
    shapes.push_back(std::move(shape));
    CellRange range;
    if (!cell_range(shapes.back().bounds, cell_size, range) ||
        cell_count(range) > kMaxCellsPerShape) {
      oversized.push_back(index);
      return true;
    }
    for (std::int64_t x = range.x0;; ++x) {
      for (std::int64_t z = range.z0;; ++z) {
        cells[{x, z}].push_back(index);
        if (z == range.z1) break;
      }
      if (x == range.x1) break;
    }
    return true;
  }
};

StaticIndex::StaticIndex(double cell_size_metres) {
  if (!std::isfinite(cell_size_metres) || cell_size_metres <= 0)
    throw std::invalid_argument("collision cell size must be positive and finite");
  impl_ = std::make_unique<Impl>(cell_size_metres);
}

StaticIndex::~StaticIndex() = default;
StaticIndex::StaticIndex(StaticIndex&&) noexcept = default;
StaticIndex& StaticIndex::operator=(StaticIndex&&) noexcept = default;

bool StaticIndex::add(const Box& box, Owner owner) {
  if (!valid_box(box)) return false;
  return impl_->add_shape({owner, box_bounds(box), box});
}

bool StaticIndex::add(const Sphere& sphere, Owner owner) {
  if (!valid_sphere(sphere)) return false;
  return impl_->add_shape({owner, sphere_bounds(sphere), sphere});
}

bool StaticIndex::add_box(const PartMatrix& frame, Vec3 half_extents, Owner owner) {
  Box box;
  return make_box(frame, half_extents, box) && add(box, owner);
}

bool StaticIndex::add_sphere(const PartMatrix& frame, double radius, Owner owner) {
  Sphere sphere;
  return make_sphere(frame, radius, sphere) && add(sphere, owner);
}

std::optional<Contact> StaticIndex::overlap(const Box& moving, Owner ignore_owner,
                                            QueryStats* stats) const {
  if (stats) *stats = {};
  if (!valid_box(moving)) return std::nullopt;
  const Aabb query_bounds = box_bounds(moving);
  const auto& shapes = impl_->shapes;
  std::vector<std::size_t> candidates;
  const auto add_candidate = [&](std::size_t index) {
    candidates.push_back(index);
  };
  for (std::size_t index : impl_->oversized) add_candidate(index);

  CellRange range;
  if (!cell_range(query_bounds, impl_->cell_size, range) ||
      cell_count(range) > kMaxCellsPerQuery) {
    for (std::size_t index = 0; index < shapes.size(); ++index) add_candidate(index);
  } else {
    for (std::int64_t x = range.x0;; ++x) {
      for (std::int64_t z = range.z0;; ++z) {
        const auto cell = impl_->cells.find({x, z});
        if (cell != impl_->cells.end())
          for (std::size_t index : cell->second) add_candidate(index);
        if (z == range.z1) break;
      }
      if (x == range.x1) break;
    }
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
  if (stats) stats->broadphase_candidates = candidates.size();

  std::optional<Contact> best;
  for (std::size_t index : candidates) {
    const Shape& shape = shapes[index];
    if ((ignore_owner && shape.owner == ignore_owner) ||
        !overlaps(query_bounds, shape.bounds))
      continue;
    if (stats) ++stats->narrowphase_tests;
    Contact candidate;
    const bool hit = std::visit(
        [&](const auto& geometry) {
          return intersect(moving, geometry, candidate);
        },
        shape.geometry);
    if (!hit) continue;
    candidate.owner = shape.owner;
    candidate.shape_index = index;
    if (!best || candidate.penetration > best->penetration ||
        (candidate.penetration == best->penetration && index < best->shape_index))
      best = candidate;
  }
  return best;
}

std::optional<Contact> StaticIndex::overlap_box(const PartMatrix& frame,
                                                Vec3 half_extents,
                                                Owner ignore_owner,
                                                QueryStats* stats) const {
  Box moving;
  if (!make_box(frame, half_extents, moving)) {
    if (stats) *stats = {};
    return std::nullopt;
  }
  return overlap(moving, ignore_owner, stats);
}

void StaticIndex::clear() noexcept {
  impl_->shapes.clear();
  impl_->cells.clear();
  impl_->oversized.clear();
}

std::optional<Bounds> StaticIndex::bounds() const noexcept {
  if (impl_->shapes.empty()) return std::nullopt;
  Bounds result{impl_->shapes.front().bounds.minimum,
                impl_->shapes.front().bounds.maximum};
  for (const auto& shape : impl_->shapes) {
    result.minimum.x = std::min(result.minimum.x, shape.bounds.minimum.x);
    result.minimum.y = std::min(result.minimum.y, shape.bounds.minimum.y);
    result.minimum.z = std::min(result.minimum.z, shape.bounds.minimum.z);
    result.maximum.x = std::max(result.maximum.x, shape.bounds.maximum.x);
    result.maximum.y = std::max(result.maximum.y, shape.bounds.maximum.y);
    result.maximum.z = std::max(result.maximum.z, shape.bounds.maximum.z);
  }
  return result;
}

std::size_t StaticIndex::size() const noexcept { return impl_->shapes.size(); }
double StaticIndex::cell_size() const noexcept { return impl_->cell_size; }

}  // namespace slrr::collision
