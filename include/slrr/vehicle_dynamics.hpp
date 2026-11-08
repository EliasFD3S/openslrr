#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace slrr::physics {
// Script-space metres, kilograms and seconds. Renderer handedness stays
// outside.
struct Vec3 {
  double x = 0, y = 0, z = 0;
  Vec3 operator+(Vec3 b) const { return {x + b.x, y + b.y, z + b.z}; }
  Vec3 operator-(Vec3 b) const { return {x - b.x, y - b.y, z - b.z}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
  bool operator==(const Vec3 &) const = default;
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

// World-space static collision triangles, in script metres. A null mesh keeps
// the native corner-contact fallback; a non-null mesh enables Chrono contacts.
struct CollisionMesh {
  std::vector<float> positions; // xyz
  std::vector<uint32_t> indices;
};

// Integration backend state (Project Chrono when SLRR_HAS_CHRONO). The
// RigidBody fields stay the authoritative mirror: callers may overwrite
// velocity/momentum at any time and the backend is resynchronized from them
// on every step. Copies start with a fresh backend instead of sharing one.
struct ChronoBodyState;
class BackendHandle {
public:
  BackendHandle();
  ~BackendHandle();
  BackendHandle(const BackendHandle &);
  BackendHandle &operator=(const BackendHandle &);
  BackendHandle(BackendHandle &&) noexcept;
  BackendHandle &operator=(BackendHandle &&) noexcept;
  ChronoBodyState *get() const { return state_.get(); }
  void reset(ChronoBodyState *state);

private:
  std::unique_ptr<ChronoBodyState> state_;
};
// "chrono" or "native": the integrator RigidBody::integrate uses.
const char *backend_name();

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
  void contact(Vec3 point, Vec3 normal, double restitution = .12,
               double friction = .6);
  std::array<Vec3, 8> corners() const;

private:
  friend struct ChronoIntegrator;
  Vec3 inverse_inertia_, force_, torque_;
  BackendHandle backend_;
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
};
double suspension_force(double length, double velocity, double rest,
                        double maximum, double spring, double bump,
                        double rebound);

// First dynamic vertical/contact stage of Vehicle3D/Suspension3D. No engine
// thrust or tyre slip is invented here: drivetrain is a separate port stage.
class VehicleDynamics {
public:
  static constexpr unsigned frequency = 700;
  static constexpr unsigned update_frequency = 60;
  static constexpr double timestep = 1.0 / frequency;
  RigidBody body;
  // Shared immutable scene snapshot. The native backend ignores this and the
  // Chrono backend rebuilds its static collision body when the pointer changes.
  std::shared_ptr<const CollisionMesh> collision;
  std::array<WheelConfig, 4> wheels;
  std::array<WheelState, 4> state;
  uint64_t ticks = 0;
  unsigned advance(double seconds, const Terrain &terrain);
  void reset_clock();
  void release_wheels();
  uint64_t backlog() const;
  void sync_wheels();

private:
  double accumulator_ = 0;
  void step(const Terrain &terrain);
};
} // namespace slrr::physics
