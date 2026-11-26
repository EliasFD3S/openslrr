#pragma once
#include "slrr/tire_model.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

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

// Adapted contracts: SLRR_rev a2ee10de, rigid-body-3d.ts. The 8ac2ba9
// fallback uses a solid box of 0.75 times the bounds dimensions (host tuning,
// not the collision dimensions or the original game's compound inertia).
class RigidBody {
public:
  double mass = 1;
  Vec3 position, velocity, momentum, center, minimum, maximum;
  Quaternion orientation;
  void configure(double kilograms, Vec3 bounds_min, Vec3 bounds_max, Vec3 com);
  // Principal moments about `com` along the body axes, in kg m². All three
  // must be positive and finite; otherwise the entire fallback is retained.
  void configure(double kilograms, Vec3 bounds_min, Vec3 bounds_max, Vec3 com,
                 Vec3 principal_inertia);
  Vec3 inertia() const;
  void set_frame(const Matrix &frame);
  Matrix frame() const;
  Vec3 inverse_world_inertia(Vec3 v) const;
  Vec3 omega() const;
  void set_omega(Vec3 angular_velocity);
  Vec3 point_velocity(Vec3 point) const;
  void add_force(Vec3 force, Vec3 point);
  void impulse(Vec3 impulse, Vec3 point);
  void integrate(double dt);
  // Split semi-implicit integration for constraints that must remove slip
  // after forces/gravity, but before those velocities advance the pose.
  void integrate_forces(double dt);
  void integrate_pose(double dt);
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
// Real-life aerodynamics (ENGINE_CONTRACT §11): ½ρv² forces from the air
// velocity relative to the car, with typical road-car coefficients.
// The mean wind of a weather report: m/s at 10 m height, blowing towards
// `heading` (degrees from +Z towards +X).
struct Wind {
  double speed = 0, heading = 0;
};
// Air velocity (world m/s) at car height at a world position and time: the
// mean wind reduced by the log wind profile, plus gusts that travel with it.
Vec3 wind_velocity(const Wind &wind, Vec3 position, double seconds);
struct AeroWing {
  // One face of a wing in chassis-frame metres: unit normal, planform m².
  Vec3 position, normal;
  double area = 0;
};
struct Aerodynamics {
  // Chassis.C_drag (kg/m): the extra drag of missing body panels, against
  // the velocity of drag_center (chassis frame) above 1 m/s.
  double drag = 0;
  Vec3 drag_center;
  // Drag and lift of the body, sized by the rigid body's bounds.
  bool body = false;
  std::vector<AeroWing> wings;
  // Air velocity around the car (world m/s); every force uses the car's
  // velocity relative to it.
  Vec3 wind;
};
// Resultant aerodynamic force (world) and its torque about the centre of
// mass for one step of `dt`, as VehicleDynamics applies it.
struct AeroLoad {
  Vec3 force, torque;
};
AeroLoad aero_load(const Aerodynamics &aero, const RigidBody &body, double dt);
struct WheelConfig {
  bool enabled = false;
  // Fixed suspension travel with compliant tyre/bump-stop support.
  bool rigid_contact = false;
  bool metal_contact = false; // no installed rim/tyre, only hub hardware
  Vec3 center, down{0, -1, 0};
  double radius = 0, rest = 0, minimum = 0, maximum = 0;
  // Half the tyre width (m). The linked contact samples the tyre footprint
  // across it; zero samples the centre line only (#380).
  double half_width = 0;
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
  // Stored WheelRef.setInstantCenter data, relative to `center` in chassis
  // axes: front-view x/y, side-view z/y. Retained for native compatibility;
  // the intended 8ac2ba9 solver does not apply an instant-centre load force.
  bool front_instant_center = false, side_instant_center = false;
  Vec3 front_center_offset, side_center_offset;
  bool linked() const { return unsprung > 0 || arm_length > 0; }
};
struct WheelState {
  bool contact = false, seeded = false;
  // force: spring and damper support, used as the tyre load. jacking is a
  // reserved legacy field; the intended solver does not add it to the load.
  double length = 0, speed = 0, force = 0, jacking = 0;
  Vec3 center, normal{0, 1, 0}, hub, patch;
  Quaternion hub_rotation;
  double spin = 0, rotation = 0;
  double slip_speed = 0, lateral_speed = 0;
  // Relaxed tan(slip angle): the tyre's lateral deflection builds over its
  // relaxation length instead of following the slip at once (#383).
  double slip_angle = 0;
  // Unspent brake/bearing budget at the patch (N s), not free tyre grip.
  double holding_impulse = 0;
  TireForces tire_force{};
};
// Optional host anti-roll approximation, in N/m and N s/m. Invalid or
// negative coefficients act as zero; no installed bar means both stay zero.
struct AntiRollConfig {
  double stiffness = 0, damping = 0;
};
struct DriveWheel {
  double weight = 0, steer = 0, brake = 0, handbrake = 0, bearing = 0;
  double rolling_resistance = 0;
  TireParameters tyre;
};
struct DriveInput {
  bool native_traffic = false; // SLRR_rev TrafficVehicle tyre-force driver.
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
  // Net open-throttle crank torque (Nm) at the requested rpm, including
  // the shared mechanical friction loss; the solver applies no extra gain.
  std::function<double(double)> torque;
  // Optional wet dyno bank. Empty/zero leaves the existing dry drive unchanged.
  std::function<double(double)> nitro_torque;
  double nitro = 0;
};
double suspension_force(double length, double velocity, double rest,
                        double maximum, double spring, double bump,
                        double rebound);

// Compile all consumers with the same experimental solver rate (#203).
// The game additionally requires a multiple of its 60 Hz shared-world tick.
#ifndef SLRR_VEHICLE_PHYSICS_HZ
#define SLRR_VEHICLE_PHYSICS_HZ 1000
#endif

class VehicleDynamics {
public:
  // Solver rate (#380). SLRR_VEHICLE_PHYSICS_HZ is the start value;
  // set_frequency changes it at run time (Physics Lab, physics thread). All
  // vehicles share it: change it only while no solver step is running.
  static inline unsigned frequency = SLRR_VEHICLE_PHYSICS_HZ;
  static constexpr unsigned update_frequency = 60;  // host script tick
  static_assert(SLRR_VEHICLE_PHYSICS_HZ >= 60, "solver rate must be at least 60 Hz");
  static inline double timestep = 1.0 / SLRR_VEHICLE_PHYSICS_HZ;
  static void set_frequency(unsigned hz);
  // Solver steps in host tick number `tick` (0-based, counted from any fixed
  // origin): floor((tick+1) f / 60) - floor(tick f / 60). Always f/60 when 60
  // divides f; 16 or 17 at 1000 Hz.
  static unsigned steps_in_host_tick(uint64_t tick);
  RigidBody body;
  struct Impact {
    Vec3 point, normal;
    double speed = 0;
    // Pair contacts supply the actual impulse and pre-impact rigid motion.
    // Negative retains the historical immovable-wall reconstruction.
    double normal_impulse = -1;
    Vec3 pre_velocity{}, pre_omega{};
    bool has_pre_motion = false;
  } ground_impact;
  std::array<WheelConfig, 4> wheels;
  std::array<WheelState, 4> state;
  // Front (0/1) and rear (2/3): both corners must contact on non-rigid
  // suspension. Couples world-Y hardpoint forces, bounded to half body weight
  // per side, after tyre evaluation; never changes the current tyre loads.
  // Deflection is relative to each corner's rest length (no rest-offset preload).
  std::array<AntiRollConfig, 2> anti_roll{};
  // Installed-body low points in chassis coordinates; no implicit support
  // is created when the runtime supplies none. Contact is floor-only.
  std::array<Vec3, 8> body_supports{};
  std::size_t body_support_count = 0;
  DriveInput drive;
  Aerodynamics aero;
  double engine_omega = 0;
  bool engine_running = false;
  int gear = 0;
  uint64_t ticks = 0;
  // Runs the whole solver steps owed after adding `seconds`, at most one host
  // tick's worth. With `merge`, all owed steps run as one step of their total
  // length: distant traffic steps at a fraction of the rate (#380). Returns
  // the solver steps consumed.
  unsigned advance(double seconds, const Terrain &terrain, bool merge = false);
  // Adds time without stepping; a later advance runs it.
  void defer(double seconds) { accumulator_ += seconds; }
  void reset_clock();
  void release_wheels();
  void reset_drive();
  // Fraction of the upcoming engine step allowed by the existing rev limiter.
  double engine_limit_fraction() const;
  uint64_t backlog() const;
  void sync_wheels();

private:
  double accumulator_ = 0;
  double dt_ = timestep;  // length of the step being run
  double previous_rpm_ = 0, shift_time_ = 0;
  double starter_remaining_ = 0, service_brake_ = 0;
  double launch_factor_ = 1;
  // How far the auto clutch is opened for a hard stop, 0..1 (#448).
  double braking_clutch_ = 0;
  int previous_shift_ = 0;
  bool accelerator_down_ = false, forced_reverse_ = false;
  bool launch_active_ = false;
  void step(const Terrain &terrain);
  void apply_aero();
  void step_drive();
  void update_gear(double rpm, double shaft, double road_shaft);
};
} // namespace slrr::physics
