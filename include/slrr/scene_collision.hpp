#pragma once

#include "slrr/part_config.hpp"
#include "slrr/scx_mesh.hpp"
#include "slrr/vehicle_dynamics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace slrr::collision {

using Owner = const void*;

// Rigid, world-space collision primitives. Box axes are its PartMatrix columns;
// half extents and all positions are in script metres.
struct Box {
  physics::Vec3 center{};
  std::array<physics::Vec3, 3> axes = {
      physics::Vec3{1, 0, 0}, physics::Vec3{0, 1, 0}, physics::Vec3{0, 0, 1}};
  physics::Vec3 half_extents{};
};

struct Sphere {
  physics::Vec3 center{};
  double radius = 0;
};

// False for non-finite/non-rigid frames or non-positive/non-finite dimensions.
// A failed construction leaves output unchanged.
bool make_box(const PartMatrix& frame, physics::Vec3 half_extents, Box& output) noexcept;
bool make_sphere(const PartMatrix& frame, double radius, Sphere& output) noexcept;

// World-space triangle meshes, with metre positions and position_scale == 1.
// Invalid input returns an empty mesh with info.ok == false and info.error set.
ScxGeometry make_box_mesh(const PartMatrix& frame, physics::Vec3 half_extents);
ScxGeometry make_sphere_mesh(const PartMatrix& frame, double radius,
                             uint32_t segments = 24, uint32_t rings = 12);

struct Contact {
  Owner owner = nullptr;
  std::size_t shape_index = 0;
  // Unit vector pointing from the static obstacle towards the moving box.
  physics::Vec3 normal{0, 1, 0};
  double penetration = 0;
  // Representative world-space point on the static obstacle, not a manifold.
  physics::Vec3 point{};
};

struct QueryStats {
  std::size_t broadphase_candidates = 0;
  std::size_t narrowphase_tests = 0;
};

struct Bounds {
  physics::Vec3 minimum{};
  physics::Vec3 maximum{};
};

// Immutable during queries. Shapes retain insertion indices until clear().
// The deepest overlap is returned; ties retain the earlier inserted shape.
class StaticIndex {
public:
  explicit StaticIndex(double cell_size_metres = 4.0);
  ~StaticIndex();

  StaticIndex(StaticIndex&&) noexcept;
  StaticIndex& operator=(StaticIndex&&) noexcept;
  StaticIndex(const StaticIndex&) = delete;
  StaticIndex& operator=(const StaticIndex&) = delete;

  bool add(const Box& box, Owner owner = nullptr);
  bool add(const Sphere& sphere, Owner owner = nullptr);
  bool add_box(const PartMatrix& frame, physics::Vec3 half_extents,
               Owner owner = nullptr);
  bool add_sphere(const PartMatrix& frame, double radius, Owner owner = nullptr);

  // A non-null ignore_owner skips every shape with that exact owner key.
  // Contact::owner still allows callers to apply their own filtering policy.
  std::optional<Contact> overlap(const Box& moving, Owner ignore_owner = nullptr,
                                 QueryStats* stats = nullptr) const;
  std::optional<Contact> overlap_box(const PartMatrix& frame,
                                     physics::Vec3 half_extents,
                                     Owner ignore_owner = nullptr,
                                     QueryStats* stats = nullptr) const;

  void clear() noexcept;
  std::size_t size() const noexcept;
  double cell_size() const noexcept;
  // Exact union of all analytic shape bounds, independent of mesh tessellation.
  std::optional<Bounds> bounds() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace slrr::collision
