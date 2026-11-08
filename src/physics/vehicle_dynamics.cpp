#include "slrr/suspension_geometry.hpp"
#include "slrr/vehicle_dynamics.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace slrr::physics {
double Vec3::length() const { return std::sqrt(dot(*this)); }
Vec3 Vec3::normalized() const {
  const double n = length();
  return n > 1e-12 ? *this / n : Vec3{};
}
bool Vec3::finite() const {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}
Quaternion Quaternion::operator*(Quaternion b) const {
  return {w * b.x + x * b.w + y * b.z - z * b.y,
          w * b.y - x * b.z + y * b.w + z * b.x,
          w * b.z + x * b.y - y * b.x + z * b.w,
          w * b.w - x * b.x - y * b.y - z * b.z};
}
Quaternion Quaternion::normalized() const {
  double n = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(n > 1e-12) || !std::isfinite(n))
    throw std::runtime_error("invalid rigid orientation");
  return {x / n, y / n, z / n, w / n};
}
Vec3 Quaternion::rotate(Vec3 v) const {
  Vec3 axis{x, y, z};
  Vec3 t = axis.cross(v) * 2;
  return v + t * w + axis.cross(t);
}
Vec3 Quaternion::inverse_rotate(Vec3 v) const {
  return Quaternion{-x, -y, -z, w}.rotate(v);
}
void RigidBody::configure(double kilograms, Vec3 lo, Vec3 hi, Vec3 com) {
  if (!(kilograms > 0) || !std::isfinite(kilograms) || !lo.finite() ||
      !hi.finite() || !com.finite() || hi.x <= lo.x || hi.y <= lo.y ||
      hi.z <= lo.z)
    throw std::invalid_argument("invalid rigid body mass/bounds");
  mass = kilograms;
  minimum = lo;
  maximum = hi;
  center = com;
  Vec3 s = hi - lo;
  inverse_inertia_ = {12 / (mass * (s.y * s.y + s.z * s.z)),
                      12 / (mass * (s.x * s.x + s.z * s.z)),
                      12 / (mass * (s.x * s.x + s.y * s.y))};
}
void RigidBody::set_frame(const Matrix &m) {
  for (float v : m)
    if (!std::isfinite(v))
      throw std::invalid_argument("invalid rigid frame");
  const double trace = m[0] + m[5] + m[10];
  if (trace > 0) {
    double s = std::sqrt(trace + 1) * 2;
    orientation = {(m[6] - m[9]) / s, (m[8] - m[2]) / s, (m[1] - m[4]) / s,
                   s / 4};
  } else if (m[0] > m[5] && m[0] > m[10]) {
    double s = std::sqrt(1 + m[0] - m[5] - m[10]) * 2;
    orientation = {s / 4, (m[4] + m[1]) / s, (m[8] + m[2]) / s,
                   (m[6] - m[9]) / s};
  } else if (m[5] > m[10]) {
    double s = std::sqrt(1 + m[5] - m[0] - m[10]) * 2;
    orientation = {(m[4] + m[1]) / s, s / 4, (m[9] + m[6]) / s,
                   (m[8] - m[2]) / s};
  } else {
    double s = std::sqrt(1 + m[10] - m[0] - m[5]) * 2;
    orientation = {(m[8] + m[2]) / s, (m[9] + m[6]) / s, s / 4,
                   (m[1] - m[4]) / s};
  }
  orientation = orientation.normalized();
  position = {m[0] * center.x + m[4] * center.y + m[8] * center.z + m[12],
              m[1] * center.x + m[5] * center.y + m[9] * center.z + m[13],
              m[2] * center.x + m[6] * center.y + m[10] * center.z + m[14]};
  velocity = momentum = force_ = torque_ = {};
}
Matrix RigidBody::frame() const {
  Vec3 x = orientation.rotate({1, 0, 0}), y = orientation.rotate({0, 1, 0}),
       z = orientation.rotate({0, 0, 1});
  Vec3 p = position - orientation.rotate(center);
  return {float(x.x), float(x.y), float(x.z), 0,
          float(y.x), float(y.y), float(y.z), 0,
          float(z.x), float(z.y), float(z.z), 0,
          float(p.x), float(p.y), float(p.z), 1};
}
Vec3 RigidBody::inverse_world_inertia(Vec3 v) const {
  Vec3 a = orientation.inverse_rotate(v);
  return orientation.rotate({a.x * inverse_inertia_.x, a.y * inverse_inertia_.y,
                             a.z * inverse_inertia_.z});
}
Vec3 RigidBody::omega() const { return inverse_world_inertia(momentum); }
Vec3 RigidBody::point_velocity(Vec3 p) const {
  return velocity + omega().cross(p - position);
}
void RigidBody::add_force(Vec3 f, Vec3 p) {
  force_ += f;
  torque_ += (p - position).cross(f);
}
void RigidBody::impulse(Vec3 j, Vec3 p) {
  velocity += j / mass;
  momentum += (p - position).cross(j);
}
void RigidBody::integrate(double dt) {
  if (!std::isfinite(dt) || dt <= 0)
    throw std::invalid_argument("invalid physics dt");
  velocity += force_ * (dt / mass);
  velocity.y -= 9.81 * dt;
  momentum = (momentum + torque_ * dt) * std::exp(-.08 * dt);
  position += velocity * dt;
  Vec3 w = omega();
  double angle = w.length() * dt;
  if (angle > 1e-12) {
    Vec3 axis = w.normalized() * std::sin(angle * .5);
    orientation =
        (Quaternion{axis.x, axis.y, axis.z, std::cos(angle * .5)} * orientation)
            .normalized();
  }
  force_ = torque_ = {};
  if (!position.finite() || !velocity.finite() || !momentum.finite())
    throw std::runtime_error("non-finite rigid state");
}
void RigidBody::contact(Vec3 p, Vec3 normal, double restitution,
                        double friction) {
  Vec3 n = normal.normalized(), r = p - position, v = point_velocity(p);
  double vn = v.dot(n);
  if (vn >= 0)
    return;
  double denom = 1 / mass + n.dot(inverse_world_inertia(r.cross(n)).cross(r));
  if (!(denom > 0))
    return;
  double magnitude = -(1 + (vn < -.8 ? restitution : 0)) * vn / denom;
  impulse(n * magnitude, p);
  Vec3 tangent = v - n * vn;
  double speed = tangent.length();
  if (speed > 1e-8) {
    tangent = tangent / speed;
    double d = 1 / mass +
               tangent.dot(inverse_world_inertia(r.cross(tangent)).cross(r));
    impulse(tangent * (-std::min(friction * magnitude, speed / d)), p);
  }
}
std::array<Vec3, 8> RigidBody::corners() const {
  std::array<Vec3, 8> out;
  unsigned i = 0;
  for (double x : {minimum.x, maximum.x})
    for (double y : {minimum.y, maximum.y})
      for (double z : {minimum.z, maximum.z})
        out[i++] = position + orientation.rotate(Vec3{x, y, z} - center);
  return out;
}
Vec3 body_point(const RigidBody &body, Vec3 local) {
  return body.position + body.orientation.rotate(local - body.center);
}
Vec3 ray_center(const WheelConfig &w, double length) {
  return w.center + w.down.normalized() * (length - w.anchor_length());
}
bool hinge_hub(const WheelConfig &w, Vec3 ray, Vec3 &hub) {
  if (!(w.arm_length > 0)) {
    hub = ray;
    return ray.finite();
  }
  CornerGeometry geometry;
  geometry.reference = w.center;
  const Vec3 pivot = w.arm_pivot - w.center, axis = w.arm_axis;
  geometry.arm = {float(w.arm_length), float(pivot.x), float(pivot.y),
                  float(pivot.z),       float(axis.x),  float(axis.y),
                  float(axis.z)};
  geometry.hub = w.hub;
  geometry.alignment = w.hub_alignment;
  if (!w.has_hub)
    geometry.hub[0] = -1;
  CornerFrames frames;
  if (!solve_corner_geometry(geometry, ray, frames))
    return false;
  hub = frames.hub_origin;
  return hub.finite();
}
double suspension_force(double length, double speed, double rest, double max,
                        double k, double bump, double rebound);
// Spring force stays on the 1-DOF length. The chassis feels it at the
// hinge-projected patch, which is the external support once the arm and
// the unsprung mass are internal. A massless ray (no arm, no type mass)
// never enters here.
void solve_linked(RigidBody &body, const WheelConfig &w, WheelState &s,
                  const Terrain &terrain, double ceiling, double dt) {
  const Vec3 down = w.down.normalized();
  const Vec3 hard = body_point(body, w.center - down * w.anchor_length());
  if (!s.seeded) {
    s.length = std::clamp(w.rest, w.minimum, w.maximum);
    s.speed = 0;
    s.seeded = true;
  }
  auto project = [&](double length, Vec3 &local, Vec3 &world) {
    local = ray_center(w, length);
    hinge_hub(w, local, local);
    world = body_point(body, local);
  };
  Vec3 hub, hub_w;
  project(s.length, hub, hub_w);
  GroundHit hit;
  bool ground =
      down.y < -.05 &&
      terrain(hub_w.x, hub_w.z, std::max(ceiling, hub_w.y + 1), hit);
  auto tyre_error = [&](double value, Vec3 &local, Vec3 &world, GroundHit &sample,
                        bool &hit_ground) {
    project(value, local, world);
    hit_ground = down.y < -.05 && terrain(world.x, world.z,
                                          std::max(ceiling, world.y + 1),
                                          sample);
    if (!hit_ground)
      return 0.0;
    const Vec3 n = sample.normal.normalized();
    return n.dot(Vec3{world.x, sample.height, world.z} -
                 (world - n * w.radius));
  };
  double speed = s.speed;
  double length = s.length;
  bool contact = false;
  if (!(w.unsprung > 0)) {
    if (ground && down.y < -.05) {
      const double raw = (hard.y - hit.height - w.radius) / -down.y;
      length = raw;
      contact = raw < w.maximum;
    }
  } else {
    const Vec3 gravity{0, -9.81, 0};
    const double hold =
        suspension_force(length, speed, w.rest, w.maximum, w.spring,
                         w.bump_damping, w.rebound_damping);
    const double free_speed =
        speed + (hold + w.unsprung * gravity.dot(down)) / w.unsprung * dt;
    const double free_length = length + free_speed * dt;
    double seated = std::clamp(length, w.minimum, w.maximum);
    bool landed = false;
    if (down.y < -.05) {
      for (int i = 0; i < 8; ++i) {
        Vec3 local, world, nudged, nudged_w;
        GroundHit sample;
        bool hit_ground = false;
        const double error =
            tyre_error(seated, local, world, sample, hit_ground);
        if (!hit_ground || std::fabs(error) < 1e-4)
          break;
        const Vec3 n = sample.normal.normalized();
        const double probe =
            seated < w.maximum - 1e-4 ? seated + 1e-3 : seated - 1e-3;
        project(probe, nudged, nudged_w);
        const double slope = (nudged_w.dot(n) - world.dot(n)) / (probe - seated);
        if (std::fabs(slope) < 1e-5)
          break;
        const double next =
            std::clamp(seated + error / slope, w.minimum, w.maximum);
        if (next == seated)
          break;
        seated = next;
      }
      Vec3 local, world;
      GroundHit sample;
      bool hit_ground = false;
      const double error = tyre_error(seated, local, world, sample, hit_ground);
      // Free motion that would put the tyre through the surface is replaced
      // by the seated length. A wheel still in the air keeps its integral.
      landed = hit_ground && error >= -2e-3 && free_length >= seated - 1e-4;
    }
    if (landed) {
      length = seated;
      speed = 0;
      contact = true;
    } else {
      length = free_length;
      speed = free_speed;
    }
  }
  length = std::clamp(length, w.minimum, w.maximum);
  project(length, hub, hub_w);
  ground = down.y < -.05 &&
           terrain(hub_w.x, hub_w.z, std::max(ceiling, hub_w.y + 1), hit);
  const Vec3 normal = ground ? hit.normal.normalized() : Vec3{0, 1, 0};
  const double rate = speed - body.point_velocity(hub_w).dot(down);
  double force = suspension_force(length, rate, w.rest, w.maximum, w.spring,
                                  w.bump_damping, w.rebound_damping);
  force += std::max(0.0, w.minimum - length) * w.spring * 8;
  force = std::clamp(force, 0.0, body.mass * 9.81 * 3);
  // The spring is internal: it loads the chassis along the strut whether or
  // not the tyre is touching. Ground load stays on the unsprung mass.
  body.add_force(down * -force, hub_w);
  if (length <= w.minimum && speed < 0)
    speed = 0;
  if (length >= w.maximum && speed > 0)
    speed = 0;
  s.length = length;
  s.speed = speed;
  s.force = ground ? force : 0;
  s.contact = contact && ground;
  s.normal = normal;
  hinge_hub(w, ray_center(w, s.length), s.hub);
  s.center = body_point(body, s.hub);
  s.patch = s.center - normal * w.radius;
  if (!s.contact)
    return;
  Vec3 velocity = body.point_velocity(s.patch);
  Vec3 tangent = velocity - normal * velocity.dot(normal);
  const double slip = tangent.length();
  if (slip <= 1e-9)
    return;
  tangent = tangent / slip;
  const Vec3 arm = s.patch - body.position;
  const double inverse =
      1 / body.mass +
      tangent.dot(body.inverse_world_inertia(arm.cross(tangent)).cross(arm));
  const double amount = std::min(s.force * .8, slip / (inverse * dt * 4));
  body.add_force(tangent * (-amount), s.patch);
}
double suspension_force(double length, double speed, double rest, double max,
                        double k, double bump, double rebound) {
  if (length >= max)
    return 0;
  const double velocity = 2 * speed;
  return std::max(
      0.0,
      -velocity * (velocity >= 0 ? (rebound > 0 ? rebound : .7 * bump) : bump) -
          (length - rest) * k);
}
void VehicleDynamics::step(const Terrain &terrain) {
  // Recover initial overlaps through physical contacts, not a floor teleport.
  double ceiling = body.position.y;
  for (Vec3 p : body.corners())
    ceiling = std::max(ceiling, p.y);
  for (size_t i = 0; i < wheels.size(); ++i) {
    const auto &w = wheels[i];
    auto &s = state[i];
    s.contact = false;
    s.force = 0;
    if (!s.seeded)
      s.length = w.maximum;
    if (!w.enabled)
      continue;
    if (w.rigid_contact) {
      s.length = w.minimum;
      s.speed = 0;
      s.seeded = true;
      continue; // The rigid contact pass below supports the tyre.
    }
    if (w.linked()) {
      solve_linked(body, w, s, terrain, ceiling, timestep);
      continue;
    }
    s.length = w.maximum;
    Vec3 down = body.orientation.rotate(w.down.normalized());
    Vec3 hard =
        body.position +
        body.orientation.rotate(
            w.center - w.down.normalized() * w.anchor_length() - body.center);
    GroundHit hit;
    if (down.y >= -.05 ||
        !terrain(hard.x, hard.z, std::max(ceiling, hard.y), hit))
      continue;
    const double length = (hard.y - hit.height - w.radius) / -down.y;
    s.length = std::clamp(length, w.minimum, w.maximum);
    s.contact = length < w.maximum;
    if (!s.contact)
      continue;
    s.normal = hit.normal.normalized();
    double force = suspension_force(
        length, body.point_velocity(hard).dot(s.normal), w.rest, w.maximum,
        w.spring, w.bump_damping, w.rebound_damping);
    force += std::max(0.0, w.minimum - length) * w.spring * 8;
    s.force = std::clamp(force, 0.0, body.mass * 9.81 * 3);
    body.add_force(s.normal * s.force, hard);
    // Parked tyre friction consumes kinetic motion, bounded by normal load.
    Vec3 point = hard + down * length + Vec3{0, -w.radius, 0};
    Vec3 v = body.point_velocity(point),
         tangent = v - s.normal * v.dot(s.normal);
    double speed = tangent.length();
    if (speed > 1e-9) {
      Vec3 direction = tangent / speed, r = point - body.position;
      double inverse =
          1 / body.mass +
          direction.dot(
              body.inverse_world_inertia(r.cross(direction)).cross(r));
      double amount = std::min(s.force * .8, speed / (inverse * timestep * 4));
      body.add_force(direction * (-amount), point);
    }
  }
  body.integrate(timestep);
  for (int iteration = 0; iteration < 4; ++iteration) {
    double depth = 0;
    Vec3 correction;
    auto solve = [&](Vec3 point, double restitution) {
      GroundHit ground;
      if (!terrain(point.x, point.z, ceiling, ground))
        return false;
      double overlap = ground.height - point.y;
      if (overlap <= 0)
        return false;
      body.contact(point, ground.normal, restitution);
      if (overlap > depth) {
        depth = overlap;
        correction = ground.normal.normalized();
      }
      return true;
    };
    for (Vec3 point : body.corners())
      solve(point, .12);
    // A fully compressed wheel is also a unilateral support constraint.
    // Without this hard stop an initially buried tyre remains underground
    // while the penalty spring pumps enough energy to launch the chassis.
    for (size_t i = 0; i < wheels.size(); ++i) {
      const auto &wheel = wheels[i];
      if (!wheel.enabled || (wheel.linked() && !wheel.rigid_contact))
        continue;
      const auto down = wheel.down.normalized();
      const auto local =
          wheel.center + down * (wheel.minimum - wheel.anchor_length());
      const auto point = body.position +
                         body.orientation.rotate(local - body.center) -
                         Vec3{0, wheel.radius, 0};
      if (solve(point, 0) && wheel.rigid_contact)
        state[i].contact = true;
    }
    // One patch correction per iteration prevents duplicate corners pumping
    // position/energy; contact impulses still act at their individual points.
    if (depth > .001)
      body.position += correction * ((depth - .001) * .6);
    else
      break;
  }
  sync_wheels();
}
void VehicleDynamics::sync_wheels() {
  for (size_t i = 0; i < wheels.size(); ++i) {
    const auto &w = wheels[i];
    auto &s = state[i];
    Vec3 local =
        w.center + w.down.normalized() * (s.length - w.anchor_length());
    s.center = body.position + body.orientation.rotate(local - body.center);
  }
}
unsigned VehicleDynamics::advance(double seconds, const Terrain &terrain) {
  if (!std::isfinite(seconds) || seconds < 0)
    throw std::invalid_argument("invalid elapsed physics time");
  accumulator_ += seconds;
  unsigned count = 0;
  constexpr unsigned max_steps =
      (frequency + update_frequency - 1) / update_frequency;
  while (accumulator_ + timestep * 1e-10 >= timestep && count < max_steps) {
    step(terrain);
    accumulator_ = std::max(0.0, accumulator_ - timestep);
    ++ticks;
    ++count;
  }
  return count;
}
void VehicleDynamics::reset_clock() {
  accumulator_ = 0;
  ticks = 0;
}
void VehicleDynamics::release_wheels() {
  for (auto &wheel : state)
    wheel = {};
}
uint64_t VehicleDynamics::backlog() const {
  return static_cast<uint64_t>((accumulator_ + timestep * 1e-10) / timestep);
}
} // namespace slrr::physics
