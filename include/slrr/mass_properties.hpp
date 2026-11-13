#pragma once

#include "slrr/vehicle_dynamics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace slrr::physics {

// Rigid mass distribution in one frame: total mass, centre of mass and the
// inertia tensor about that centre (kg m^2, row-major, symmetric), all in the
// axes of the frame the properties are expressed in.
//
// This is a host model, not recovered stock behaviour: the original game sums
// point masses (EXTERNAL_RESEARCH §4). OpenSLRR spreads each CFG `body` mass
// over that body's own shape (box, sphere or closed phys hull) and adds the
// pieces with the parallel-axis theorem. See ENGINE_CONTRACT §11.
struct MassProperties {
  double mass = 0;
  Vec3 center;
  std::array<double, 9> inertia{};
  bool empty() const { return !(mass > 0); }
  // Moments about the centre along this frame's own axes (the tensor
  // diagonal). Products of inertia are left out by callers that need a
  // diagonal body-axis inertia, such as RigidBody.
  Vec3 moments() const { return {inertia[0], inertia[4], inertia[8]}; }
};

MassProperties point_mass(double mass, Vec3 at);
// Uniform solid box with half extents `half`, axis-aligned in this frame.
MassProperties box_mass(double mass, Vec3 center, Vec3 half);
MassProperties sphere_mass(double mass, Vec3 center, double radius);
// Uniform solid enclosed by a closed triangle mesh. `positions` holds xyz
// triples scaled by `scale` (SCX centimetres use 0.01). Winding may be
// inward or outward; vertices may be split by normal or UV (equal positions
// are welded at 0.1 mm). Returns false when the triangles are not a closed,
// consistently wound surface around a positive volume: the caller then falls
// back to the mesh bounds.
bool mesh_mass(double mass, const float* positions, size_t floats, double scale,
               const uint32_t* indices, size_t index_count, MassProperties& out);
// Express `local` in the parent frame `frame` (column-major rigid matrix:
// columns 0..2 are the local axes, 12..14 the origin).
MassProperties transformed(const MassProperties& local, const Matrix& frame);
// Add `part` to `sum` (both in the same frame), moving both inertias to the
// combined centre with the parallel-axis theorem.
void accumulate(MassProperties& sum, const MassProperties& part);

}  // namespace slrr::physics
