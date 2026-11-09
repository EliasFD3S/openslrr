#include "slrr/suspension_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace slrr::physics {
namespace {
Quaternion axis_angle(Vec3 axis, double angle) {
  axis = axis.normalized();
  const double s = std::sin(angle * .5);
  return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * .5)};
}
bool between(Vec3 from, Vec3 to, Quaternion &rotation) {
  if (from.dot(from) < 1e-16 || to.dot(to) < 1e-16)
    return false;
  from = from.normalized();
  to = to.normalized();
  const double cosine = std::clamp(from.dot(to), -1., 1.);
  if (cosine < -1 + 1e-10)
    return false;
  const auto axis = from.cross(to);
  rotation = Quaternion{axis.x, axis.y, axis.z, 1 + cosine}.normalized();
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
  const auto finite = [](float v) { return std::isfinite(v); };
  if (!(g.arm[0] > 0) || (g.hub[0] != -1 && g.hub[0] != 0) ||
      !point.finite() || !g.reference.finite() ||
      !std::all_of(g.arm.begin(), g.arm.end(), finite) ||
      !std::all_of(g.hub.begin(), g.hub.end(), finite) ||
      !std::isfinite(g.alignment.x) || !std::isfinite(g.alignment.y) ||
      !std::isfinite(g.alignment.z) || !std::isfinite(g.alignment.w))
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

double suspension_hub_length(const WheelConfig &wheel, const Vec3 &hub) {
  const Vec3 down = wheel.down.normalized();
  if (!(wheel.arm_length > 0))
    return wheel.anchor_length() + (hub - wheel.center).dot(down);
  const Vec3 axis = wheel.arm_axis.normalized();
  const Vec3 relative = hub - wheel.arm_pivot;
  const Vec3 radial = relative - axis * relative.dot(axis);
  const Vec3 normal = axis.cross(radial);
  const double denominator = normal.dot(down);
  if (normal.length() > 1e-12 && std::fabs(denominator) > 1e-9)
    return wheel.anchor_length() +
           normal.dot(wheel.arm_pivot - wheel.center) / denominator;
  return std::numeric_limits<double>::quiet_NaN();
}

double suspension_motion_ratio(const WheelConfig &wheel, const Vec3 &hub,
                               const Vec3 &direction) {
  constexpr double delta = 1e-5;
  return (suspension_hub_length(wheel, hub + direction * delta) -
          suspension_hub_length(wheel, hub - direction * delta)) /
         (2 * delta);
}

bool chrono_suspension_geometry(const WheelConfig &wheel, double initial) {
  const Vec3 down = wheel.down.normalized();
  if (down.length() < .9)
    return false;
  const auto ray = wheel.center + down * (initial - wheel.anchor_length());
  Vec3 physical;
  if (!hinge_hub(wheel, ray, physical))
    return false;
  const double top_mount = std::max(.1, wheel.maximum);
  const Vec3 anchor =
      wheel.center - down * (wheel.anchor_length() + top_mount);
  const double reference =
      (physical - anchor).length() + wheel.rest - initial;
  if (!(reference > 1e-6) || !std::isfinite(reference))
    return false;

  double ratio_sign = 0;
  for (double sample : {wheel.minimum,
                        std::clamp(wheel.rest, wheel.minimum, wheel.maximum),
                        wheel.maximum}) {
    const auto sample_ray =
        wheel.center + down * (sample - wheel.anchor_length());
    Vec3 sample_hub;
    if (!hinge_hub(wheel, sample_ray, sample_hub))
      return false;
    const Vec3 direction = (sample_hub - anchor).normalized();
    const double ratio = suspension_motion_ratio(wheel, sample_hub, direction);
    if (!std::isfinite(ratio) || std::fabs(ratio) < 1e-4 ||
        (ratio_sign && ratio_sign * ratio < 0))
      return false;
    ratio_sign = ratio;
  }
  return true;
}

Quaternion wheel_steering(double angle) {
  return {0, std::sin(angle * .5), 0, std::cos(angle * .5)};
}

CornerFrames wheel_pose(const WheelConfig& wheel, double length, bool* fitted) {
  const double travel = length - wheel.anchor_length();
  const Vec3 ray = wheel.center + wheel.down.normalized() * travel;
  if (fitted) *fitted = false;
  if (wheel.arm_length > 0) {
    CornerGeometry geometry;
    geometry.reference = wheel.center;
    const Vec3 pivot = wheel.arm_pivot - wheel.center, axis = wheel.arm_axis;
    geometry.arm = {float(wheel.arm_length), float(pivot.x), float(pivot.y),
                    float(pivot.z), float(axis.x), float(axis.y), float(axis.z)};
    geometry.hub = wheel.hub;
    geometry.alignment = wheel.hub_alignment;
    if (!wheel.has_hub) geometry.hub[0] = -1;
    CornerFrames frames;
    // The fitted hub is driven by the vertical CFG travel, with alignment
    // applied by the hub fit. Tilting the input ray applies it twice.
    if (solve_corner_geometry(geometry, wheel.center + Vec3{0, -travel, 0}, frames)) {
      if (fitted) *fitted = true;
      return frames;
    }
  }
  return {ray, ray, {}, wheel.hub_alignment};
}
} // namespace slrr::physics
