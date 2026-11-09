#include "slrr/suspension_geometry.hpp"
#include "slrr/vehicle_dynamics.hpp"
#include "slrr/sl2_pacejka.hpp"
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
// Integrate travel along the strut and transmit ground support at its mount.
void solve_linked(RigidBody &body, const WheelConfig &w, WheelState &s,
                  const Terrain &terrain, double ceiling, double dt,
                  bool parked_friction) {
  const Vec3 local_down = w.down.normalized();
  const Vec3 down = body.orientation.rotate(local_down);
  const Vec3 hard = body_point(body, w.center - local_down * w.anchor_length());
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
  const double rate = speed + body.point_velocity(hard).dot(normal);
  double force = suspension_force(length, rate, w.rest, w.maximum, w.spring,
                                  w.bump_damping, w.rebound_damping);
  force = std::clamp(force, 0.0, body.mass * 9.81 * 3);
  if (length <= w.minimum && speed < 0)
    speed = 0;
  if (length >= w.maximum && speed > 0)
    speed = 0;
  s.length = length;
  s.speed = speed;
  s.contact = contact && ground;
  s.force = s.contact ? force : 0;
  if (s.contact) body.add_force(normal * s.force, hard);
  s.normal = normal;
  hinge_hub(w, ray_center(w, s.length), s.hub);
  s.center = body_point(body, s.hub);
  s.patch = s.center - normal * w.radius;
  if (!s.contact)
    return;
  if (!parked_friction) return;
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
namespace {
constexpr double rpm_per_rad = 9.54929658551372;
constexpr double wheel_inertia = 1.6;

Vec3 wheel_forward(const RigidBody& body, double steer) {
  return body.orientation.rotate({-std::sin(steer), 0, -std::cos(steer)});
}

// SLRR_rev cf4fd30, native-vehicle-physics.ts engineLimiter.
double limiter_factor(double rpm, double previous, double limit) {
  const double predicted = 2 * rpm - previous;
  if (predicted <= limit)
    return rpm > limit ? (limit - predicted) / (rpm - predicted) : 1;
  return rpm > limit ? 0 : (limit - rpm) / (predicted - rpm);
}

double relaxed_slip(double previous, double speed, double length, double damping) {
  return std::clamp((previous + speed * VehicleDynamics::timestep / length) /
                    (1 + 500 * damping * VehicleDynamics::timestep), -1.0, 1.0);
}

double longitudinal_slip(double previous, double rolling, double road, double blend) {
  const double target = road > .1 ? rolling / road : 0;
  return std::lerp(relaxed_slip(previous, rolling, .3, .1), target, blend);
}
}  // namespace

// SLRR_rev cf4fd30, MechanicalSystem.advance. Host gear 7 denotes reverse.
void VehicleDynamics::update_gear(double rpm, double shaft, double road_shaft) {
  const bool shift_pressed = drive.shift && drive.shift != previous_shift_;
  previous_shift_ = drive.shift;
  if (!drive.engaged || drive.gears <= 0) {
    gear = 0;
    shift_time_ = 0;
    forced_reverse_ = false;
    return;
  }

  shift_time_ = std::max(0.0, shift_time_ - timestep);
  if (gear != 7 && gear > drive.gears) gear = drive.gears;
  if (drive.reverse && !forced_reverse_ && drive.ratios[7] < 0) {
    forced_reverse_ = true;
    gear = 7;
    shift_time_ = 0;
  } else if (!drive.reverse && forced_reverse_) {
    forced_reverse_ = false;
    gear = drive.automatic_shift ? 1 : 0;
  } else if (shift_pressed && shift_time_ == 0 && !forced_reverse_) {
    const int next = std::clamp((gear == 7 ? -1 : gear) + drive.shift,
                                drive.ratios[7] < 0 ? -1 : 0, drive.gears);
    gear = next < 0 ? 7 : next;
    shift_time_ = .22;
  }

  if (!drive.automatic_shift || !engine_running || shift_time_ > 0 ||
      drive.clutch < .5)
    return;
  if (gear == 0 && drive.throttle > .1) gear = 1;
  if (gear <= 0 || gear == 7 || shaft <= 2) return;
  const double ratio = drive.ratios[gear];
  const double coupled_rpm = std::abs(road_shaft * ratio) * rpm_per_rad;
  if (gear < drive.gears && rpm > drive.limit * .88 &&
      coupled_rpm > drive.limit * .7 && drive.ratios[gear + 1] > 0) {
    ++gear;
    shift_time_ = .12;
  } else if (gear > 1 && rpm < std::max(drive.idle * 1.45, drive.limit * .42) &&
             shaft * ratio * rpm_per_rad < drive.limit * .65) {
    --gear;
    shift_time_ = .22;
  }
}

void VehicleDynamics::step_drive() {
  const double rpm = engine_omega * rpm_per_rad;
  double weights = 0, shaft = 0, road_shaft = 0, square_weights = 0, launch_slip = 0;
  for (size_t i = 0; i < wheels.size(); ++i) {
    if (!wheels[i].enabled) continue;
    const auto& cfg = drive.wheels[i];
    const auto& s = state[i];
    const double w = cfg.weight;
    weights += w;
    shaft += w * s.spin;
    square_weights += w * w;
    if (s.contact && wheels[i].radius > 0) {
      const Vec3 forward = wheel_forward(body, cfg.steer);
      const Vec3 tangent = (forward - s.normal * forward.dot(s.normal)).normalized();
      const double road_speed = body.point_velocity(s.patch).dot(tangent);
      road_shaft += w * road_speed / wheels[i].radius;
      if (drive.launch_help > 0 && w > 0)
        launch_slip = std::max(launch_slip, (std::abs(s.spin) * wheels[i].radius -
            std::abs(road_speed)) / std::max(2.5, std::abs(road_speed)));
    }
  }
  if (weights > 0) {
    shaft /= weights;
    road_shaft /= weights;
  }
  update_gear(rpm, shaft, road_shaft);
  const double ratio = drive.ratios[gear];
  const double forward_speed = body.velocity.dot(wheel_forward(body, 0));
  const bool direction_brake = (ratio < 0 && forward_speed > 1) ||
                               (ratio > 0 && forward_speed < -1);
  const bool automatic_shift = drive.automatic_shift && shift_time_ > 0;
  const bool accelerator = drive.throttle > .1 && !direction_brake;
  if (!engine_running && accelerator && !accelerator_down_)
    starter_remaining_ = 2;
  accelerator_down_ = accelerator;
  const bool starting = !engine_running && drive.starter > 0 &&
                        (accelerator || starter_remaining_ > 0);
  starter_remaining_ = std::max(0.0, starter_remaining_ - timestep);
  service_brake_ = std::lerp(service_brake_, direction_brake ? 1.0 : drive.brake,
                             -std::expm1(-timestep / .08));
  const double drag = 14 + std::max(0.0, rpm - drive.idle) * .012;
  const double idle_torque = rpm < drive.idle
      ? std::min(80.0, 25.0 + (drive.idle - rpm) * .25) : 0;
  const double combustion = std::clamp((rpm - 200) / 450, 0.0, 1.0);
  const double kph = std::abs(forward_speed) * 3.6;
  double gas = direction_brake ? 0 : drive.throttle;
  if (drive.launch_help <= 0 || !engine_running || drive.clutch < .5 ||
      drive.brake > 0 || drive.handbrake > .2 || gas < .2 ||
      (gear != 1 && gear != 7) || kph >= 30) {
    launch_active_ = false;
    launch_factor_ = 1;
  } else if (launch_active_ || (kph < 2 && gas >= .6)) {
    launch_active_ = true;
    const double target = std::clamp(1 - std::max(0.0, launch_slip - .15) * 2, .05, 1.0);
    launch_factor_ = std::lerp(launch_factor_, target,
        -std::expm1(-timestep / (target < launch_factor_ ? .04 : .15)));
    gas *= std::lerp(1.0, launch_factor_, drive.launch_help);
  }
  const double combustion_torque = engine_running && rpm < drive.limit && !automatic_shift
      ? (2 * drive.torque(rpm) * gas + idle_torque) * combustion *
            limiter_factor(rpm, previous_rpm_, drive.limit)
      : 0;
  previous_rpm_ = rpm;
  const double net = combustion_torque + (starting ? drive.starter : 0) - drag;
  engine_omega = std::max(0.0, engine_omega + net * timestep / drive.inertia);

  if (weights > 0 && ratio != 0 && shift_time_ == 0 && !direction_brake &&
      (engine_running || !drive.auto_clutch)) {
    // clutchExchange, with the same wheel inertias reflected to the engine
    // and receiving the return impulse. ClutchF is not an SI torque ceiling.
    const double transmission = wheel_inertia * weights * weights / square_weights;
    const double reflected = transmission / (ratio * ratio);
    const double mix = reflected / (drive.inertia + reflected);
    const double mismatch = shaft * ratio - engine_omega;
    constexpr double exchange_rate = .9990000128746033;
    const double closed_delta = mismatch * mix * exchange_rate;
    double grip = drive.clutch;
    const double stall_risk = std::clamp((drive.idle * 1.5 -
        std::min(rpm, std::abs(shaft * ratio) * rpm_per_rad)) /
        std::max(1.0, drive.idle * .5), 0.0, 1.0);
    if (drive.auto_clutch && closed_delta < 0) {
      const double idle_floor = drive.idle * .85 / rpm_per_rad;
      grip = std::min(grip, std::max(0.0, engine_omega - idle_floor) / -closed_delta);
    }
    if (drive.auto_clutch && gas <= .05) {
      const double low_speed = std::clamp((12 - kph) / 10, 0.0, 1.0);
      grip *= 1 - stall_risk * low_speed;
    }
    engine_omega = std::max(0.0, engine_omega + closed_delta * grip);
    const double impulse = -mismatch * (1 - mix) * grip * exchange_rate /
                           ratio * transmission;
    for (size_t i = 0; i < wheels.size(); ++i)
      if (wheels[i].enabled)
        state[i].spin += impulse * drive.wheels[i].weight / (weights * wheel_inertia);
  } else if (automatic_shift && engine_running) {
    const double target = std::max(drive.idle, std::abs(road_shaft * ratio) * rpm_per_rad);
    engine_omega = std::lerp(engine_omega, target / rpm_per_rad,
                            -std::expm1(-timestep / .07));
  }

  const double next_rpm = engine_omega * rpm_per_rad;
  if (engine_running && next_rpm < std::max(250.0, drive.idle * .35))
    engine_running = false;
  else if (!engine_running && next_rpm >= std::max(500.0, drive.idle * .65) &&
           (starting || (!drive.auto_clutch && gas > 0))) {
    engine_running = true;
    starter_remaining_ = 0;
  }

  for (size_t i = 0; i < wheels.size(); ++i) {
    const auto &wheel = wheels[i];
    const auto &cfg = drive.wheels[i];
    auto &s = state[i];
    if (!wheel.enabled) continue;
    double road_speed = 0;
    if (!s.contact || s.force <= 0 || cfg.tyre[11] <= 0) {
      s.longitudinal_slip = s.lateral_slip = 0;
      s.spin *= 1 - .25 * timestep / wheel_inertia;
    } else {
      const Vec3 heading = wheel_forward(body, cfg.steer);
      const Vec3 axle = body.orientation.rotate({std::cos(cfg.steer), 0, -std::sin(cfg.steer)});
      Vec3 forward = heading;
      forward = (forward - s.normal * forward.dot(s.normal)).normalized();
      const Vec3 side = forward.cross(s.normal);
      const Vec3 speed = body.point_velocity(s.patch);
      const double road = speed.dot(forward), lateral = speed.dot(side);
      road_speed = road;
      const double rolling = s.spin * wheel.radius - road;
      const double abs_road = std::abs(road);
      const double blend = std::clamp((abs_road - .1) * .1, 0.0, 1.0);
      const double target_lat = abs_road > .1 ? std::atan(lateral / abs_road) : 0;
      s.longitudinal_slip = longitudinal_slip(s.longitudinal_slip, rolling, abs_road, blend);
      s.lateral_slip = std::lerp(relaxed_slip(s.lateral_slip, lateral, .5, .2), target_lat, blend);
      const double lon = std::cos(s.lateral_slip) * s.longitudinal_slip * cfg.tyre[12];
      const double lat = std::sin(s.lateral_slip);
      const double slip = std::hypot(lon, lat);
      constexpr double half_pi = 1.5707963267948966;
      const double maximum = cfg.tyre[11];
      const double lateral_normal = axle.dot(s.normal);
      const double slope = (std::sqrt(std::max(0.0, 1 - lateral_normal * lateral_normal)) + 1) * .5;
      const double load = slope * std::lerp(std::min(s.force, maximum),
          std::atan(s.force * half_pi / maximum) * maximum / half_pi, cfg.tyre[14]);
      Sl2PacejkaCoef curve;
      std::copy_n(cfg.tyre.begin(), curve.size(), curve.begin());
      if (cfg.tyre[16] > 0)
        curve[4] *= (cfg.tyre[15] + cfg.tyre[16]) / (load + cfg.tyre[16]);
      const double magnitude = slip >= .0001 ? sl2_pacejka_force(curve, slip, load, false) / slip : 0;
      double longitudinal = magnitude * lon;
      // SLRR_rev suspension-3d.ts clamps the reaction if it would drive
      // longitudinal slip through zero within this step.
      const double predicted = longitudinal_slip(s.longitudinal_slip,
          rolling - longitudinal * wheel.radius * wheel.radius * timestep / wheel_inertia,
          abs_road, blend);
      if (predicted * s.longitudinal_slip < 0)
        longitudinal *= std::abs(s.longitudinal_slip / (predicted - s.longitudinal_slip));
      body.add_force(forward * longitudinal - side * (magnitude * lat), s.patch);
      const double rolling_torque = s.spin == 0 ? 0 :
          std::copysign(s.force * wheel.radius * cfg.rolling_resistance, s.spin);
      s.spin -= (longitudinal * wheel.radius + rolling_torque + s.spin * .15) *
                timestep / wheel_inertia;
    }
    const double brake_step = 500 * timestep;
    const auto brake = [](double speed, double amount) {
      return std::copysign(std::max(0.0, std::abs(speed) - amount), speed);
    };
    const double free_spin = brake(s.spin, brake_step * cfg.bearing * drive.bearing_scale);
    const double torque_step = timestep / wheel_inertia;
    double braked = brake(free_spin, torque_step * service_brake_ *
                                      cfg.brake * drive.brake_scale);
    if (s.contact && std::abs(road_speed) >= 3 && free_spin * road_speed > 0) {
      const double minimum = std::abs(road_speed) / std::max(.01, wheel.radius) * .85;
      const double allowed = std::max(0.0, std::abs(free_spin) - minimum);
      const double released = std::max(0.0, std::abs(free_spin - braked) - allowed);
      braked += std::copysign(released * drive.abs_strength, road_speed);
    }
    s.spin = brake(braked, torque_step * drive.handbrake * cfg.handbrake * drive.handbrake_scale);
    s.rotation = std::remainder(s.rotation + s.spin * timestep, 6.283185307179586);
  }
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
      solve_linked(body, w, s, terrain, ceiling, timestep, !drive.torque);
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
    s.patch = point;
    if (drive.torque) continue;
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
  if (drive.torque && drive.inertia > 0) step_drive();
  body.integrate(timestep);
  for (int iteration = 0; iteration < 4; ++iteration) {
    double depth = 0;
    Vec3 correction;
    auto solve = [&](Vec3 point, double restitution, bool chassis = false) {
      GroundHit ground;
      if (!terrain(point.x, point.z, ceiling, ground))
        return false;
      double overlap = ground.height - point.y;
      if (overlap <= 0)
        return false;
      if (chassis && iteration == 0) {
        const double speed = -body.point_velocity(point).dot(ground.normal);
        if (speed > ground_impact.speed)
          ground_impact = {point, ground.normal, speed};
      }
      body.contact(point, ground.normal, restitution);
      if (overlap > depth) {
        depth = overlap;
        correction = ground.normal.normalized();
      }
      return true;
    };
    for (Vec3 point : body.corners())
      solve(point, .12, true);
    // A fully compressed wheel is also a unilateral support constraint.
    // Without this hard stop an initially buried tyre remains underground
    // while the penalty spring pumps enough energy to launch the chassis.
    for (size_t i = 0; i < wheels.size(); ++i) {
      const auto &wheel = wheels[i];
      if (!wheel.enabled)
        continue;
      const bool linked = wheel.linked() && !wheel.rigid_contact;
      if (linked && state[i].length > wheel.minimum + .002)
        continue;
      const auto down = wheel.down.normalized();
      if (!wheel.rigid_contact && body.orientation.rotate(down).y >= -.05)
        continue;
      auto local = ray_center(wheel, wheel.minimum);
      if (linked)
        hinge_hub(wheel, local, local);
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
  ground_impact = {};
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
void VehicleDynamics::reset_drive() {
  engine_omega = previous_rpm_ = shift_time_ = 0;
  starter_remaining_ = service_brake_ = 0;
  gear = previous_shift_ = 0;
  launch_active_ = false;
  launch_factor_ = 1;
  accelerator_down_ = engine_running = forced_reverse_ = false;
}

void VehicleDynamics::release_wheels() {
  for (auto &wheel : state)
    wheel = {};
}
uint64_t VehicleDynamics::backlog() const {
  return static_cast<uint64_t>((accumulator_ + timestep * 1e-10) / timestep);
}
} // namespace slrr::physics
