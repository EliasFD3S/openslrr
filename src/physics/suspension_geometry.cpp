#include "slrr/suspension_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
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
    // The hub turns about its kingpin: the line the fit lays on the strut.
    solved.steer_axis = target.normalized();
  }
  if (!solved.arm_origin.finite() || !solved.hub_origin.finite())
    return false;
  out = solved;
  return true;
}

namespace {
// The caster trail of a solved strut fit (see caster_trail).
double fitted_caster_trail(const WheelConfig& wheel, const CornerFrames& frames) {
  constexpr double none = std::numeric_limits<double>::quiet_NaN();
  // setHub 0x441880: the strut top is reference + p3 - p1 (solve_corner_geometry).
  const Vec3 hub_point{wheel.hub[1], wheel.hub[2], wheel.hub[3]};
  const Vec3 upper = wheel.center + Vec3{wheel.hub[7], wheel.hub[8], wheel.hub[9]} - hub_point;
  const Vec3 axis = upper - frames.arm_origin;
  if (!(axis.y > 1e-3)) return none;
  // Forward is -z: the axis leaning back at the top meets the ground ahead.
  // Under a degree of inclination (the stock struts' 0.3 from the hub
  // offset) counts as none.
  if (std::abs(axis.z) < std::sin(std::numbers::pi / 180) * axis.length()) return none;
  const Vec3 patch = frames.hub_origin - Vec3{0, wheel.radius, 0};
  const Vec3 ground = frames.arm_origin + axis * ((patch.y - frames.arm_origin.y) / axis.y);
  const double trail = -(ground.z - patch.z);
  return std::isfinite(trail) ? trail : none;
}

bool strut(const WheelConfig& wheel) {
  return wheel.has_hub && wheel.hub[0] == 0 && wheel.radius > 0;
}

// Where the kingpin meets the ground (#458). The axis keeps the fit's
// direction (inclination and caster) but not its place: the stock strut's
// ball joint sits 7.7 cm outboard of the wheel centre, so its line meets the
// ground about 17 cm outside the patch, and the wheel would drop as it
// steered. A road car's meets the ground at its caster trail ahead and a
// scrub radius outboard of the patch.
Vec3 kingpin_ground(const WheelConfig& wheel, const CornerFrames& frames) {
  const Vec3 patch = frames.hub_origin - Vec3{0, wheel.radius, 0};
  const double outboard = wheel.center.x < 0 ? -1 : 1;
  const double trail = fitted_caster_trail(wheel, frames);
  return patch + Vec3{outboard * kTypicalScrubRadius, 0, -(std::isfinite(trail) ? trail : 0)};
}

// The steering rotation in the body's frame, about the kingpin's direction.
Quaternion kingpin_turn(const CornerFrames& frames, double steer) {
  const Quaternion hub = frames.hub_rotation.normalized();
  const Quaternion inverse{-hub.x, -hub.y, -hub.z, hub.w};
  return hub * wheel_steering(frames.steer_axis, steer) * inverse;
}
}  // namespace

double caster_trail(const WheelConfig& wheel, double length) {
  constexpr double none = std::numeric_limits<double>::quiet_NaN();
  if (!strut(wheel)) return none;
  bool fitted = false;
  const CornerFrames frames = wheel_pose(wheel, length, &fitted);
  return fitted ? fitted_caster_trail(wheel, frames) : none;
}

double kingpin_lift_rate(const WheelConfig& wheel, double length, double steer) {
  constexpr double none = std::numeric_limits<double>::quiet_NaN();
  if (!strut(wheel)) return none;
  bool fitted = false;
  const CornerFrames frames = wheel_pose(wheel, length, &fitted);
  if (!fitted) return none;
  const Vec3 ground = kingpin_ground(wheel, frames);
  const Vec3 axle = frames.hub_rotation.rotate({1, 0, 0});
  // The tyre stands on the lowest point of its circle.
  const auto lift = [&](double angle) {
    const Quaternion turn = kingpin_turn(frames, angle);
    const Vec3 centre = ground + turn.rotate(frames.hub_origin - ground);
    const Vec3 across = turn.rotate(axle);
    return -(centre.y - wheel.radius * std::sqrt(std::max(0.0, 1 - across.y * across.y)));
  };
  constexpr double h = 1e-3;
  return (lift(steer + h) - lift(steer - h)) / (2 * h);
}

Quaternion wheel_steering(double angle) {
  return {0, std::sin(angle * .5), 0, std::cos(angle * .5)};
}

Quaternion wheel_steering(Vec3 axis, double angle) {
  if (!(axis.length() > .5)) return wheel_steering(angle);
  axis = axis.normalized();
  // `angle` is the wheel's heading on the road: about an axis leaning by
  // beta the heading turns by atan(tan(angle) cos beta), so turn further.
  const double turn = std::atan2(std::sin(angle), std::cos(angle) * std::max(.5, axis.y));
  return axis_angle(axis, turn);
}

namespace {
// The arm fit at `length`, without the camber curve or steering.
bool arm_frames(const WheelConfig& wheel, double length, CornerFrames& frames) {
  if (!(wheel.arm_length > 0)) return false;
  const double travel = length - wheel.anchor_length();
  CornerGeometry geometry;
  geometry.reference = wheel.center;
  const Vec3 pivot = wheel.arm_pivot - wheel.center, axis = wheel.arm_axis;
  geometry.arm = {float(wheel.arm_length), float(pivot.x), float(pivot.y),
                  float(pivot.z), float(axis.x), float(axis.y), float(axis.z)};
  geometry.hub = wheel.hub;
  geometry.alignment = wheel.hub_alignment;
  if (!wheel.has_hub) geometry.hub[0] = -1;
  // The fitted hub is driven by the vertical CFG travel, with alignment
  // applied by the hub fit. Tilting the input ray applies it twice.
  if (!solve_corner_geometry(geometry, wheel.center + Vec3{0, -travel, 0}, frames)) return false;
  // A strut fit replaces the alignment (camber and toe) with its own hub
  // rotation; its static camber is the fit's tilt, but its static toe (the
  // script's `toe`, through setYpr) is OpenSLRR's, turned about the body's
  // vertical (#484).
  if (geometry.hub[0] == 0) {
    const Vec3 forward = wheel.hub_alignment.rotate({0, 0, -1});
    const double toe = std::atan2(-forward.x, -forward.z);
    if (std::isfinite(toe) && toe != 0) {
      const Quaternion yaw{0, std::sin(toe * .5), 0, std::cos(toe * .5)};
      frames.hub_rotation = (yaw * frames.hub_rotation).normalized();
    }
  }
  return true;
}

// +1 for a left wheel (-x), whose top leans inboard towards +x.
double outboard_side(const WheelConfig& wheel) { return wheel.center.x < 0 ? 1 : -1; }

// Camber and toe from the axle, which the hub's pitch (a strut fit's, for
// its caster) does not move: camber its rise outboard to inboard, toe its
// sweep back.
double frames_camber(const WheelConfig& wheel, const CornerFrames& frames) {
  const Vec3 axle = frames.hub_rotation.rotate({1, 0, 0});
  return std::asin(std::clamp(axle.y * outboard_side(wheel), -1.0, 1.0));
}

// Toe against the body, positive when the wheel's front turns inboard.
double frames_toe(const WheelConfig& wheel, const CornerFrames& frames) {
  const Vec3 axle = frames.hub_rotation.rotate({1, 0, 0});
  return std::asin(std::clamp(axle.z * outboard_side(wheel), -1.0, 1.0));
}

// The curves replace the geometry's change from the static height: the hub
// turns about its own centre by the difference, about the body's
// longitudinal axis for the camber (#481) and its vertical for the toe
// (#484).
void apply_curves(const WheelConfig& wheel, double length, CornerFrames& frames) {
  if (!(wheel.static_length > 0)) return;
  const double side = outboard_side(wheel);
  if (wheel.camber_curve) {
    const double correction = camber_curve_change(wheel, length) -
                              (frames_camber(wheel, frames) - wheel.reference_camber);
    if (std::isfinite(correction) && correction != 0) {
      const double angle = std::clamp(correction, -.35, .35) * side;
      const Quaternion roll{0, 0, std::sin(angle * .5), std::cos(angle * .5)};
      frames.hub_rotation = (roll * frames.hub_rotation).normalized();
    }
  }
  if (wheel.toe_curve) {
    const double correction =
        toe_curve_change(wheel, length) - (frames_toe(wheel, frames) - wheel.reference_toe);
    if (std::isfinite(correction) && correction != 0) {
      // Toe-in turns a left wheel's front towards +x: a negative yaw.
      const double angle = -std::clamp(correction, -.1, .1) * side;
      const Quaternion yaw{0, std::sin(angle * .5), 0, std::cos(angle * .5)};
      frames.hub_rotation = (yaw * frames.hub_rotation).normalized();
    }
  }
}
}  // namespace

double geometry_camber(const WheelConfig& wheel, double length) {
  CornerFrames frames;
  if (!arm_frames(wheel, length, frames)) return std::numeric_limits<double>::quiet_NaN();
  return frames_camber(wheel, frames);
}

double geometry_toe(const WheelConfig& wheel, double length) {
  CornerFrames frames;
  if (!arm_frames(wheel, length, frames)) return std::numeric_limits<double>::quiet_NaN();
  return frames_toe(wheel, frames);
}

void update_static_reference(WheelConfig& wheel, double load) {
  wheel.static_length = 0;
  if (!(wheel.camber_curve || wheel.toe_curve) || !(wheel.spring > 0) || !(wheel.rest > 0) ||
      !(load >= 0))
    return;
  const double length =
      std::clamp(wheel.rest - load / wheel.spring, wheel.minimum, std::max(wheel.minimum, wheel.maximum));
  constexpr double h = .0125;
  const double at = geometry_camber(wheel, length);
  const double bump = geometry_camber(wheel, length - h), droop = geometry_camber(wheel, length + h);
  const double toe = geometry_toe(wheel, length);
  if (!std::isfinite(at) || !std::isfinite(bump) || !std::isfinite(droop) || !std::isfinite(toe))
    return;
  wheel.static_length = length;
  wheel.reference_camber = at;
  wheel.reference_toe = toe;
  if (std::isfinite(wheel.camber_gain)) {
    wheel.reference_gain = wheel.camber_gain;
  } else {
    // A road car's arms gain -0.2 to -1.5 deg of camber per 25 mm of bump;
    // a hinge past level (the stock Baiern arms') gains positive camber.
    constexpr double degree_per_25mm = std::numbers::pi / 180 / .025;
    wheel.reference_gain =
        std::clamp((bump - droop) / (2 * h), -1.5 * degree_per_25mm, -.2 * degree_per_25mm);
  }
}

double camber_curve_change(const WheelConfig& wheel, double length) {
  if (!wheel.camber_curve || !(wheel.static_length > 0)) return 0;
  const double bump = wheel.static_length - length;
  return wheel.reference_gain * bump + .5 * wheel.camber_progression * bump * bump;
}

double toe_curve_change(const WheelConfig& wheel, double length) {
  if (!wheel.toe_curve || !(wheel.static_length > 0)) return 0;
  const double bump = wheel.static_length - length;
  return wheel.bump_steer * bump + .5 * wheel.bump_steer_progression * bump * bump;
}

CornerFrames state_pose(const WheelConfig& wheel, const WheelState& state, double length,
                        double steer, bool* fitted) {
  CornerFrames pose = wheel_pose(wheel, length, fitted, steer);
  pose.hub_origin += state.compliance_offset;
  if (wheel.live_axle && state.axle_camber != 0 && std::isfinite(state.axle_camber)) {
    const double angle = std::clamp(state.axle_camber, -.35, .35) * outboard_side(wheel);
    const Quaternion roll{0, 0, std::sin(angle * .5), std::cos(angle * .5)};
    pose.hub_rotation = (roll * pose.hub_rotation).normalized();
  }
  return pose;
}

CornerFrames wheel_pose(const WheelConfig& wheel, double length, bool* fitted, double steer) {
  const double travel = length - wheel.anchor_length();
  const Vec3 ray = wheel.center + wheel.down.normalized() * travel;
  if (fitted) *fitted = false;
  CornerFrames frames;
  if (arm_frames(wheel, length, frames)) {
    if (fitted) *fitted = true;
    apply_curves(wheel, length, frames);
    // Steered, a strut's hub swings about its kingpin (#458): the wheel
    // centre moves on an arc, and the inclination lowers it either way,
    // which lifts the car.
    if (steer != 0 && strut(wheel)) {
      const Vec3 ground = kingpin_ground(wheel, frames);
      frames.hub_origin = ground + kingpin_turn(frames, steer).rotate(frames.hub_origin - ground);
    }
    return frames;
  }
  return {ray, ray, {}, wheel.hub_alignment};
}
} // namespace slrr::physics
