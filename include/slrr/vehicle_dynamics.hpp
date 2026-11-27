#pragma once
#include "slrr/brake_system.hpp"
#include "slrr/ground_surface.hpp"
#include "slrr/tire_model.hpp"
#include "slrr/tyre_thermal.hpp"
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
  void add_torque(Vec3 torque) { torque_ += torque; }
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
  // The ground material (#452): see GroundSurface.
  GroundSurface surface;
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
  // Anti-dive / anti-lift / anti-squat (#469): the share of this wheel's
  // longitudinal load transfer its arms carry, braking and driving, 0 to 1.
  // The arms push body and wheel apart (or together) by that share of
  // force x CoM height / wheelbase, so the spring compresses less.
  double anti_brake = 0, anti_drive = 0;
  // Camber curve (#481): about the static ride height, camber changes by
  // gain x bump + progression x bump^2 / 2 (rad/m and rad/m^2, bump the
  // spring's compression from static, metres) in place of the arm
  // geometry's own change. NaN gain: the geometry's gain at the static
  // height, clamped to a road car's range. Off: the geometry alone.
  bool camber_curve = false;
  double camber_gain = 0, camber_progression = 0;
  // Bump steer (#484): likewise for the toe, positive toe-in (the wheel's
  // front turning inboard) as the spring compresses, rad/m and rad/m^2.
  // Off: the geometry's toe alone.
  bool toe_curve = false;
  double bump_steer = 0, bump_steer_progression = 0;
  // Compliance steer (#484): the toe-in (rad) the bushings give per newton
  // of the tyre's cornering force pushing the patch inboard and per newton
  // of braking force (driving force reverses it). Zero: rigid.
  double compliance_lateral = 0, compliance_braking = 0;
  // The bushings' give (m per newton): the hub moves inboard under the
  // cornering force pushing the patch inboard and along the tyre's
  // longitudinal force (back under braking). Zero: rigid.
  double bushing_lateral = 0, bushing_longitudinal = 0;
  // Set by update_static_reference (suspension_geometry.hpp) from the car's
  // weight: the static spring length (0 unknown) and, there, the
  // geometry's camber and toe (rad) and the camber gain the curve uses
  // (rad/m).
  double static_length = 0, reference_camber = 0, reference_toe = 0, reference_gain = 0;
  bool linked() const { return unsprung > 0 || arm_length > 0; }
};
struct WheelState {
  bool contact = false, seeded = false;
  // force: spring and damper support, used as the tyre load. jacking: the
  // arms' vertical push from the longitudinal force (#469, N, extending the
  // suspension; WheelConfig::anti_brake / anti_drive), from the last step.
  double length = 0, speed = 0, force = 0, jacking = 0;
  Vec3 center, normal{0, 1, 0}, hub, patch;
  // The ground under the tread (#452): friction and rolling averaged over
  // the tread samples, the rest from the centre.
  GroundSurface surface;
  Quaternion hub_rotation;
  // The steering axis in the hub's frame (CornerFrames::steer_axis, #458).
  Vec3 steer_axis{0, 1, 0};
  // Compliance steer (#484): the road steering angle (rad, positive to the
  // left) the bushings add, from the last steps' tyre forces.
  double compliance_steer = 0;
  // The hub's displacement in the body's frame (m) from the bushings' give.
  Vec3 compliance_offset;
  double spin = 0, rotation = 0;
  double slip_speed = 0, lateral_speed = 0;
  // Relaxed tan(slip angle): the tyre's lateral deflection builds over its
  // relaxation length instead of following the slip at once (#383).
  double slip_angle = 0;
  // Unspent brake/bearing budget at the patch (N s), not free tyre grip.
  double holding_impulse = 0;
  // Rotor temperature (deg C) and the service brake pressure reaching the
  // caliper after ABS/ASR, as a fraction of full pedal (#446).
  double brake_temperature = kAmbientTemperature, brake_pressure = 0;
  // Brake fluid in the caliper (deg C) and the tyre's tread and carcass.
  double fluid_temperature = kAmbientTemperature;
  TyreTemperature tyre_temperature;
  // Hot gauge pressure (bar, 0 when not known) and the tread's wear work so
  // far: the sliding joules, each weighted by tyre_wear_weight (#461). The
  // host turns its growth into the tyre part's wear.
  double tyre_pressure = 0, wear_work = 0;
  // A puncture's deflation, 0 inflated to 1 flat; the tyre's vertical rate
  // (N/m, from the pressure and the deflation; 0 is the stock 250 kN/m);
  // rim strikes so far (the tyre squashed to the rim while closing fast,
  // which pinches it) and whether it is on the rim now (#461).
  double deflation = 0, tyre_rate = 0;
  int rim_strikes = 0;
  bool on_rim = false;
  // Flat spot (#461): a locked wheel sliding on one place of its tread
  // grinds it flat there. Its depth (m) and the wheel rotation at which it
  // touches the ground; while locked, the spot being ground (it replaces
  // the old one once deeper); the support it takes away now (m), as the
  // spot rolls under the patch; and the tyre it belongs to.
  double flat_spot = 0, flat_spot_angle = 0;
  double lock_spot = 0, lock_angle = 0, flat_drop = 0;
  bool locked = false;
  std::uintptr_t flat_tyre = 0;
  int flat_serial = 0;
  TireForces tire_force{};
};
// Sliding joules (weighted by tyre_wear_weight) that grind one millimetre
// of flat spot: a locked stop from 100 km/h (about 170 kJ on a front tyre)
// leaves about 1 mm (#461).
inline constexpr double kFlatSpotEnergy = 1.7e8;  // J per metre of depth
// Depth at which a flat spot reaches the cords and the tyre blows out.
inline constexpr double kFlatSpotCords = .006;
// Closing speed (m/s) at which a tyre squashed to its rim pinches, at its
// working pressure; a soft tyre pinches sooner (#461).
inline constexpr double kPinchSpeed = 2.5;
// Optional host anti-roll approximation, in N/m and N s/m. Invalid or
// negative coefficients act as zero; no installed bar means both stay zero.
struct AntiRollConfig {
  double stiffness = 0, damping = 0;
};
struct DriveWheel {
  double weight = 0, steer = 0, brake = 0, handbrake = 0, bearing = 0;
  double rolling_resistance = 0;
  TireParameters tyre;
  // Pad, rotor and diameter from WheelRef.setBrakeSpec (#446); unspecified
  // brakes stay cold and never fade.
  BrakeHardware hardware;
  // Compound and size from WheelRef.setTyreSpec (#446); unspecified tyres
  // stay at ambient with their scripted grip.
  TyreThermalSpec tyre_heat;
  // The Tyre script's cold gauge pressure (bar, its `inflation`; 0 when not
  // known) and the tyre part's condition (1 new, 0 worn out), #461.
  double inflation = 0, tread_wear = 1;
  // A punctured tyre (#461): 0 sound, 1 a slow leak (a pinch), 2 a blowout.
  int puncture = 0;
  // Which tyre is fitted (any value that changes with the tyre part; 0
  // none) and the flat spot it carries (#461): depth (m) and angle, as the
  // Tyre saved them. The solver takes them when the tyre or flat_serial
  // changes (a tyre fitted, a game loaded, a repair).
  std::uintptr_t tyre_id = 0;
  double flat_spot = 0, flat_spot_angle = 0;
  int flat_serial = 0;
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
  // Driving aids (Options ABS / ASR sliders, #446): 0 off, 1 the tightest
  // slip targets.
  double abs_strength = 0;
  double asr_strength = 0;
  // Rear proportioning valve (#446): above `knee` (a fraction of full
  // pedal) the rear pressure rises by `slope` only. Knee 0 disables it; an
  // active ABS replaces it with its electronic distribution.
  double rear_valve_knee = 0, rear_valve_slope = 1;
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
  // Hanging engine hardware (exhausts, mufflers, the sump), body frame
  // (#467): they scrape rather than carry the car. Each gives on its
  // hangers or mounts first, then the pipe or the pan hits, sliding at metal
  // friction.
  std::array<Vec3, 8> scrape_points{};
  std::size_t scrape_count = 0;
  // A mesh that the static ride height puts into level ground would drag
  // the car at rest and on every straight: the host raises such a point to
  // 5 mm above the ground the parked car stands on (m, body up), so it
  // scrapes only when the body comes lower than at rest.
  std::array<double, 8> scrape_lift{};
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
  // Driving aids at the last step (#446), for the HUD and the debug view.
  bool abs_active() const { return abs_.active(); }
  unsigned abs_releases(std::size_t i) const { return abs_.releases(i); }
  bool asr_active() const { return asr_.active(); }
  double reference_speed() const { return reference_.speed(); }

private:
  double accumulator_ = 0;
  double dt_ = timestep;  // length of the step being run
  double previous_rpm_ = 0, shift_time_ = 0;
  double starter_remaining_ = 0, service_brake_ = 0;
  // What the ABS/ASR controller measures (#446): the forward speed of the
  // previous step (for the accelerometer), the wheel circumferential speeds
  // and their filtered rates.
  double forward_speed_previous_ = 0, travel_direction_ = 1;
  bool sensors_seeded_ = false;
  std::array<double, 4> wheel_speed_previous_{}, wheel_acceleration_{};
  ReferenceSpeed reference_;
  AbsModulator abs_;
  TractionControl asr_;
  // How far the auto clutch is opened for a hard stop, 0..1 (#448).
  double braking_clutch_ = 0;
  int previous_shift_ = 0;
  bool accelerator_down_ = false, forced_reverse_ = false;
  void step(const Terrain &terrain);
  void update_jacking();
  void update_static_references();
  void update_compliance_steer();
  void apply_aero();
  void step_drive();
  void update_gear(double rpm, double shaft, double road_shaft);
};
} // namespace slrr::physics
