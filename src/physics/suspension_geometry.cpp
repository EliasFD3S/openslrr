#include "slrr/suspension_geometry.hpp"
#include <algorithm>
#include <cmath>
namespace slrr::physics {
namespace {
Quaternion axis_angle(Vec3 axis, double angle) {
  axis = axis.normalized();
  const double s = std::sin(angle * .5);
  return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * .5)};
}
bool between(Vec3 from, Vec3 to, Quaternion &rotation) {
  const double lengths = from.length() * to.length();
  if (lengths < 1e-12)
    return false;
  const auto axis = from.cross(to);
  const double cosine = std::clamp(from.dot(to) / lengths, -1., 1.);
  if (axis.length() < 1e-10) {
    if (cosine < 0)
      return false;
    rotation = {};
    return true;
  }
  rotation = axis_angle(axis, std::acos(cosine));
  return true;
}
} // namespace
Matrix corner_matrix(Vec3 origin, Quaternion rotation) {
  Matrix m{};
  rotation = rotation.normalized();
  for (int c = 0; c < 3; ++c) {
    const Vec3 v = rotation.rotate(c == 0   ? Vec3{1, 0, 0}
                                   : c == 1 ? Vec3{0, 1, 0}
                                            : Vec3{0, 0, 1});
    m[c * 4] = float(v.x);
    m[c * 4 + 1] = float(v.y);
    m[c * 4 + 2] = float(v.z);
  }
  m[12] = float(origin.x);
  m[13] = float(origin.y);
  m[14] = float(origin.z);
  m[15] = 1;
  return m;
}
bool solve_corner_geometry(const CornerGeometry &g, Vec3 point,
                           CornerFrames &out) {
  if (g.arm[0] <= 0 || (g.hub[0] != -1 && g.hub[0] != 0) || !point.finite())
    return false;
  const Vec3 axis = Vec3{g.arm[4], g.arm[5], g.arm[6]}.normalized();
  if (axis.length() < .9)
    return false;
  // setArm 0x441770 adds the wheel anchor and subtracts CFG base+travel
  // from Y: in chassis space this leaves reference + raw arm point.
  const Vec3 pivot = g.reference + Vec3{g.arm[1], g.arm[2], g.arm[3]};
  const Vec3 hub_point{g.hub[1], g.hub[2], g.hub[3]};
  if (g.hub[0] >= 0)
    point.y += hub_point.y; // 0x44FD1C..0x44FD27
  const auto relative = point - pivot;
  const double along = relative.dot(axis);
  const Vec3 radial = relative - axis * along;
  const double radius = radial.length();
  const double horizontal_squared = radius * radius - relative.y * relative.y;
  if (radius < 1e-9 || horizontal_squared < -1e-8)
    return false;
  // 0x45018F..0x45031A: project to the arm circle, retaining hinge-axis
  // displacement. The sign witness is dot(relative,(axis.z,0,-axis.x)).
  const double witness = relative.dot({axis.z, 0, -axis.x});
  const double sign = witness > 0 ? 1 : witness < 0 ? -1 : 0;
  const double angle =
      std::atan2(relative.y, std::sqrt(std::max(0., horizontal_squared))) *
      sign;
  CornerFrames solved;
  solved.arm_rotation = axis_angle(axis, angle);
  solved.arm_origin = pivot + axis * along + radial * (g.arm[0] / radius);
  solved.hub_origin = solved.arm_origin;
  solved.hub_rotation = (solved.arm_rotation * g.alignment).normalized();
  if (g.hub[0] == 0) {
    // setHub 0x441880 normalizes p2 and stores pc+reference-p1.
    // 0x4508F0..0x450D14 intersects the strut line with the lower
    // attachment sphere, chooses the positive ray root, and fits the hub.
    const auto strut = Vec3{g.hub[4], g.hub[5], g.hub[6]}.normalized();
    if (strut.length() < .9)
      return false;
    const auto upper =
        g.reference + Vec3{g.hub[7], g.hub[8], g.hub[9]} - hub_point;
    const auto span = upper - solved.arm_origin;
    const double distance_squared = span.dot(span),
                 projection = hub_point.dot(strut);
    const double discriminant =
        distance_squared - hub_point.dot(hub_point) + projection * projection;
    if (distance_squared < 1e-12 || discriminant <= 0)
      return false;
    const double root = std::sqrt(discriminant);
    double length = projection + root;
    if (length < 0)
      length = projection - root;
    const auto target = strut * length - hub_point;
    if (!between(target, span, solved.hub_rotation))
      return false;
    solved.hub_origin =
        solved.arm_origin - solved.hub_rotation.rotate(hub_point);
  }
  if (!solved.arm_origin.finite() || !solved.hub_origin.finite())
    return false;
  out = solved;
  return true;
}
} // namespace slrr::physics
