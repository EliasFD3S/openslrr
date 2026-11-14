#include "slrr/flap_hinge.hpp"

#include <algorithm>
#include <cmath>

namespace slrr::physics {
namespace {

using T = FlapTuning;

double dot(const double a[3], const double b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
void cross(const double a[3], const double b[3], double out[3]) {
  const double x = a[1] * b[2] - a[2] * b[1];
  const double y = a[2] * b[0] - a[0] * b[2];
  const double z = a[0] * b[1] - a[1] * b[0];
  out[0] = x;
  out[1] = y;
  out[2] = z;
}
// Component of v normal to the unit axis a.
void normal_part(const double v[3], const double a[3], double out[3]) {
  const double along = dot(v, a);
  for (int i = 0; i < 3; ++i) out[i] = v[i] - along * a[i];
}
// Rodrigues rotation of v about the unit axis a.
void rotate(const double v[3], const double a[3], double angle, double out[3]) {
  const double c = std::cos(angle), s = std::sin(angle);
  double ax[3];
  cross(a, v, ax);
  const double along = dot(a, v) * (1 - c);
  for (int i = 0; i < 3; ++i) out[i] = v[i] * c + ax[i] * s + a[i] * along;
}
bool finite3(const double v[3]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// Distance from the hinge line to the free edge, the speed reference for
// latching and seal kicks.
double edge(const FlapGeometry &g) { return std::max(g.length, .1); }

void release(FlapState &state) {
  state.latched = false;
  state.open = true;
  state.hold = false;
  state.driving = false;
  state.since_release = 0;
}

void latch(FlapState &state) {
  state.angle = 0;
  state.rate = 0;
  state.latched = true;
  state.open = state.hold = state.driving = false;
}

// Angular acceleration that a force per unit mass f at the centre of mass
// gives the panel about the hinge, at the current angle.
double load(const FlapGeometry &g, const double lever[3], const double f[3]) {
  double torque[3];
  cross(lever, f, torque);
  return dot(torque, g.axis) / g.inertia;
}

}  // namespace

bool flap_geometry_from_cfg(const float pivot[3], const float axis[3],
                            const float dir[3], float lo, float hi,
                            FlapGeometry &out) {
  FlapGeometry g;
  double a[3] = {axis[0], axis[1], axis[2]};
  const double length = std::sqrt(dot(a, a));
  if (!(length > 1e-9) || !std::isfinite(length) || !std::isfinite(lo) ||
      !std::isfinite(hi))
    return false;
  // The open stop is the limit farther from the closed pose. Flip the axis
  // when it is negative, so travel is always [lower <= 0, upper > 0].
  const double sign = std::fabs(lo) > std::fabs(hi) ? -1. : 1.;
  const double open = std::fabs(lo) > std::fabs(hi) ? lo : hi;
  const double other = std::fabs(lo) > std::fabs(hi) ? hi : lo;
  if (!(std::fabs(open) > 1e-4)) return false;
  for (int i = 0; i < 3; ++i) {
    g.pivot[i] = pivot[i];
    g.axis[i] = a[i] / length * sign;
  }
  g.upper = std::fabs(open);
  // Zero is the CFG pose, where the latch sits; keep it inside the travel.
  g.lower = std::min(0., other * sign);
  double d[3] = {dir[0], dir[1], dir[2]}, arm[3];
  normal_part(d, g.axis, arm);
  const double reach = std::sqrt(dot(arm, arm));
  if (reach > 1e-9 && finite3(arm)) {
    for (int i = 0; i < 3; ++i) g.arm[i] = arm[i] / reach;
  } else {
    // No usable direction: any unit vector normal to the axis.
    const double helper[3] = {std::fabs(g.axis[1]) < .9 ? 0. : 1.,
                              std::fabs(g.axis[1]) < .9 ? 1. : 0., 0.};
    cross(g.axis, helper, arm);
    const double n = std::sqrt(dot(arm, arm));
    for (int i = 0; i < 3; ++i) g.arm[i] = arm[i] / n;
  }
  for (int i = 0; i < 3; ++i) g.com[i] = g.arm[i] * g.length * .5;
  g.inertia = g.length * g.length / 3;
  out = g;
  return true;
}

void flap_size_from_points(FlapGeometry &g, const double (*points)[3],
                           size_t count, double mass) {
  if (std::isfinite(mass) && mass > 0) g.mass = std::max(1., mass);
  double near_along = 1e30, far_along = -1e30, low_axis = 1e30, high_axis = -1e30;
  double centre[3]{};
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!finite3(points[i])) continue;
    const double r[3] = {points[i][0] - g.pivot[0], points[i][1] - g.pivot[1],
                         points[i][2] - g.pivot[2]};
    const double along = dot(r, g.arm), across = dot(r, g.axis);
    near_along = std::min(near_along, along);
    far_along = std::max(far_along, along);
    low_axis = std::min(low_axis, across);
    high_axis = std::max(high_axis, across);
    for (int k = 0; k < 3; ++k) centre[k] += r[k];
    ++used;
  }
  if (!used) return;
  // A panel reaches from its hinge to its free edge. Clamp against meshes
  // whose bounds are far off the hinge line (a stray LOD, a tiny trim part).
  g.length = std::clamp(far_along, .1, 3.);
  g.width = std::clamp(high_axis - low_axis, .1, 3.);
  for (int k = 0; k < 3; ++k) centre[k] /= static_cast<double>(used);
  double com[3];
  normal_part(centre, g.axis, com);
  if (dot(com, g.arm) < .05 * g.length) {
    // The bounds centre is on or behind the hinge: fall back to mid-panel.
    for (int k = 0; k < 3; ++k) com[k] = g.arm[k] * g.length * .5;
  }
  for (int k = 0; k < 3; ++k) g.com[k] = com[k];
  const double span = std::clamp(far_along - std::max(near_along, -g.length), .1, 3.);
  // Plate about its centre of mass plus the parallel-axis term.
  g.inertia = dot(com, com) + span * span / 12;
}

void flap_transform(FlapGeometry &g, const float m[16]) {
  const auto apply = [&](double (&v)[3], double w) {
    const double x = v[0], y = v[1], z = v[2];
    for (int r = 0; r < 3; ++r)
      v[r] = m[r] * x + m[4 + r] * y + m[8 + r] * z + m[12 + r] * w;
  };
  apply(g.pivot, 1);
  apply(g.axis, 0);
  apply(g.arm, 0);
  apply(g.com, 0);
}

bool flap_toggle(FlapState &state) {
  if (state.open) {
    // Close: drive it shut; it latches when it gets there.
    state.open = state.hold = false;
    state.driving = !state.latched;
  } else {
    release(state);
    state.hold = state.driving = true;
  }
  return state.open;
}

void flap_carrier_impulse(FlapState &state, const FlapGeometry &g,
                          const double dv[3], const double dw[3]) {
  if (state.latched || !finite3(dv) || !finite3(dw)) return;
  double lever[3];
  rotate(g.com, g.axis, state.angle, lever);
  // A free panel keeps its velocity: relative to the carrier it gets -dv at
  // its centre of mass and -dw about the hinge.
  const double minus_dv[3] = {-dv[0], -dv[1], -dv[2]};
  double change = load(g, lever, minus_dv) - dot(dw, g.axis);
  change = std::clamp(change, -T::max_rate, T::max_rate);
  state.rate = std::clamp(state.rate + change, -T::max_rate, T::max_rate);
}

bool flap_hit(FlapState &state, const FlapGeometry &g, const double point[3],
              const double normal[3], double speed) {
  if (!(speed >= 1) || !finite3(point) || !finite3(normal)) return false;
  bool changed = false;
  if (state.latched) {
    if (speed < T::unlatch_speed) return false;
    release(state);
    // The compressed seal pushes a released panel off its stop.
    state.rate = T::seal_kick / edge(g);
    changed = true;
  }
  // The obstacle pushes the struck point along the normal. Velocity matching
  // at that point is limited to the outer part of the panel, so a hit right on
  // the hinge line cannot spin it arbitrarily fast.
  const double r[3] = {point[0] - g.pivot[0], point[1] - g.pivot[1],
                       point[2] - g.pivot[2]};
  double radial[3], tangent[3];
  normal_part(r, g.axis, radial);
  const double distance = std::sqrt(dot(radial, radial));
  if (distance < 1e-3) return changed;
  cross(g.axis, radial, tangent);  // |tangent| == distance
  const double push = speed * dot(normal, tangent) / distance;
  const double radius = std::max(distance, .4 * edge(g));
  if (std::fabs(push) < 1e-6) return changed;
  state.rate = std::clamp(state.rate + push / radius, -T::max_rate, T::max_rate);
  return true;
}

FlapContact flap_contact(FlapState &state, const FlapGeometry &g,
                         const double point[3], const double normal[3],
                         double speed, double depth) {
  if (!finite3(point) || !finite3(normal) || !std::isfinite(speed) ||
      !std::isfinite(depth))
    return FlapContact::Blocked;
  // Harder than the hinge pins and the door check can take.
  if (speed >= T::tear_speed) return FlapContact::Torn;
  const double r[3] = {point[0] - g.pivot[0], point[1] - g.pivot[1],
                       point[2] - g.pivot[2]};
  double radial[3], tangent[3];
  normal_part(r, g.axis, radial);
  const double distance = std::sqrt(dot(radial, radial));
  // Share of the push that turns the panel about its hinge.
  double along = 0;
  if (distance > 1e-3) {
    cross(g.axis, radial, tangent);
    along = dot(normal, tangent) / distance;
  }
  const double crush = std::max(depth, 0.);
  if (std::fabs(along) < .2)
    return crush > T::tear_depth ? FlapContact::Torn : FlapContact::Blocked;
  // Turn out of the overlap, as far as the stops let it.
  const double wanted = state.angle + std::clamp(crush / (distance * along), -.3, .3);
  const double reached = std::clamp(wanted, g.lower, g.upper);
  const double left = std::fabs(wanted - reached) * distance * std::fabs(along);
  state.angle = reached;
  // Lose the approach speed, with a little bounce.
  if (speed > 0) {
    const double change = (1 + T::contact_restitution) * speed / (distance * along);
    state.rate = std::clamp(state.rate + change, -T::max_rate, T::max_rate);
  }
  if (reached != wanted) {
    // Pressed onto a stop: what the swing could not clear stays an overlap.
    if ((reached <= g.lower && state.rate < 0) || (reached >= g.upper && state.rate > 0))
      state.rate = 0;
    if (left > T::tear_depth) return FlapContact::Torn;
    return FlapContact::Blocked;
  }
  return FlapContact::Pushed;
}

bool flap_step(FlapState &state, const FlapGeometry &g, const FlapLoads &loads,
               double seconds) {
  if (!(seconds > 0) || !std::isfinite(seconds)) return false;
  if (state.latched) {
    const bool moved = state.angle != 0;
    state.angle = state.rate = 0;
    return moved;
  }
  const double before = state.angle;
  state.since_release += seconds;
  double lever[3];
  rotate(g.com, g.axis, state.angle, lever);
  double accel = load(g, lever, loads.gravity);
  // Relative wind on the plate, applied at the centre of mass. It vanishes
  // in the closed pose, where the panel is part of the body shell.
  const double air2 = dot(loads.air, loads.air);
  if (air2 > 1 && finite3(loads.air)) {
    double plate_arm[3], plate_normal[3];
    rotate(g.arm, g.axis, state.angle, plate_arm);
    cross(g.axis, plate_arm, plate_normal);
    const double exposure = std::min(1., std::fabs(state.angle) / .35);
    const double pressure = .5 * T::air_density * T::drag * g.length * g.width *
                            exposure / g.mass;
    const double along = dot(loads.air, plate_normal);
    const double f[3] = {pressure * along * std::sqrt(air2) * plate_normal[0],
                         pressure * along * std::sqrt(air2) * plate_normal[1],
                         pressure * along * std::sqrt(air2) * plate_normal[2]};
    accel += load(g, lever, f);
  }
  accel -= T::friction * state.rate;
  // A load that forces a held panel well back from its open stop beats the
  // strut or door check: from there it swings freely.
  if (state.open && state.hold && !state.driving &&
      state.angle < g.upper - T::hold_giveway)
    state.hold = false;
  if (state.open && state.hold) {
    // Strut: pushes the panel past the open stop so the stop holds it. While
    // the script opening is under way it is a firm drive; afterwards it only
    // beats gravity at the open pose and a small detent.
    const double w = T::drive_frequency;
    double drive = w * w * (g.upper + T::hold_margin - state.angle) - 2 * w * state.rate;
    double limit = T::drive_limit;
    if (!state.driving) {
      double open_lever[3];
      rotate(g.com, g.axis, g.upper, open_lever);
      limit = std::min(T::drive_limit,
                       T::hold_detent + 1.25 * std::fabs(load(g, open_lever, loads.gravity)));
    }
    accel += std::clamp(drive, -limit, limit);
  } else if (!state.open && state.driving) {
    // Script close: drive it shut through the closed pose so it latches.
    const double w = T::drive_frequency;
    const double drive = w * w * (-T::hold_margin - state.angle) - 2 * w * state.rate;
    accel += std::clamp(drive, -T::drive_limit, T::drive_limit);
  }
  state.rate = std::clamp(state.rate + accel * seconds, -T::max_rate, T::max_rate);
  state.angle += state.rate * seconds;
  if (state.angle >= g.upper) {
    state.angle = g.upper;
    if (state.rate > 0) state.rate *= -T::open_restitution;
    if (std::fabs(state.rate) < .05) state.rate = 0;
  }
  // The script opening ends at the stop, or just short of it where a heavy
  // panel settles against the drive.
  if (state.driving && state.open && std::fabs(state.rate) < .05 &&
      (state.angle >= g.upper - .005 ||
       (state.angle > g.upper - .05 && std::fabs(state.rate) < .02)))
    state.driving = false;
  const bool driven = !state.open && state.driving;
  if (state.angle <= 0 && (before > 0 || driven)) {
    // Reaching the closed pose. A script close always latches; a free swing
    // latches when it slams shut after the release has settled.
    const bool slam = before > 0 && -state.rate * edge(g) >= T::latch_speed &&
                      state.since_release >= T::latch_delay;
    if (state.latch_broken) {
      // A bent panel does not catch. A script close keeps pressing it onto
      // its stop, unlatched; a free one bounces off it.
    } else if (driven || slam) {
      latch(state);
      return before != 0;
    }
  }
  if (state.angle <= g.lower) {
    state.angle = g.lower;
    if (state.rate < 0) state.rate *= -(g.lower < 0 ? T::open_restitution
                                                     : T::closed_restitution);
    if (std::fabs(state.rate) < .05) state.rate = 0;
  }
  // A free panel that settled shut too gently to slam the latch still
  // catches once it rests in the closed pose.
  if (!state.hold && !state.latch_broken && std::fabs(state.angle) < 1e-3 &&
      std::fabs(state.rate) < .05 && state.since_release >= T::latch_delay) {
    latch(state);
    return before != 0;
  }
  return state.angle != before;
}

void flap_matrix(const FlapGeometry &g, double angle, float out[16]) {
  const double c = std::cos(angle), s = std::sin(angle), t = 1 - c;
  const double x = g.axis[0], y = g.axis[1], z = g.axis[2];
  double m[16]{};
  m[0] = t * x * x + c;
  m[1] = t * x * y + s * z;
  m[2] = t * x * z - s * y;
  m[4] = t * x * y - s * z;
  m[5] = t * y * y + c;
  m[6] = t * y * z + s * x;
  m[8] = t * x * z + s * y;
  m[9] = t * y * z - s * x;
  m[10] = t * z * z + c;
  m[15] = 1;
  // p' = R (p - pivot) + pivot
  for (int r = 0; r < 3; ++r)
    m[12 + r] = g.pivot[r] - (m[r] * g.pivot[0] + m[4 + r] * g.pivot[1] +
                              m[8 + r] * g.pivot[2]);
  for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(m[i]);
}

}  // namespace slrr::physics
