#pragma once
#include "slrr/tire_model.hpp"
#include <array>
#include <cstdint>
#include <functional>

namespace slrr::physics {
// Script-space metres, kilograms and seconds. Renderer handedness stays
// outside.
struct Vec3 {
  double x = 0, y = 0, z = 0;
  Vec3 operator+(Vec3 b) const { return {x + b.x, y + b.y, z + b.z}; }
  Vec3 operator-(Vec3 b) const { return {x - b.x, y - b.y, z - b.z}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
  Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(double s) const { return *this * (1 / s); }
  Vec3 &operator+=(Vec3 b) {
    *this = *this + b;
    return *this;
  }
  double dot(Vec3 b) const { return x * b.x + y * b.y + z * b.z; }
  Vec3 cross(Vec3 b) const {
    return {y * b.z - z * b.y, z * b.x - x * b.z, x * b.y - y * b.x};
  }
  double length() const;
  Vec3 normalized() const;
  bool finite() const;
};
struct Quaternion {
  double x = 0, y = 0, z = 0, w = 1;
  Quaternion operator*(Quaternion b) const;
  Quaternion normalized() const;
  Vec3 rotate(Vec3 v) const;
  Vec3 inverse_rotate(Vec3 v) const;
};
using Matrix = std::array<float, 16>;

// Adapted contracts: SLRR_rev a2ee10de, rigid-body-3d.ts. Box inertia is the
// upstream approximation; it is not the original game's compound inertia.
class RigidBody {
public:
  double mass = 1;
  Vec3 position, velocity, momentum, center, minimum, maximum;
  Quaternion orientation;
  void configure(double kilograms, Vec3 bounds_min, Vec3 bounds_max, Vec3 com);
  void set_frame(const Matrix &frame);
  Matrix frame() const;
  Vec3 inverse_world_inertia(Vec3 v) const;
  Vec3 omega() const;
  Vec3 point_velocity(Vec3 point) const;
  void add_force(Vec3 force, Vec3 point);
  void impulse(Vec3 impulse, Vec3 point);
  void integrate(double dt);
  double contact(Vec3 point, Vec3 normal, double restitution = .12,
                 double friction = .6, RigidBody* other = nullptr);
  std::array<Vec3, 8> corners() const;

private:
  Vec3 inverse_inertia_, force_, torque_;
};

struct GroundHit {
  double height = 0;
  Vec3 normal{0, 1, 0};
};
// from_y is a ray origin/ceiling, not a default ground height. False = air.
using Terrain =
    std::function<bool(double x, double z, double from_y, GroundHit &)>;
struct WheelConfig {
  bool enabled = false;
  bool rigid_contact = false;
  Vec3 center, down{0, -1, 0};
  double radius = 0, rest = 0, minimum = 0, maximum = 0;
  // CFG spring base + travel, independent of the tunable spring rest length.
  // Zero is the generic kernel fallback (anchor at rest length).
  double anchor = 0;
  double anchor_length() const { return anchor > 0 ? anchor : rest; }
  double spring = 0, bump_damping = 0, rebound_damping = 0;
  // CFG `type` mass. Zero keeps the massless ray. An arm length projects the
  // support onto the native hinge circle; the spring stays the 1-DOF element.
  double unsprung = 0, arm_length = 0;
  Vec3 arm_pivot, arm_axis{0, 0, 1};
  // Raw setHub arguments. Without a hub the arm circle is the support point.
  bool has_hub = false;
  std::array<float, 10> hub{};
  Quaternion hub_alignment;
  bool linked() const { return unsprung > 0 || arm_length > 0; }
};
struct WheelState {
  bool contact = false, seeded = false;
  double length = 0, speed = 0, force = 0;
  Vec3 center, normal{0, 1, 0}, hub, patch;
  Quaternion hub_rotation;
  double spin = 0, rotation = 0;
  double slip_speed = 0, lateral_speed = 0;
  TireForces tire_force{};
};
struct DriveWheel {
  double weight = 0, steer = 0, brake = 0, handbrake = 0, bearing = 0;
  double rolling_resistance = 0;
  TireParameters tyre;
};
// Read-only telemetry views. Impulses are the normal contact impulses from
// the most recent 120 Hz substep; they are not accumulated force estimates.
struct WheelTelemetry {
  bool enabled = false, contact = false;
  double length = 0, speed = 0, force = 0;
  double spin = 0, rotation = 0, slip_speed = 0, lateral_speed = 0;
  double tire_longitudinal = 0, tire_lateral = 0;
  double contact_impulse = 0;
  Vec3 center, normal, patch;
};
struct VehicleTelemetry {
  double mass = 0;
  Vec3 position, velocity, momentum, omega;
  Quaternion orientation;
  uint64_t dynamics_ticks = 0;
  double engine_omega = 0;
  bool engine_running = false;
  int gear = 0;
  double throttle = 0, brake = 0, handbrake = 0, clutch = 0;
  std::array<WheelTelemetry, 4> wheels{};
  struct ImpactTelemetry {
    Vec3 point, normal;
    double speed = 0;
  } ground_impact;
  double chassis_contact_impulse = 0;
};

struct DriveInput {
  std::array<DriveWheel, 4> wheels{};
  std::array<double, 8> ratios{};
  int gears = 0, shift = 0;
  bool engaged = false, reverse = false, automatic_shift = true, auto_clutch = true;
  bool automatic_first = false;
  double throttle = 0, brake = 0, handbrake = 0;
  double brake_scale = 0, handbrake_scale = 0, bearing_scale = 0;
  double abs_strength = 0;
  double launch_help = 0;
  double differential_lock = 0;
  double inertia = 0, idle = 0, limit = 0, starter = 0;
  double clutch = 1;
  std::function<double(double)> torque;
};
double suspension_force(double length, double velocity, double rest,
                        double maximum, double spring, double bump,
                        double rebound);

class VehicleDynamics {
public:
  // SLRR_rev physics-timing.ts driving frequency.
  static constexpr unsigned frequency = 120;
  static constexpr unsigned update_frequency = 60;
  static constexpr double timestep = 1.0 / frequency;
  RigidBody body;
  struct Impact {
    Vec3 point, normal;
    double speed = 0;
  } ground_impact;
  std::array<WheelConfig, 4> wheels;
  std::array<WheelState, 4> state;
  DriveInput drive;
  double engine_omega = 0;
  bool engine_running = false;
  int gear = 0;
  uint64_t ticks = 0;
  unsigned advance(double seconds, const Terrain &terrain);
  void reset_clock();
  void release_wheels();
  void reset_drive();
  uint64_t backlog() const;
  void sync_wheels();
  VehicleTelemetry telemetry_snapshot() const;

private:
  std::array<double, 4> last_rigid_impulses_{};
  double last_chassis_impulse_ = 0;
  double accumulator_ = 0;
  double previous_rpm_ = 0, shift_time_ = 0;
  double starter_remaining_ = 0, service_brake_ = 0;
  double launch_factor_ = 1;
  int previous_shift_ = 0;
  bool accelerator_down_ = false, forced_reverse_ = false;
  bool launch_active_ = false;
  void step(const Terrain &terrain);
  void step_drive();
  void update_gear(double rpm, double shaft, double road_shaft);
};
} // namespace slrr::physics
