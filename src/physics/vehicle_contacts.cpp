#include "slrr/vehicle_contacts.hpp"

#include "slrr/scene_collision.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace slrr::physics {
namespace {

// Adapted from nedobylskiy/SLRR_rev v221, commit
// 7441a697f3b7b2d0ff7ddbe3f20f2e29a03165aa:
// src/engine/vehicle-pair-test.ts vehiclePairContact and
// src/engine/vehicle-physics-world.ts shared-tick positional correction.
// These are the upstream host model, not recovered original-game constants.
// OpenSLRR reuses its two-body RigidBody::contact response (including its
// post-normal-impulse friction), captures the actual impulse, and adds strict
// transactional validation and a bounded translational sweep.
using collision::Box;
using collision::Contact;

constexpr unsigned kSweepSamples = 64;
constexpr unsigned kSweepBisections = 12;

void require(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}

void finite_result(bool condition) {
  if (!condition) throw std::runtime_error("non-finite vehicle contact result");
}

double length(Vec3 v) { return std::hypot(v.x, v.y, v.z); }

Box body_box(const RigidBody& body) {
  require(std::isfinite(body.mass) && body.mass > 0 &&
              std::isfinite(1 / body.mass) && 1 / body.mass > 0,
          "invalid vehicle contact mass");
  require(body.position.finite() && body.velocity.finite() &&
              body.momentum.finite() && body.center.finite() &&
              body.minimum.finite() && body.maximum.finite(),
          "invalid vehicle contact state");
  require(body.maximum.x > body.minimum.x && body.maximum.y > body.minimum.y &&
              body.maximum.z > body.minimum.z,
          "invalid vehicle contact bounds");
  const auto& q = body.orientation;
  const double norm2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  require(std::isfinite(norm2) && std::abs(norm2 - 1) <= 2e-6,
          "invalid vehicle contact orientation");

  Box box;
  box.half_extents = body.maximum * .5 - body.minimum * .5;
  require(box.half_extents.finite() && box.half_extents.x > 0 &&
              box.half_extents.y > 0 && box.half_extents.z > 0 &&
              std::isfinite(length(box.half_extents)),
          "invalid vehicle contact dimensions");
  const Vec3 local_center = body.minimum * .5 + body.maximum * .5;
  box.center = body.position + q.rotate(local_center - body.center);
  require(box.center.finite(), "invalid vehicle contact box centre");
  box.axes = {q.rotate({1, 0, 0}), q.rotate({0, 1, 0}), q.rotate({0, 0, 1})};
  for (Vec3 axis : box.axes) {
    const Vec3 inverse = body.inverse_world_inertia(axis);
    require(axis.finite() && inverse.finite() && inverse.dot(axis) > 0 &&
                std::isfinite(inverse.dot(axis)),
            "invalid vehicle contact inertia");
  }
  require(body.omega().finite(), "invalid vehicle contact angular velocity");
  return box;
}

std::array<double, 25> order_key(const RigidBody& body, const Box& box) {
  return {box.center.x, box.center.y, box.center.z,
          box.half_extents.x, box.half_extents.y, box.half_extents.z,
          box.axes[0].x, box.axes[0].y, box.axes[0].z,
          box.axes[1].x, box.axes[1].y, box.axes[1].z,
          box.axes[2].x, box.axes[2].y, box.axes[2].z,
          body.velocity.x, body.velocity.y, body.velocity.z,
          body.momentum.x, body.momentum.y, body.momentum.z,
          body.mass, body.center.x, body.center.y, body.center.z};
}

std::optional<Contact> pair_overlap(const RigidBody& a, const RigidBody& b,
                                    const Box& box_a, const Box& box_b) {
  // Allocation-free broad rejection, independent of a body's local COM offset.
  const double reach = length(box_a.half_extents) + length(box_b.half_extents);
  const double distance = length(box_a.center - box_b.center);
  finite_result(std::isfinite(reach) && std::isfinite(distance));
  if (distance > reach + 1e-9 * std::max(1.0, reach)) return std::nullopt;

  // SAT's equal-depth ties otherwise depend on which body's axes are tested
  // first. Use geometry/state, never addresses or creation order, to fix that
  // order. Fully identical coincident resting states have no distinguishable
  // side; only that intrinsically ambiguous case retains a fixed axis sign.
  const bool reverse_order = order_key(b, box_b) < order_key(a, box_a);
  const Box& first = reverse_order ? box_b : box_a;
  const Box& second = reverse_order ? box_a : box_b;
  auto hit = collision::overlap_boxes(first, second);
  if (!hit) return std::nullopt;
  const auto reverse = collision::overlap_boxes(second, first);
  finite_result(reverse.has_value());
  hit->point = hit->point * .5 + reverse->point * .5;
  if (reverse_order) hit->normal = -hit->normal;
  finite_result(hit->point.finite() && hit->normal.finite() &&
                std::isfinite(hit->penetration) && hit->penetration >= 0);
  return hit;
}

std::array<double, 2> inverse_mass_weights(const RigidBody& a, const RigidBody& b) {
  const double inverse_a = 1 / a.mass, inverse_b = 1 / b.mass;
  // Scale first, avoiding overflow for individually valid very small masses.
  const double scale = std::max(inverse_a, inverse_b);
  const double sum = inverse_a / scale + inverse_b / scale;
  return {inverse_a / scale / sum, inverse_b / scale / sum};
}

std::optional<VehicleContact> resolve(RigidBody& a, RigidBody& b,
                                       const Box& box_a, const Box& box_b) {
  const auto hit = pair_overlap(a, b, box_a, box_b);
  if (!hit) return std::nullopt;
  VehicleContact result;
  result.point = hit->point;
  result.normal = hit->normal;
  result.penetration = hit->penetration;
  const Vec3 relative = a.point_velocity(result.point) - b.point_velocity(result.point);
  finite_result(relative.finite());
  const double closing = -relative.dot(result.normal);
  finite_result(std::isfinite(closing));
  result.speed = std::max(0.0, closing);

  // A failure must leave both owners untouched; their runtime contains it.
  const auto response = [&](const RigidBody& body, Vec3 direction) {
    const Vec3 arm = result.point - body.position;
    const Vec3 change = direction / body.mass +
                       body.inverse_world_inertia(arm.cross(direction)).cross(arm);
    finite_result(arm.finite() && change.finite());
    return change;
  };
  const Vec3 normal_response = response(a, result.normal) + response(b, result.normal);
  const double normal_inverse = result.normal.dot(normal_response);
  finite_result(normal_response.finite() && std::isfinite(normal_inverse) && normal_inverse > 0);
  if (result.speed > 0) {
    const double normal_impulse = result.speed * (result.speed > .8 ? 1.08 : 1) / normal_inverse;
    const Vec3 post_normal = relative + normal_response * normal_impulse;
    const Vec3 tangent = post_normal - result.normal * post_normal.dot(result.normal);
    finite_result(std::isfinite(normal_impulse) && post_normal.finite() && tangent.finite());
    const double tangent_speed = length(tangent);
    finite_result(std::isfinite(tangent_speed));
    if (tangent_speed > 1e-8) {
      const Vec3 direction = tangent / tangent_speed;
      const double inverse = direction.dot(response(a, direction) + response(b, direction));
      finite_result(std::isfinite(inverse) && inverse > 0);
    }
  }
  RigidBody next_a = a, next_b = b;
  const double applied_normal = next_a.contact(result.point, result.normal, .08, .6, &next_b);
  finite_result(std::isfinite(applied_normal));
  result.impulse = (next_a.velocity - a.velocity) * a.mass;
  result.normal_impulse = std::max(0.0, result.impulse.dot(result.normal));
  const double correction = std::min(.2, std::max(0.0, result.penetration - .002) * .6);
  const auto weights = inverse_mass_weights(a, b);
  next_a.position += result.normal * (correction * weights[0]);
  next_b.position += result.normal * (-correction * weights[1]);
  finite_result(result.impulse.finite() && std::isfinite(result.normal_impulse));
  body_box(next_a);
  body_box(next_b);
  a = next_a;
  b = next_b;
  return result;
}

}  // namespace

void validate_vehicle_contact_body(const RigidBody& body) {
  body_box(body);
}

std::optional<VehicleContact> resolve_vehicle_contact(RigidBody& a, RigidBody& b) {
  require(&a != &b, "vehicle contact requires distinct bodies");
  const Box box_a = body_box(a), box_b = body_box(b);
  return resolve(a, b, box_a, box_b);
}

std::optional<VehicleContact> resolve_swept_vehicle_contact(
    RigidBody& a, RigidBody& b, const RigidBody& previous_a,
    const RigidBody& previous_b) {
  require(&a != &b, "vehicle contact requires distinct bodies");
  const Box box_a = body_box(a), box_b = body_box(b);
  body_box(previous_a);
  body_box(previous_b);
  const Vec3 travel_a = a.position - previous_a.position;
  const Vec3 travel_b = b.position - previous_b.position;
  const Vec3 relative_travel = travel_a - travel_b;
  const double distance = length(relative_travel);
  finite_result(travel_a.finite() && travel_b.finite() &&
                relative_travel.finite() && std::isfinite(distance));
  if (distance <= 1e-10) return resolve(a, b, box_a, box_b);

  const auto overlap_at = [&](double fraction) {
    Box sample_a = box_a, sample_b = box_b;
    sample_a.center = box_a.center - travel_a * (1 - fraction);
    sample_b.center = box_b.center - travel_b * (1 - fraction);
    finite_result(sample_a.center.finite() && sample_b.center.finite());
    return collision::overlap_boxes(sample_a, sample_b).has_value();
  };
  if (overlap_at(0)) return resolve(a, b, box_a, box_b);

  // Reject a distant sweep with a segment/sphere test before any SAT sampling.
  const Vec3 start = (box_a.center - travel_a) - (box_b.center - travel_b);
  const Vec3 direction = relative_travel / distance;
  const double projection = start.dot(direction);
  const Vec3 nearest = start + direction * std::clamp(-projection, 0.0, distance);
  const double reach = length(box_a.half_extents) + length(box_b.half_extents);
  finite_result(start.finite() && std::isfinite(projection) && nearest.finite() &&
                std::isfinite(reach));
  if (length(nearest) > reach + 1e-9 * std::max(1.0, reach)) return std::nullopt;

  const double spacing = .5 * std::min({box_a.half_extents.x, box_a.half_extents.y,
                                      box_a.half_extents.z, box_b.half_extents.x,
                                      box_b.half_extents.y, box_b.half_extents.z});
  const double requested = std::ceil(distance / spacing);
  const unsigned samples = requested >= kSweepSamples
                               ? kSweepSamples
                               : std::max(1u, static_cast<unsigned>(requested));
  double clear = 0;
  for (unsigned i = 1; i <= samples; ++i) {
    double contact = static_cast<double>(i) / samples;
    if (!overlap_at(contact)) {
      clear = contact;
      continue;
    }
    for (unsigned step = 0; step < kSweepBisections; ++step) {
      const double middle = (clear + contact) * .5;
      if (overlap_at(middle)) contact = middle;
      else clear = middle;
    }
    const auto weights = inverse_mass_weights(a, b);
    RigidBody next_a = a, next_b = b;
    // Preserve common translation and current angular/linear dynamics. Only
    // relative positional movement beyond the first contact is discarded.
    next_a.position += relative_travel * (-(1 - contact) * weights[0]);
    next_b.position += relative_travel * ((1 - contact) * weights[1]);
    auto result = resolve_vehicle_contact(next_a, next_b);
    finite_result(result.has_value());
    a = next_a;
    b = next_b;
    return result;
  }
  return std::nullopt;
}

}  // namespace slrr::physics
