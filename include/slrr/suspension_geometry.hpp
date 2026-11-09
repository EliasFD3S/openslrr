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
};
// Native 0x44FA70 arm-circle projection and hub sentinel -1 / strut mode 0.
// Inputs are raw Java setArm/setHub arguments, in chassis metres.
// False leaves the caller's existing approximation intact for unsupported or
// degenerate geometry. This is pose geometry, not a multi-body force solver.
bool solve_corner_geometry(const CornerGeometry &, Vec3 spring_point,
                           CornerFrames &);
Matrix corner_matrix(Vec3 origin, Quaternion rotation);
// Shared physical ray-to-arm/hub projection used by native and Chrono setup.
bool hinge_hub(const WheelConfig &wheel, Vec3 ray, Vec3 &hub);
// Backend-neutral hub travel, motion ratio, and full multi-body compatibility.
// These contain no Chrono types and are shared by eligibility and construction.
double suspension_hub_length(const WheelConfig &wheel, const Vec3 &hub);
double suspension_motion_ratio(const WheelConfig &wheel, const Vec3 &hub,
                               const Vec3 &direction);
bool chrono_suspension_geometry(const WheelConfig &wheel, double initial);
CornerFrames wheel_pose(const WheelConfig& wheel, double length, bool* fitted = nullptr);
Quaternion wheel_steering(double angle);
} // namespace slrr::physics
