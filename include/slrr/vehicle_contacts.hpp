#pragma once

#include "slrr/vehicle_dynamics.hpp"

#include <optional>

namespace slrr::physics {

// Shared chassis OBB contact, in script metres/kilograms/seconds. The normal
// points from b towards a; impulse is the actual total linear impulse applied
// to a (including friction), with its opposite applied to b. No damage, sleep,
// events, pair enumeration or world ownership lives in this kernel.
struct VehicleContact {
  Vec3 point, normal, impulse;
  double speed = 0;           // Pre-response relative normal approach speed.
  double normal_impulse = 0;  // Normal component of the actual impulse on a.
  double penetration = 0;    // Uncorrected overlap depth.
};

// Non-mutating validation shared with the resolvers. Throws invalid_argument
// for invalid mass, bounds, rigid state, orientation or inertia. Owners may use
// it to isolate an invalid passive participant after a pair-resolution failure.
void validate_vehicle_contact_body(const RigidBody& body);

// Calls RigidBody::contact(point, normal, .08, .6, &b) once, using the average
// of the reciprocal SAT points. Separates by inverse mass, with the SLRR_rev
// .002 m slop, .6 correction fraction and .2 m cap. Resting/separating overlaps
// still return contact metadata and receive positional correction, not a kick.
// Invalid bodies (including unconfigured bounds, non-unit orientations and
// aliasing a/b) throw std::invalid_argument; failed arithmetic throws without
// changing either body. A valid separated pair returns nullopt.
std::optional<VehicleContact> resolve_vehicle_contact(RigidBody& a, RigidBody& b);

// Translational high-relative-speed guard for one shared substep. Saved bodies
// supply previous COM positions; both current orientations and shapes are held
// fixed throughout the sweep. Starts already overlapping use the discrete path
// instead, so a separating pair is never rewound into an earlier overlap.
// Up to 64 samples (spacing targeted at half the smallest half extent), then
// 12 bisections locate the first sampled contact. This is bounded protection,
// not exact CCD: sufficiently thin/grazing contacts or motion beyond the sample
// cap can be missed, and rotational-only tunnelling is not covered.
// Only relative positional travel is rewound, preserving the current pair COM,
// velocities, angular momenta and orientations; then the discrete resolver runs
// once. All four input bodies are validated before either live body is changed.
std::optional<VehicleContact> resolve_swept_vehicle_contact(
    RigidBody& a, RigidBody& b, const RigidBody& previous_a,
    const RigidBody& previous_b);

}  // namespace slrr::physics
