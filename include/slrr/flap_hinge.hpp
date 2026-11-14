#pragma once
#include <cstddef>

// One-degree-of-freedom hinge of a part mounted on a CFG `flap` slot: doors,
// hoods, trunks and tailgates, on a car or in the garage alike.
//
// The CFG line `flap px py pz  ax ay az  dx dy dz  lo hi` sits after a slot.
// The pivot and axis are in the parent part's frame, `dir` points from the
// hinge towards the free edge of the closed panel, and [lo, hi] is the travel
// in radians about the axis, 0 being the CFG (closed) pose. Stock data opens
// either way: Baiern doors use `0 1.134`, a Focer trunk `-0.960 0`. The limit
// farther from zero is the open stop.
//
// The panel is a rigid plate on that hinge: it is latched while closed, a
// script open/close drives it, a hit can release the latch and swing it, and
// once free it reacts to gravity, the carrier's acceleration and rotation, and
// the relative wind, with damped end stops. All constants are an OpenSLRR host
// design; the original executable's flap solver is not recovered, and SLRR_rev
// (77b0fb6) has no flap native.
namespace slrr::physics {

struct FlapGeometry {
  // Parent part frame, metres. `axis` is oriented so that opening is a
  // positive angle; `arm` is the unit lever of the closed panel, normal to it.
  double pivot[3]{}, axis[3]{0, 1, 0}, arm[3]{1, 0, 0};
  // Travel, lower <= 0 < upper. `upper` is the open stop.
  double lower = 0, upper = 0;
  // Closed-pose centre of mass relative to the pivot, normal to the axis.
  double com[3]{};
  // Hinge to free edge along `arm`, and the panel extent along the axis.
  double length = .5, width = .5;
  double mass = 10;
  // Moment of inertia about the hinge axis divided by mass (m^2).
  double inertia = .1;
};

// Builds the hinge from the CFG values. False when the axis is degenerate or
// the travel has no open side; the slot then behaves as a fixed mount.
bool flap_geometry_from_cfg(const float pivot[3], const float axis[3],
                            const float dir[3], float lo, float hi,
                            FlapGeometry &out);
// Sizes the plate from closed-pose points of the panel in the parent frame
// (for instance the eight corners of its bounds) and its mass.
void flap_size_from_points(FlapGeometry &geometry, const double (*points)[3],
                           size_t count, double mass);
// Moves the hinge by a rigid column-major transform of the parent frame (a
// setSlotPos or damage pose of its slot). Sizes, limits and mass are kept.
void flap_transform(FlapGeometry &geometry, const float transform[16]);

struct FlapState {
  double angle = 0, rate = 0;  // radians and rad/s about FlapGeometry::axis
  bool latched = true;
  // Script view (Part.flap): 1 while opened by a script or swung open.
  bool open = false;
  // Opened by a script: a strut pushes the panel to the open stop.
  bool hold = false;
  // A script open or close is still under way.
  bool driving = false;
  // Bent too far (panel deformation) for the latch to catch: it closes onto
  // its stop but stays unlatched.
  bool latch_broken = false;
  double since_release = 0;  // seconds since the latch last let go
};

// Per-step loads, in the parent frame.
struct FlapLoads {
  double gravity[3]{0, -9.81, 0};  // m/s^2
  double air[3]{};                 // air velocity relative to the panel, m/s
};

// Part.flap(1) / the `flap_toggle` command. Returns the new script state.
bool flap_toggle(FlapState &state);
// Velocity change of the carrier over the last frame: dv at the pivot and dw,
// both in the parent frame. A free panel keeps its own momentum, so relative to
// the carrier it swings the other way. No effect while latched.
void flap_carrier_impulse(FlapState &state, const FlapGeometry &geometry,
                          const double dv[3], const double dw[3]);
// An impact at `point` (parent frame, current pose) whose obstacle pushes the
// car along `normal` at `speed` m/s. A strong hit releases the latch; the push
// on a free panel swings it, and the closed stop returns an inward push.
// Returns true when the panel was released or its motion changed.
bool flap_hit(FlapState &state, const FlapGeometry &geometry,
              const double point[3], const double normal[3], double speed);
// What a world contact did to an open panel.
enum class FlapContact {
  Pushed,   // turned out of the obstacle along its swing
  Blocked,  // the obstacle pushes along the hinge or into a stop
  Torn,     // the hinge gives way: the panel comes off
};
// A world contact on the swinging panel, in the parent frame and current
// pose: the obstacle pushes the panel along unit `normal` at `point`, `speed`
// is the panel point's speed into the obstacle (m/s, the carrier's motion
// included) and `depth` the overlap in metres. Above FlapTuning::tear_speed,
// or when an obstacle it cannot swing away from presses more than
// FlapTuning::tear_depth into it, the panel tears off. Otherwise it turns out
// of the overlap and loses its approach speed where the hinge lets it; an
// obstacle in front of an open door therefore swings it shut.
FlapContact flap_contact(FlapState &state, const FlapGeometry &geometry,
                         const double point[3], const double normal[3],
                         double speed, double depth);
// Advances one step. Returns true when the angle changed.
bool flap_step(FlapState &state, const FlapGeometry &geometry,
               const FlapLoads &loads, double seconds);
// Column-major rotation by `angle` about the hinge, in the parent frame.
void flap_matrix(const FlapGeometry &geometry, double angle, float out[16]);

// Host tuning, exposed for tests and documentation.
struct FlapTuning {
  static constexpr double unlatch_speed = 3.5;   // local impact speed, m/s
  static constexpr double seal_kick = .4;        // free edge speed on release, m/s
  static constexpr double latch_speed = .35;     // free edge closing speed that re-latches
  static constexpr double latch_delay = .3;      // s after a release before it can re-latch
  static constexpr double closed_restitution = .3;
  static constexpr double open_restitution = .25;
  static constexpr double friction = .8;         // hinge viscous damping, 1/s
  static constexpr double drive_frequency = 9;   // script drive, rad/s (critically damped)
  static constexpr double drive_limit = 60;      // rad/s^2
  static constexpr double hold_margin = .35;     // drive target past the stop, rad
  static constexpr double hold_detent = .5;      // strut floor (a door check), rad/s^2
  static constexpr double hold_giveway = .35;    // forced this far off the stop, it lets go (rad)
  static constexpr double air_density = 1.225;
  static constexpr double drag = 1;              // flat-plate normal force coefficient
  static constexpr double max_rate = 20;         // rad/s
  static constexpr double tear_speed = 4.5;      // contact speed that rips an open panel off, m/s
  static constexpr double tear_depth = .15;      // overlap it cannot swing out of, m
  static constexpr double contact_restitution = .2;
};

}  // namespace slrr::physics
