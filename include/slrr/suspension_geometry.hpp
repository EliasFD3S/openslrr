#pragma once
#include "slrr/vehicle_dynamics.hpp"
namespace slrr::physics {
struct CornerGeometry {
  Vec3 reference;
  std::array<float, 7> arm{};
  std::array<float, 10> hub{};
  Quaternion alignment;
};
struct CornerFrames {
  Vec3 arm_origin, hub_origin;
  Quaternion arm_rotation, hub_rotation;
  // The steering axis in the hub's frame (#458): a strut's kingpin, the ball
  // joint to the strut top; otherwise the hub's vertical.
  Vec3 steer_axis{0, 1, 0};
};
// Native 0x44FA70 arm-circle projection and hub sentinel -1 / strut mode 0.
// Inputs are raw Java setArm/setHub arguments, in chassis metres.
// False leaves the caller's existing approximation intact for unsupported or
// degenerate geometry. This is pose geometry, not a multi-body force solver.
bool solve_corner_geometry(const CornerGeometry &, Vec3 spring_point,
                           CornerFrames &);
Matrix corner_matrix(Vec3 origin, Quaternion rotation);
// steer (rad, positive to the left) swings a strut's hub origin about its
// kingpin (#458); the hub rotation stays unsteered (see wheel_steering).
CornerFrames wheel_pose(const WheelConfig& wheel, double length, bool* fitted = nullptr,
                        double steer = 0);
Quaternion wheel_steering(double angle);
// Steering by `angle` (positive to the left) about a hub-frame axis.
Quaternion wheel_steering(Vec3 axis, double angle);
// Caster trail (#458): how far ahead of the contact patch the steering axis
// (the strut's ball joint to its top mount) meets the ground, in metres at
// suspension `length`, on level ground. NaN when the wheel has no strut fit or
// its axis has no side-view inclination: no stock strut authors caster (their
// setHub axes lie in the front-view plane), so a level axis means "not set".
double caster_trail(const WheelConfig& wheel, double length);
// Kingpin jacking (#458): how fast the tyre's lowest point drops below the
// body (m per radian of road steering angle, positive to the left) as the
// hub turns about a strut's kingpin at `steer`. The tyre's load against it is
// a steering torque of -load x rate: the inclination lifts the car either
// way, so the weight centres the wheel; caster lifts one side and lowers the
// other. The axis has the fit's direction through a typical road car's
// ground point (caster trail ahead, kTypicalScrubRadius outboard). NaN
// without a strut fit.
inline constexpr double kTypicalScrubRadius = .01;
double kingpin_lift_rate(const WheelConfig& wheel, double length, double steer);
} // namespace slrr::physics
