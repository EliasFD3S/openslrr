#include "slrr/suspension_geometry.hpp"
#include "slrr/dyno_model.hpp"
#include "slrr/vehicle_dynamics.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace slrr::physics {

double Vec3::length() const { return std::sqrt(dot(*this)); }

Vec3 Vec3::normalized() const {
  const double n = length();
  return n > 1e-12 ? *this / n : Vec3{};
}

bool Vec3::finite() const {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

Quaternion Quaternion::operator*(Quaternion b) const {
  return {w * b.x + x * b.w + y * b.z - z * b.y,
          w * b.y - x * b.z + y * b.w + z * b.x,
          w * b.z + x * b.y - y * b.x + z * b.w,
          w * b.w - x * b.x - y * b.y - z * b.z};
}

Quaternion Quaternion::normalized() const {
  double n = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(n > 1e-12) || !std::isfinite(n))
    throw std::runtime_error("invalid rigid orientation");
  return {x / n, y / n, z / n, w / n};
}

Vec3 Quaternion::rotate(Vec3 v) const {
  Vec3 axis{x, y, z};
  Vec3 t = axis.cross(v) * 2;
  return v + t * w + axis.cross(t);
}

Vec3 Quaternion::inverse_rotate(Vec3 v) const {
  return Quaternion{-x, -y, -z, w}.rotate(v);
}

void RigidBody::configure(double kilograms, Vec3 lo, Vec3 hi, Vec3 com) {
  if (!(kilograms > 0) || !std::isfinite(kilograms) || !lo.finite() ||
      !hi.finite() || !com.finite() || hi.x <= lo.x || hi.y <= lo.y ||
      hi.z <= lo.z)
    throw std::invalid_argument("invalid rigid body mass/bounds");
  mass = kilograms;
  minimum = lo;
  maximum = hi;
  center = com;

  // Les masses d'une voiture sont concentrées dans l'empattement et la voie,
  // pas dans les porte-à-faux ni le pavillon. Un facteur ~0.75 sur les dimensions
  // évite de surestimer l'inertie en lacet et en tangage.
  Vec3 s = (hi - lo) * 0.75;
  inverse_inertia_ = {12.0 / (mass * (s.y * s.y + s.z * s.z)),
                      12.0 / (mass * (s.x * s.x + s.z * s.z)),
                      12.0 / (mass * (s.x * s.x + s.y * s.y))};
}

void RigidBody::configure(double kilograms, Vec3 lo, Vec3 hi, Vec3 com,
                          Vec3 inertia) {
  configure(kilograms, lo, hi, com);
  if (inertia.finite() && inertia.x > 0 && inertia.y > 0 && inertia.z > 0) {
    inverse_inertia_ = {1.0 / inertia.x, 1.0 / inertia.y, 1.0 / inertia.z};
  }
}

Vec3 RigidBody::inertia() const {
  return {1.0 / inverse_inertia_.x, 1.0 / inverse_inertia_.y, 1.0 / inverse_inertia_.z};
}

void RigidBody::set_frame(const Matrix &m) {
  for (float v : m)
    if (!std::isfinite(v))
      throw std::invalid_argument("invalid rigid frame");
  const double trace = m[0] + m[5] + m[10];
  if (trace > 0) {
    double s = std::sqrt(trace + 1) * 2;
    orientation = {(m[6] - m[9]) / s, (m[8] - m[2]) / s, (m[1] - m[4]) / s,
                   s / 4};
  } else if (m[0] > m[5] && m[0] > m[10]) {
    double s = std::sqrt(1 + m[0] - m[5] - m[10]) * 2;
    orientation = {s / 4, (m[4] + m[1]) / s, (m[8] + m[2]) / s,
                   (m[6] - m[9]) / s};
  } else if (m[5] > m[10]) {
    double s = std::sqrt(1 + m[5] - m[0] - m[10]) * 2;
    orientation = {(m[4] + m[1]) / s, s / 4, (m[9] + m[6]) / s,
                   (m[8] - m[2]) / s};
  } else {
    double s = std::sqrt(1 + m[10] - m[0] - m[5]) * 2;
    orientation = {(m[8] + m[2]) / s, (m[9] + m[6]) / s, s / 4,
                   (m[1] - m[4]) / s};
  }
  orientation = orientation.normalized();
  position = {m[0] * center.x + m[4] * center.y + m[8] * center.z + m[12],
              m[1] * center.x + m[5] * center.y + m[9] * center.z + m[13],
              m[2] * center.x + m[6] * center.y + m[10] * center.z + m[14]};
  velocity = momentum = force_ = torque_ = {};
}

Matrix RigidBody::frame() const {
  Vec3 x = orientation.rotate({1, 0, 0}), y = orientation.rotate({0, 1, 0}),
       z = orientation.rotate({0, 0, 1});
  Vec3 p = position - orientation.rotate(center);
  return {float(x.x), float(x.y), float(x.z), 0,
          float(y.x), float(y.y), float(y.z), 0,
          float(z.x), float(z.y), float(z.z), 0,
          float(p.x), float(p.y), float(p.z), 1};
}

Vec3 RigidBody::inverse_world_inertia(Vec3 v) const {
  Vec3 a = orientation.inverse_rotate(v);
  return orientation.rotate({a.x * inverse_inertia_.x, a.y * inverse_inertia_.y,
                             a.z * inverse_inertia_.z});
}

Vec3 RigidBody::omega() const { return inverse_world_inertia(momentum); }

void RigidBody::set_omega(Vec3 angular_velocity) {
  const Vec3 local = orientation.inverse_rotate(angular_velocity);
  momentum = orientation.rotate({local.x / inverse_inertia_.x,
                                 local.y / inverse_inertia_.y,
                                 local.z / inverse_inertia_.z});
}

Vec3 RigidBody::point_velocity(Vec3 p) const {
  return velocity + omega().cross(p - position);
}

void RigidBody::add_force(Vec3 f, Vec3 p) {
  force_ += f;
  torque_ += (p - position).cross(f);
}

void RigidBody::impulse(Vec3 j, Vec3 p) {
  velocity += j / mass;
  momentum += (p - position).cross(j);
}

void RigidBody::integrate(double dt) {
  integrate_forces(dt);
  integrate_pose(dt);
}

void RigidBody::integrate_forces(double dt) {
  if (!std::isfinite(dt) || dt <= 0)
    throw std::invalid_argument("invalid physics dt");
  velocity += force_ * (dt / mass);
  velocity.y -= 9.81 * dt;
  momentum = (momentum + torque_ * dt) * std::exp(-0.015 * dt);
  force_ = torque_ = {};
  if (!velocity.finite() || !momentum.finite())
    throw std::runtime_error("non-finite rigid state");
}

void RigidBody::integrate_pose(double dt) {
  if (!std::isfinite(dt) || dt <= 0)
    throw std::invalid_argument("invalid physics dt");
  position += velocity * dt;
  Vec3 w = omega();
  double angle = w.length() * dt;
  if (angle > 1e-12) {
    Vec3 axis = w.normalized() * std::sin(angle * .5);
    orientation =
        (Quaternion{axis.x, axis.y, axis.z, std::cos(angle * .5)} * orientation)
            .normalized();
  }
  if (!position.finite() || !velocity.finite() || !momentum.finite())
    throw std::runtime_error("non-finite rigid state");
}

double RigidBody::contact(Vec3 p, Vec3 normal, double restitution,
                         double friction, RigidBody *other) {
  const Vec3 n = normal.normalized();
  const auto relative_velocity = [&] {
    return point_velocity(p) - (other ? other->point_velocity(p) : Vec3{});
  };
  const auto inverse_mass = [&](const RigidBody &body, Vec3 axis) {
    const Vec3 r = p - body.position;
    return 1 / body.mass +
           axis.dot(body.inverse_world_inertia(r.cross(axis)).cross(r));
  };
  const auto exchange = [&](Vec3 j) {
    impulse(j, p);
    if (other)
      other->impulse(-j, p);
  };
  const Vec3 v = relative_velocity();
  double vn = v.dot(n);
  if (vn >= 0)
    return 0;
  const double denom =
      inverse_mass(*this, n) + (other ? inverse_mass(*other, n) : 0);
  if (!(denom > 0))
    return 0;
  double magnitude = -(1 + (vn < -.8 ? restitution : 0)) * vn / denom;
  exchange(n * magnitude);
  const Vec3 post = other ? relative_velocity() : v;
  Vec3 tangent = post - n * post.dot(n);
  double speed = tangent.length();
  if (speed > 1e-8) {
    tangent = tangent / speed;
    const double d = inverse_mass(*this, tangent) +
                     (other ? inverse_mass(*other, tangent) : 0);
    exchange(tangent * (-std::min(friction * magnitude, speed / d)));
  }
  return magnitude;
}

std::array<Vec3, 8> RigidBody::corners() const {
  std::array<Vec3, 8> out;
  unsigned i = 0;
  for (double x : {minimum.x, maximum.x})
    for (double y : {minimum.y, maximum.y})
      for (double z : {minimum.z, maximum.z})
        out[i++] = position + orientation.rotate(Vec3{x, y, z} - center);
  return out;
}

Vec3 body_point(const RigidBody &body, Vec3 local) {
  return body.position + body.orientation.rotate(local - body.center);
}

double suspension_force(double length, double speed, double rest, double max,
                        double k, double bump, double rebound) {
  if (length >= max)
    return 0.0;
  const double spring_force = (rest - length) * k;
  const double damping_coeff =
      speed >= 0.0 ? (rebound > 0.0 ? rebound : 0.7 * bump) : bump;
  // Atténuation progressive de la détente près de la butée haute (top-out)
  // pour éviter un claquement numérique quand la roue décolle.
  const double travel_left = std::clamp((max - length) / std::max(0.02, max - rest), 0.0, 1.0);
  const double effective_damping = speed >= 0.0 ? damping_coeff * travel_left : damping_coeff;
  return std::max(0.0, spring_force - speed * effective_damping);
}

namespace {
// Typical real-world values, not recovered from the original game.
constexpr double air_density = 1.225;      // kg/m³, ISA sea level at 15 °C
constexpr double frontal_fill = .85;       // frontal area / (width × height)
constexpr double body_drag = .32;          // road-car Cd on the frontal area
constexpr double body_cross_drag = 1.0;    // crossflow Cd of the side and plan
constexpr double body_lift = .25;          // road-car CL on the frontal area
constexpr double wing_lift_slope = 4.2;    // per radian, 2π·A/(A+2) at A = 4
constexpr double wing_normal_limit = 1.5;  // stalled single-element wing
constexpr double wing_profile_drag = .05;
// Wind at car height: the log wind profile over open terrain (roughness
// length 0.03 m) at 1 m, about the height of a car's aerodynamic centre.
constexpr double wind_roughness = .03, wind_height = 1, wind_report_height = 10;
// Gusts: plane waves frozen in the moving air (Taylor's hypothesis), three
// along the wind and three across it, with fixed phases so a run repeats.
// Their 28–240 m wavelengths span the turbulence length scales near the
// ground.
struct Gust {
  double wavelength, angle, phase;  // m; wave direction from the wind, rad
  bool across;
};
constexpr Gust gusts[] = {{240, 0, 0, false},   {90, .6, 2.1, false},
                          {35, -.9, 4.4, false}, {160, 1.2, 1.3, true},
                          {60, -.4, 3.7, true},  {28, .3, 5.6, true}};

// Calls apply(force, point) with world vectors for each aerodynamic force.
template <class Apply>
void each_aero_force(const Aerodynamics &aero, const RigidBody &body, double dt,
                     Apply &&apply) {
  // One step's impulse never stops a point past rest along a drag force.
  const auto stopping_force = [&](Vec3 point, Vec3 axis, double speed) {
    const Vec3 arm = point - body.position;
    const double inverse =
        1 / body.mass + axis.dot(body.inverse_world_inertia(arm.cross(axis)).cross(arm));
    return inverse > 0 ? std::abs(speed) / (inverse * dt) : 0.0;
  };
  // EXE 0x456A83: Chassis.C_drag as -C |v| v above 1 m/s.
  if (aero.drag > 0) {
    const Vec3 point = body_point(body, aero.drag_center);
    const Vec3 velocity = body.point_velocity(point) - aero.wind;
    const double square = velocity.dot(velocity);
    if (square > 1) {
      const double speed = std::sqrt(square);
      const Vec3 direction = velocity / speed;
      apply(direction * -std::min(aero.drag * square,
                                  stopping_force(point, direction, speed)),
            point);
    }
  }
  // The body, from the air's velocity in the car's axes: drag along each
  // axis on the projected bounds, a side force at a quarter of the length
  // from the leading end, and lift from the flow along the car.
  if (aero.body) {
    const Vec3 size = body.maximum - body.minimum;
    const Vec3 centre = (body.minimum + body.maximum) * .5;
    const Vec3 point = body_point(body, centre);
    const Vec3 local = body.orientation.inverse_rotate(body.point_velocity(point) - aero.wind);
    const double frontal = frontal_fill * size.x * size.y;
    const double side = frontal_fill * size.z * size.y;
    const double plan = frontal_fill * size.x * size.z;
    // Frontal Cd on the flow along the car, crossflow Cd on the flow across
    // its side and plan views, each at the centre of the bounds.
    const auto axis_drag = [&](Vec3 axis, double speed, double cd_area) {
      if (!(std::abs(speed) > 1e-3)) return;
      const Vec3 direction = body.orientation.rotate(axis);
      apply(direction * -std::copysign(std::min(.5 * air_density * cd_area * speed * speed,
                                                stopping_force(point, direction, speed)),
                                       speed),
            point);
    };
    axis_drag({0, 0, 1}, local.z, body_drag * frontal);
    axis_drag({1, 0, 0}, local.x, body_cross_drag * side);
    axis_drag({0, 1, 0}, local.y, body_cross_drag * plan);
    // Slender-body side force: the side view as a wing of chord L and span
    // 2H, the ground mirroring it, so πAR/2 = πH/L per radian of yaw on the
    // side area, at the quarter chord.
    if (std::abs(local.x) > 1e-3 && std::abs(local.z) > 1e-3) {
      const Vec3 quarter{centre.x, centre.y,
                         local.z < 0 ? body.minimum.z + .25 * size.z
                                     : body.maximum.z - .25 * size.z};
      const Vec3 at = body_point(body, quarter);
      const Vec3 direction = body.orientation.rotate({1, 0, 0});
      const double slope = std::numbers::pi * size.y / size.z;
      apply(direction * -std::copysign(
                            std::min(.5 * air_density * slope * side *
                                         std::abs(local.x * local.z),
                                     stopping_force(at, direction, local.x)),
                            local.x),
            at);
    }
    apply(body.orientation.rotate({0, 1, 0}) *
              (.5 * air_density * body_lift * frontal * local.z * local.z),
          point);
  }
  // Wings: the face meeting the air carries the normal force of the whole
  // element, rising with incidence until it stalls, plus its profile drag.
  for (const auto &wing : aero.wings) {
    const Vec3 point = body_point(body, wing.position);
    const Vec3 normal = body.orientation.rotate(wing.normal);
    const Vec3 velocity = body.point_velocity(point) - aero.wind;
    const double square = velocity.dot(velocity);
    if (!(square > 1e-6)) continue;
    const double speed = std::sqrt(square);
    const double incidence = velocity.dot(normal) / speed;  // sin α
    if (incidence <= 0) continue;
    const double pressure = .5 * air_density * wing.area * square;
    apply(normal * -(pressure * std::min(wing_lift_slope * incidence, wing_normal_limit)) -
              velocity / speed * (pressure * wing_profile_drag),
          point);
  }
}
} // namespace

Vec3 wind_velocity(const Wind &wind, Vec3 position, double seconds) {
  if (!(wind.speed > 0) || !std::isfinite(wind.speed) || !std::isfinite(wind.heading) ||
      !position.finite() || !std::isfinite(seconds))
    return {};
  const double profile = std::log(wind_height / wind_roughness);
  const double mean = wind.speed * profile / std::log(wind_report_height / wind_roughness);
  // Turbulence intensity 1/ln(z/z0) along the wind (EN 1991-1-4 §4.4), 0.75
  // of it across; each set of three waves shares its variance.
  const double along_gust = mean / profile * std::sqrt(2. / 3);
  const double across_gust = .75 * along_gust;
  const double heading = wind.heading * std::numbers::pi / 180;
  const Vec3 along{std::sin(heading), 0, std::cos(heading)};
  const Vec3 across{along.z, 0, -along.x};
  // The point's coordinates in the air that the mean wind carries.
  const double x = position.dot(along) - mean * seconds, y = position.dot(across);
  Vec3 air = along * mean;
  for (const auto &gust : gusts) {
    const double phase = 2 * std::numbers::pi / gust.wavelength *
                             (x * std::cos(gust.angle) + y * std::sin(gust.angle)) +
                         gust.phase;
    air += (gust.across ? across * across_gust : along * along_gust) * std::sin(phase);
  }
  return air;
}

AeroLoad aero_load(const Aerodynamics &aero, const RigidBody &body, double dt) {
  AeroLoad load;
  each_aero_force(aero, body, dt, [&](Vec3 force, Vec3 point) {
    load.force += force;
    load.torque += (point - body.position).cross(force);
  });
  return load;
}

void VehicleDynamics::apply_aero() {
  each_aero_force(aero, body, dt_, [&](Vec3 force, Vec3 point) {
    if (force.finite()) body.add_force(force, point);
  });
}

// A passenger-car tyre's radial stiffness and damping, and how far it may
// squash before the rim (about half a 55-series sidewall): typical values,
// not recovered from the original game (#380).
constexpr double kTyreRate = 250000, kTyreDamping = 1500, kTyreDeflection = .06;

void solve_linked(RigidBody &body, const WheelConfig &w, WheelState &s,
                  const Terrain &terrain, double ceiling, double dt, double steer) {
  const Vec3 local_down = w.down.normalized();
  const Vec3 down = body.orientation.rotate(local_down);
  const Vec3 hard = body_point(body, w.center - local_down * w.anchor_length());
  const bool fresh = !s.seeded;
  if (!s.seeded) {
    s.length = std::clamp(w.rest, w.minimum, w.maximum);
    s.speed = 0;
    s.seeded = true;
  }
  s.length = std::clamp(s.length, w.minimum, w.maximum);
  const auto project = [&](double length) {
    return body_point(body, wheel_pose(w, length).hub_origin);
  };
  const auto sample = [&](Vec3 world, GroundHit &hit) {
    return down.y < -.05 &&
           terrain(world.x, world.z, std::max(ceiling, world.y + 1), hit);
  };
  // The tyre's footprint (#380): the ground under points along the rolling
  // direction and across the section, each kept as its local plane. A point
  // `along` the rolling direction reaches sqrt(R^2 - along^2) below the hub;
  // across, the tread edge and the sidewall sit higher (`drop`). On any one
  // plane the centre point is the deepest, as with the former single-point
  // contact; another point wins only at an edge such as a kerb, and lifts the
  // wheel progressively as the edge moves under the tyre.
  struct Footprint {
    Vec3 ground, normal;
    double reach = 0;
    bool valid = false;
  };
  struct Point {
    double along, side, drop;  // fractions of R (along) and metres
  };
  // Centre line: 7 points along the tread. Across: the tread edge (a third of
  // the half width up) and the sidewall bulge 2 cm beyond it (10 cm up),
  // near the bottom of the tyre only.
  const double hw = w.half_width;
  const Point points[] = {
      {0, 0, 0},         {-.3, 0, 0},        {.3, 0, 0},         {-.6, 0, 0},
      {.6, 0, 0},        {-.85, 0, 0},       {.85, 0, 0},
      {0, -hw, hw / 3},  {0, hw, hw / 3},    {-.4, -hw, hw / 3}, {-.4, hw, hw / 3},
      {.4, -hw, hw / 3}, {.4, hw, hw / 3},
      {0, -hw - .02, .1}, {0, hw + .02, .1}, {-.4, -hw - .02, .1}, {-.4, hw + .02, .1},
      {.4, -hw - .02, .1}, {.4, hw + .02, .1}};
  constexpr size_t kPoints = sizeof(points) / sizeof(points[0]);
  const size_t used = hw > 1e-3 ? kPoints : 7;
  // After the grid, one refined point per direction (front, back, left,
  // right) at the exact position of the nearest step.
  std::array<Footprint, kPoints + 4> footprint{};
  const auto sample_footprint = [&](Vec3 hub) {
    const Quaternion turn = body.orientation * wheel_pose(w, s.length).hub_rotation *
                            wheel_steering(steer);
    Vec3 forward = turn.rotate({0, 0, -1}), across = turn.rotate({1, 0, 0});
    forward.y = across.y = 0;
    forward = forward.length() > 1e-6 ? forward.normalized() : Vec3{0, 0, -1};
    across = across.length() > 1e-6 ? across.normalized() : Vec3{1, 0, 0};
    for (size_t k = 0; k < used; ++k) {
      auto &f = footprint[k];
      const double along = points[k].along * w.radius;
      const Vec3 point = hub + forward * along + across * points[k].side;
      GroundHit hit;
      f.valid = sample(point, hit);
      if (!f.valid) continue;
      f.ground = {point.x, hit.height, point.z};
      f.normal = hit.normal.normalized();
      f.reach = std::sqrt(std::max(0.0, w.radius * w.radius - along * along)) - points[k].drop;
    }
    // Grid points are centimetres apart, so a kerb edge between two of them
    // would lift the wheel in steps (a step of a few centimetres bottoms the
    // suspension). Walking out from the centre, find the first point that
    // stands on higher ground than the one inside it and bisect for the edge:
    // the point just past it lifts the wheel continuously as the edge moves.
    const auto at = [&](const Point &p) {
      return hub + forward * (p.along * w.radius) + across * p.side;
    };
    const auto refine = [&](size_t slot, std::initializer_list<size_t> walk) {
      auto &out = footprint[kPoints + slot];
      out.valid = false;
      size_t inner = *walk.begin();
      for (auto it = walk.begin() + 1; it != walk.end(); ++it) {
        const size_t outer = *it;
        if (outer >= used) return;
        const auto &fi = footprint[inner], &fo = footprint[outer];
        if (!fi.valid || !fo.valid) return;
        // Higher ground: the outer point stands above the inner point's plane.
        const auto higher = [&](const Vec3 &ground) {
          return fi.normal.dot(ground - fi.ground) > .005;
        };
        if (higher(fo.ground)) {
          Point lo = points[inner], hi = points[outer];
          Footprint best = fo;
          for (int i = 0; i < 6; ++i) {
            const Point mid{(lo.along + hi.along) * .5, (lo.side + hi.side) * .5,
                            (lo.drop + hi.drop) * .5};
            const Vec3 point = at(mid);
            GroundHit hit;
            if (!sample(point, hit)) break;
            const Vec3 ground{point.x, hit.height, point.z};
            if (higher(ground)) {
              hi = mid;
              const double along = mid.along * w.radius;
              best.ground = ground;
              best.normal = hit.normal.normalized();
              best.reach =
                  std::sqrt(std::max(0.0, w.radius * w.radius - along * along)) - mid.drop;
            } else {
              lo = mid;
            }
          }
          out = best;
          return;
        }
        inner = outer;
      }
    };
    refine(0, {0, 2, 4, 6});
    refine(1, {0, 1, 3, 5});
    refine(2, {0, 7, 13});
    refine(3, {0, 8, 14});
  };
  // Penetration of the deepest footprint sample for a hub at `world`, and
  // which sample it is (-1: none).
  const auto tyre_error = [&](Vec3 world, int &deepest) {
    double error = -1e9;
    deepest = -1;
    for (size_t k = 0; k < footprint.size(); ++k) {
      const auto &f = footprint[k];
      if (!f.valid) continue;
      const double e = f.normal.dot(f.ground - world) + f.reach;
      if (e > error + 1e-9) {
        error = e;
        deepest = static_cast<int>(k);
      }
    }
    return error;
  };
  // The contact normal at a hub: the deepest sample's plane, unless that plane
  // does not reach under the hub (an edge, such as a kerb's): then the tyre
  // touches the edge itself, and the normal runs from the edge to the hub.
  const auto contact_normal_at = [&](Vec3 hub, int deepest) {
    const auto &f = footprint[static_cast<size_t>(deepest)];
    if (deepest == 0) return f.normal;
    const Vec3 foot = hub - f.normal * (f.normal.dot(hub - f.ground));
    GroundHit under;
    const bool same_plane =
        sample(foot, under) &&
        std::fabs(f.normal.dot(Vec3{foot.x, under.height, foot.z} - f.ground)) < .01;
    const Vec3 edge = hub - f.ground;
    return same_plane || edge.length() <= 1e-6 ? f.normal : edge.normalized();
  };
  if (w.unsprung > 0) {
    // Quarter car (#380). The wheel is a mass between the suspension (spring
    // and damper to the body) and the tyre (a radial spring and damper to the
    // ground), integrated implicitly along the suspension travel. It used to be
    // placed kinematically on the ground each step: a rigid tyre, so a kerb
    // edge lifted it at v * tan(angle), up to 1.6 times the car's speed, into
    // a firm damper, and the car jumped. Now the tyre squashes on the edge
    // first and the wheel rises with the suspension.
    double l0 = s.length, v0 = s.speed;
    Vec3 hub0 = project(l0);
    sample_footprint(hub0);
    int deepest = -1;
    double p0 = tyre_error(hub0, deepest);
    // Hub displacement per metre of travel (pointing out of the body).
    const double probe = l0 < w.maximum - 1e-4 ? l0 + 1e-3 : l0 - 1e-3;
    const Vec3 axis = (project(probe) - hub0) / (probe - l0);
    const Vec3 axis_dir = axis.length() > 1e-9 ? axis.normalized() : down;
    if (fresh && deepest >= 0) {
      // A new wheel starts on the ground, not inside it.
      const double g = -axis.dot(footprint[static_cast<size_t>(deepest)].normal);
      if (g > 1e-3) {
        l0 = std::clamp(l0 - p0 / g, w.minimum, w.maximum);
        v0 = 0;
        hub0 = project(l0);
        p0 = tyre_error(hub0, deepest);
      }
    }
    Vec3 n{0, 1, 0};
    double g = 0, body_rate = 0;
    if (deepest >= 0) {
      const Vec3 plane = footprint[static_cast<size_t>(deepest)].normal;
      n = contact_normal_at(hub0, deepest);
      // Penetration per metre of extension, and its rate from the body's motion.
      g = std::max(0.0, -axis.dot(plane));
      body_rate = -body.point_velocity(hub0).dot(plane);
    } else {
      p0 = -1;
    }
    const double m = w.unsprung;
    // No gravity on the wheel: the body's mass already includes the wheels,
    // and their weight reaches the tyres through it. Counting it again added
    // 7 % of grip in the Lab.
    const double gravity = 0;
    const double spring = s.length < w.rest ? w.spring : 0;
    // Solve m (v1 - v0) / dt = suspension(l1, v1) - tyre(l1, v1) g + m gravity
    // with l1 = l0 + dt v1, the tyre active only while it penetrates.
    const auto solve = [&](bool tyre, double damping) {
      double lhs = m / dt + spring * dt + damping;
      double rhs = m * v0 / dt + spring * (w.rest - l0) + m * gravity;
      if (tyre) {
        lhs += g * (kTyreRate * g * dt + kTyreDamping * g);
        rhs -= g * (kTyreRate * (p0 + body_rate * dt) + kTyreDamping * body_rate);
      }
      return rhs / lhs;
    };
    const auto damping_for = [&](double v) {
      return v >= 0 ? (w.rebound_damping > 0 ? w.rebound_damping : .7 * w.bump_damping)
                    : w.bump_damping;
    };
    bool tyre = p0 > 0;
    double v1 = solve(tyre, damping_for(v0));
    v1 = solve(tyre, damping_for(v1));
    const auto penetration = [&](double v) { return p0 + (body_rate + g * v) * dt; };
    if (tyre != (penetration(v1) > 0)) {
      tyre = !tyre;
      v1 = solve(tyre, damping_for(v1));
    }
    double l1 = l0 + v1 * dt;
    bool bottomed = false;
    if (l1 <= w.minimum) {
      l1 = w.minimum;
      v1 = std::max(0.0, v1);
      bottomed = true;
    } else if (l1 >= w.maximum) {
      l1 = w.maximum;
      v1 = std::min(0.0, v1);
    }
    const double p1 = p0 + body_rate * dt + g * (l1 - l0);
    const double p_rate = body_rate + g * v1;
    const double tyre_force =
        p1 > 0 ? std::clamp(kTyreRate * std::min(p1, kTyreDeflection) + kTyreDamping * p_rate,
                            0.0, body.mass * 9.81 * 3)
               : 0.0;
    double suspension = suspension_force(l1, v1, w.rest, w.maximum, w.spring, w.bump_damping,
                                         w.rebound_damping);
    const bool contact = tyre_force > 0;
    if (contact) {
      // Through the bump stop the body takes the whole tyre force; otherwise
      // the spring and damper along the travel, and the arms the tyre force's
      // part across it.
      const double along = n.dot(axis_dir);
      if (bottomed) body.add_force(n * tyre_force, hard);
      else body.add_force(axis_dir * -suspension + (n - axis_dir * along) * tyre_force, hard);
    } else if (suspension > 0) {
      body.add_force(axis_dir * -suspension, hard);
    }
    const auto pose = wheel_pose(w, l1);
    s.length = l1;
    s.speed = v1;
    s.contact = contact;
    s.force = tyre_force;
    s.normal = contact ? n : Vec3{0, 1, 0};
    s.hub = pose.hub_origin;
    s.hub_rotation = pose.hub_rotation;
    s.center = body_point(body, pose.hub_origin);
    s.patch = s.center - s.normal * w.radius;
    return;
  }
  double speed = 0;
  double free_length = w.maximum;
  if (w.unsprung > 0) {
    const double hold = suspension_force(s.length, s.speed, w.rest, w.maximum,
                                         w.spring, w.bump_damping, w.rebound_damping);
    speed = s.speed + (hold - w.unsprung * 9.81 * down.y) / w.unsprung * dt;
    free_length = s.length + speed * dt;
  }
  double seated = std::clamp(s.length, w.minimum, w.maximum);
  sample_footprint(project(seated));
  for (int i = 0; i < 8; ++i) {
    const Vec3 world = project(seated);
    int deepest = -1;
    const double error = tyre_error(world, deepest);
    if (deepest < 0)
      break;
    if (std::fabs(error) < 1e-4)
      break;
    const double probe =
        seated < w.maximum - 1e-4 ? seated + 1e-3 : seated - 1e-3;
    const double slope =
        (project(probe) - world).dot(footprint[static_cast<size_t>(deepest)].normal) /
        (probe - seated);
    if (std::fabs(slope) < 1e-5)
      break;
    const double next =
        std::clamp(seated + error / slope, w.minimum, w.maximum);
    if (next == seated)
      break;
    seated = next;
  }
  const Vec3 seated_hub = project(seated);
  int deepest = -1;
  // A wheel a few millimetres short of the ground is still on it: the tyre's
  // radial give, which this rigid contact lacks. Without it, a wheel lifted by
  // a kerb edge left the ground for a step or two every few milliseconds at
  // 1000 Hz, and its load flickered (#380).
  const bool contact = free_length >= seated - 3e-3 &&
                       tyre_error(seated_hub, deepest) >= -2e-3 && deepest >= 0;
  // The contact normal: the deepest sample's plane, unless that plane does
  // not reach under the hub (an edge, such as a kerb's): then the tyre
  // touches the edge itself, and the normal runs from the edge to the hub.
  Vec3 contact_normal{0, 1, 0};
  if (contact) {
    const auto &f = footprint[static_cast<size_t>(deepest)];
    contact_normal = f.normal;
    if (deepest != 0) {
      const Vec3 foot = seated_hub - f.normal * (f.normal.dot(seated_hub - f.ground));
      GroundHit under;
      const bool same_plane =
          sample(foot, under) &&
          std::fabs(f.normal.dot(Vec3{foot.x, under.height, foot.z} - f.ground)) < .01;
      if (!same_plane) {
        const Vec3 edge = seated_hub - f.ground;
        if (edge.length() > 1e-6) contact_normal = edge.normalized();
      }
    }
  }
  const double length =
      contact ? seated : std::clamp(free_length, w.minimum, w.maximum);
  // On the ground the wheel moves with it: the damper sees the ground-relative
  // rate (speed 0 below), but the wheel keeps the travel rate it actually had
  // this step for the next step's free-length prediction (#380). Starting
  // that prediction from rest allowed only a*dt^2 of extension per step
  // (0.24 mm at 1000 Hz against 1.7 cm at 120 Hz), so ground falling away a
  // little faster, as on a kerb, made the wheel leave and retouch it every
  // other step.
  double travel = 0;
  if (contact) {
    if (w.unsprung > 0) travel = std::clamp((length - s.length) / dt, -10.0, 10.0);
    speed = 0;
  }
  const auto pose = wheel_pose(w, length);
  const Vec3 hub_w = body_point(body, pose.hub_origin);
  const Vec3 normal = contact_normal;
  // The damper works on the suspension's own travel rate. On the ground it
  // used the hard point's velocity along the contact normal: right on flat
  // ground, but on a kerb edge the normal leans back, and the car's forward
  // speed read as compression (#380): at 8 km/h, 1.9 m/s and about 10 kN of
  // extra damping per wheel on a firm sport damper, which threw the car up.
  const double rate = contact && w.unsprung > 0 && s.contact
                          ? (length - s.length) / dt
                          : speed + body.point_velocity(hard).dot(normal);
  double force = suspension_force(length, rate, w.rest, w.maximum, w.spring,
                                  w.bump_damping, w.rebound_damping);
  // Bottomed: the ground still pushes the wheel in, and the tyre takes it
  // as a radial spring (#380). Before, the remaining penetration went to the
  // rigid deep-penetration stop, so a kerb taller than the bump travel (7 cm
  // on a lowered car) threw the car up. The rigid stop now acts only past
  // kTyreDeflection, the tyre squashed to the rim.
  if (contact && length <= w.minimum + 1e-6) {
    int deepest_at_stop = -1;
    const double squash =
        std::clamp(tyre_error(project(w.minimum), deepest_at_stop), 0.0, kTyreDeflection);
    if (squash > 0) {
      const double closing = -body.point_velocity(hard).dot(normal);
      const double tyre = std::max(0.0, kTyreRate * squash + kTyreDamping * closing);
      force = std::max(force, tyre);
    }
  }
  force = std::clamp(force, 0.0, body.mass * 9.81 * 3);
  if (length <= w.minimum && speed < 0)
    speed = 0;
  if (length >= w.maximum && speed > 0)
    speed = 0;
  if (contact) {
    if (length <= w.minimum && travel < 0) travel = 0;
    if (length >= w.maximum && travel > 0) travel = 0;
  }
  s.length = length;
  s.speed = contact ? travel : speed;
  s.contact = contact;
  s.force = s.contact ? force : 0;
  if (s.contact)
    body.add_force(normal * s.force, hard);
  s.normal = normal;
  s.hub = pose.hub_origin;
  s.hub_rotation = pose.hub_rotation;
  s.center = hub_w;
  s.patch = s.center - normal * w.radius;
}

namespace {
// Host-designed compliant support, solved implicitly and jointly. Independent
// explicit dampers can exceed the 120 Hz stability bound when several low
// points act on the same rotation, even if each individual damper is stable.
struct FixedSupport {
  Vec3 point, normal;
  double inverse = 0, softness = 0, target = 0, limit = 0, impulse = 0;
  int wheel = -1;
};
FixedSupport fixed_support(const RigidBody& body, Vec3 point, Vec3 normal,
                           double depth, bool metal, double dt, int wheel = -1,
                           double installed_damping = 0) {
  const Vec3 arm = point - body.position;
  const double inverse = 1 / body.mass +
      normal.dot(body.inverse_world_inertia(arm.cross(normal)).cross(arm));
  const double stiffness = body.mass * (metal ? 800.0 : 160.0);
  const double damping = 1.4 * std::sqrt(stiffness / inverse) +
                         std::max(0.0, installed_damping);
  const double response = damping + stiffness * dt;
  return {point, normal, inverse, 1 / (dt * response),
          stiffness * depth / response, body.mass * 9.81 * 3 * dt, 0, wheel};
}
void solve_fixed_support(const RigidBody& body, std::array<FixedSupport, 12>& contacts,
                         size_t count, double dt) {
  if (!count) return;
  auto predicted = body;
  predicted.integrate_forces(dt); // healthy springs, external forces and gravity
  for (int iteration = 0; iteration < 16; ++iteration) {
    for (size_t i = 0; i < count; ++i) {
      auto& c = contacts[i];
      const double speed = predicted.point_velocity(c.point).dot(c.normal);
      const double delta = (c.target - speed - c.softness * c.impulse) /
                           (c.inverse + c.softness);
      const double next = std::clamp(c.impulse + delta, 0.0, c.limit);
      predicted.impulse(c.normal * (next - c.impulse), c.point);
      c.impulse = next;
    }
  }
}

struct RestContact {
  Vec3 point, normal, heading;
  double limit_x = 0, limit_y = 0;
  Vec3 impulse;
};

// Project the contact impulses into their friction disks, after gravity but
// before pose integration. Unlike velocity damping this can actually stick
// without advancing the chassis downhill by a tiny amount every tick.
void solve_rest_contacts(RigidBody& body, std::array<RestContact, 12>& contacts,
                         size_t count) {
  for (int iteration = 0; iteration < 12; ++iteration) {
    for (size_t i = 0; i < count; ++i) {
      auto& c = contacts[i];
      const Vec3 seed = std::abs(c.normal.y) < .9 ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
      const Vec3 x = c.heading.length() > .5 ? c.heading.normalized() : seed.cross(c.normal).normalized();
      const Vec3 y = c.normal.cross(x);
      const Vec3 r = c.point - body.position;
      const auto inverse = [&](Vec3 a, Vec3 b) {
        return a.dot(b) / body.mass +
            a.dot(body.inverse_world_inertia(r.cross(b)).cross(r));
      };
      const double xx = inverse(x, x), yy = inverse(y, y), xy = inverse(x, y);
      const double determinant = xx * yy - xy * xy;
      if (!(determinant > 1e-18)) continue;
      const Vec3 v = body.point_velocity(c.point);
      const double vx = v.dot(x), vy = v.dot(y);
      const double old_x = c.impulse.dot(x), old_y = c.impulse.dot(y);
      const double gx = vx - xx * old_x - xy * old_y;
      const double gy = vy - xy * old_x - yy * old_y;
      double ix = 0, iy = 0;
      if (c.limit_x <= 0) {
        if (c.limit_y > 0) iy = std::clamp(-gy / yy, -c.limit_y, c.limit_y);
      } else if (c.limit_y <= 0) {
        ix = std::clamp(-gx / xx, -c.limit_x, c.limit_x);
      } else {
        ix = (xy * gy - yy * gx) / determinant;
        iy = (xy * gx - xx * gy) / determinant;
        if (std::hypot(ix / c.limit_x, iy / c.limit_y) > 1) {
          // Project in the effective-mass metric, not radially. This is the
          // minimum post-impulse kinetic energy inside the friction ellipse;
          // arbitrary anisotropic clipping can otherwise add energy.
          double a = xx * c.limit_x * c.limit_x, b = yy * c.limit_y * c.limit_y;
          double cross = xy * c.limit_x * c.limit_y;
          double fx = gx * c.limit_x, fy = gy * c.limit_y;
          const double scale = std::max({a, b, std::abs(cross), std::abs(fx), std::abs(fy)});
          a /= scale; b /= scale; cross /= scale; fx /= scale; fy /= scale;
          const auto at = [&](double multiplier) {
            const double d = (a + multiplier) * (b + multiplier) - cross * cross;
            return Vec3{(cross * fy - (b + multiplier) * fx) / d,
                        (cross * fx - (a + multiplier) * fy) / d, 0};
          };
          double lo = 0, hi = std::hypot(fx, fy);
          for (int pass = 0; pass < 24; ++pass) {
            const double middle = (lo + hi) * .5;
            if (at(middle).length() > 1) lo = middle;
            else hi = middle;
          }
          const auto q = at(hi);
          ix = q.x * c.limit_x;
          iy = q.y * c.limit_y;
        }
      }
      const auto energy = [&](double a, double b) {
        return .5 * (xx * a * a + 2 * xy * a * b + yy * b * b) + gx * a + gy * b;
      };
      if (!std::isfinite(ix) || !std::isfinite(iy) || energy(ix, iy) > energy(old_x, old_y)) {
        ix = old_x;
        iy = old_y;
      }
      const Vec3 candidate = x * ix + y * iy;
      body.impulse(candidate - c.impulse, c.point);
      c.impulse = candidate;
    }
  }
}

// Host approximation following the bounded paired chassis couple in
// SLRR_rev 77b0fb6, suspension-3d.ts (not recovered MWM physics). Reimplemented
// mathematically, with per-corner rest offsets removed to avoid a static
// preload from unequal spring rest lengths. Tyre loads have already been used.
void solve_anti_roll(RigidBody& body, const std::array<WheelConfig, 4>& wheels,
                     const std::array<WheelState, 4>& state,
                     const std::array<AntiRollConfig, 2>& config) {
  for (size_t axle = 0; axle < config.size(); ++axle) {
    const auto coefficient = [](double value) {
      return std::isfinite(value) && value > 0 ? value : 0.0;
    };
    const double stiffness = coefficient(config[axle].stiffness);
    const double damping = coefficient(config[axle].damping);
    const double scale = std::max(stiffness, damping);
    if (!(scale > 0)) continue; // Preserve the unconfigured force path exactly.
    const size_t left = axle * 2, right = left + 1;
    if (!wheels[left].enabled || !wheels[right].enabled ||
        wheels[left].rigid_contact || wheels[right].rigid_contact ||
        !state[left].contact || !state[right].contact)
      continue;
    const auto hardpoint = [&](size_t corner) {
      const auto& w = wheels[corner];
      return body_point(body, w.center - w.down.normalized() * w.anchor_length());
    };
    const Vec3 a = hardpoint(left), b = hardpoint(right);
    if (!a.finite() || !b.finite()) continue;
    const double deflection = (state[right].length - wheels[right].rest) -
                              (state[left].length - wheels[left].rest);
    const double speed = body.point_velocity(a).y - body.point_velocity(b).y;
    const double limit = body.mass * (.5 * 9.81);
    if (!std::isfinite(deflection) || !std::isfinite(speed) ||
        !std::isfinite(limit) || !(limit > 0))
      continue;
    // Scale before combining: even finite DBL_MAX coefficients must not
    // overflow two products into an indeterminate infinity-minus-infinity.
    const double response = (stiffness / scale) * deflection -
                            (damping / scale) * speed;
    if (response == 0) continue;
    const double force = std::abs(response) >= limit / scale
        ? std::copysign(limit, response) : response * scale;
    body.add_force({0, force, 0}, a);
    body.add_force({0, -force, 0}, b);
  }
}

constexpr double rpm_per_rad = 9.54929658551372;
constexpr double wheel_inertia = 1.6;

Vec3 wheel_forward(const RigidBody &body, double steer, Quaternion hub = {}) {
  return body.orientation.rotate(
      (hub * wheel_steering(steer)).rotate({0, 0, -1}));
}

double limiter_factor(double rpm, double previous, double limit) {
  const double predicted = 2 * rpm - previous;
  if (predicted <= limit)
    return rpm > limit ? (limit - predicted) / (rpm - predicted) : 1;
  return rpm > limit ? 0 : (limit - rpm) / (predicted - rpm);
}
} // namespace

void VehicleDynamics::update_gear(double rpm, double shaft,
                                  double road_shaft) {
  const bool shift_pressed = drive.shift && drive.shift != previous_shift_;
  previous_shift_ = drive.shift;
  if (!drive.engaged || drive.gears <= 0) {
    gear = 0;
    shift_time_ = 0;
    forced_reverse_ = false;
    return;
  }

  shift_time_ = std::max(0.0, shift_time_ - dt_);
  if (gear != 7 && gear > drive.gears)
    gear = drive.gears;
  if (drive.reverse && !forced_reverse_ && drive.ratios[7] < 0) {
    forced_reverse_ = true;
    gear = 7;
    shift_time_ = 0;
  } else if (!drive.reverse && forced_reverse_) {
    forced_reverse_ = false;
    gear = drive.automatic_shift || drive.automatic_first ? 1 : 0;
  } else if (shift_pressed && shift_time_ == 0 && !forced_reverse_) {
    const int next = std::clamp((gear == 7 ? -1 : gear) + drive.shift,
                                drive.ratios[7] < 0 ? -1 : 0, drive.gears);
    gear = next < 0 ? 7 : next;
    shift_time_ = .22;
  }

  if ((!drive.automatic_shift && !drive.automatic_first) || !engine_running ||
      shift_time_ > 0 || drive.clutch < .5)
    return;
  if (gear == 0 && drive.throttle > .1)
    gear = 1;
  if (!drive.automatic_shift)
    return;
  if (gear <= 0 || gear == 7 || shaft <= 2)
    return;
  const double ratio = drive.ratios[gear];
  const double coupled_rpm = std::abs(road_shaft * ratio) * rpm_per_rad;
  if (gear < drive.gears && rpm > drive.limit * .88 &&
      coupled_rpm > drive.limit * .7 && drive.ratios[gear + 1] > 0) {
    ++gear;
    shift_time_ = .12;
  } else if (gear > 1 &&
             rpm < std::max(drive.idle * 1.45, drive.limit * .42) &&
             shaft * ratio * rpm_per_rad < drive.limit * .65) {
    --gear;
    shift_time_ = .22;
  }
}

double VehicleDynamics::engine_limit_fraction() const {
  return std::clamp(limiter_factor(engine_omega * rpm_per_rad, previous_rpm_, drive.limit), 0.0, 1.0);
}

void VehicleDynamics::step_drive() {
  const double rpm = engine_omega * rpm_per_rad;
  double weights = 0, shaft = 0, road_shaft = 0, square_weights = 0;
  for (size_t i = 0; i < wheels.size(); ++i) {
    if (!wheels[i].enabled)
      continue;
    const auto &cfg = drive.wheels[i];
    const auto &s = state[i];
    const double w = cfg.weight;
    weights += w;
    shaft += w * s.spin;
    square_weights += w * w;
    if (s.contact && wheels[i].radius > 0) {
      const Vec3 forward = wheel_forward(body, cfg.steer, s.hub_rotation);
      const Vec3 tangent =
          (forward - s.normal * forward.dot(s.normal)).normalized();
      const double road_speed = body.point_velocity(s.patch).dot(tangent);
      road_shaft += w * road_speed / wheels[i].radius;
    }
  }
  if (weights > 0) {
    shaft /= weights;
    road_shaft /= weights;
  }
  update_gear(rpm, shaft, road_shaft);
  const double ratio = drive.ratios[gear];
  const double forward_speed = body.velocity.dot(wheel_forward(body, 0));
  // A gear only drives the wheels in its own direction through the clutch;
  // moving against it never applies the brakes. The old direction brake
  // locked every wheel whenever the car slid backwards in a forward gear,
  // so a spin could not be caught.
  const bool automatic_shift = drive.automatic_shift && shift_time_ > 0;
  const bool accelerator = drive.throttle > .1;
  if (!engine_running && accelerator && !accelerator_down_)
    starter_remaining_ = 2;
  accelerator_down_ = accelerator;
  const bool starting = !engine_running && drive.starter > 0 &&
                        (accelerator || starter_remaining_ > 0);
  starter_remaining_ = std::max(0.0, starter_remaining_ - dt_);
  service_brake_ = std::lerp(service_brake_, drive.brake,
                             -std::expm1(-dt_ / .08));
  const double friction = engine_friction_torque(rpm);
  const double pumping = 14 + std::max(0.0, rpm - drive.idle) * .012;
  const double idle_torque =
      rpm < drive.idle ? std::min(80.0, 25.0 + (drive.idle - rpm) * .25) : 0;
  const double combustion = std::clamp((rpm - 200) / 450, 0.0, 1.0);
  const double kph = std::abs(forward_speed) * 3.6;
  double gas = std::clamp(drive.throttle, 0.0, 1.0);
  // The driver's pedal before traction control: the auto clutch follows it
  // (#380). Following the reduced `gas` made a launch-control cut lower the
  // engagement rpm, so the clutch grabbed harder, the wheels spun more and
  // the cut deepened until the clutch locked in a few milliseconds.
  const double pedal = gas;

  // ABS/ASR sensors (#446): the wheels' circumferential speeds along the
  // direction of travel, their rates and the longitudinal accelerometer.
  // Near standstill the direction is the gear's.
  const double direction = forward_speed > .5    ? 1
                           : forward_speed < -.5 ? -1
                           : gear == 7           ? -1
                           : gear > 0            ? 1
                                                 : travel_direction_;
  std::array<double, 4> wheel_speed{};
  std::array<bool, 4> enabled{}, contact{}, driven{}, undriven{};
  for (size_t i = 0; i < wheels.size(); ++i) {
    enabled[i] = wheels[i].enabled;
    contact[i] = enabled[i] && state[i].contact;
    driven[i] = enabled[i] && drive.wheels[i].weight > 0;
    undriven[i] = enabled[i] && !driven[i];
    wheel_speed[i] = state[i].spin * wheels[i].radius * direction;
  }
  if (!sensors_seeded_ || direction != travel_direction_) {
    wheel_speed_previous_ = wheel_speed;
    wheel_acceleration_ = {};
    forward_speed_previous_ = forward_speed;
    reference_.reset(std::abs(forward_speed));
    travel_direction_ = direction;
    sensors_seeded_ = true;
  }
  for (size_t i = 0; i < wheels.size(); ++i) {
    const double rate = (wheel_speed[i] - wheel_speed_previous_[i]) / dt_;
    wheel_acceleration_[i] += (rate - wheel_acceleration_[i]) * -std::expm1(-dt_ / .004);
  }
  wheel_speed_previous_ = wheel_speed;
  const double acceleration = (forward_speed - forward_speed_previous_) / dt_ * direction;
  forward_speed_previous_ = forward_speed;
  const double reference =
      reference_.update(dt_, acceleration, service_brake_ > .02 || drive.handbrake > .05,
                        wheel_speed, contact, undriven);
  // ASR: engine torque cut on driven-wheel slip, in any gear and at any
  // speed, while the driver accelerates and does not brake.
  const bool asr_allowed = engine_running && gear != 0 && drive.clutch >= .5 &&
                           drive.brake < .05 && drive.handbrake < .2 && gas > .05 &&
                           !automatic_shift;
  gas *= asr_.update(dt_, drive.asr_strength, asr_allowed, reference, wheel_speed, driven,
                     contact);
  double combustion_torque = 0;
  if (engine_running && rpm < drive.limit && !automatic_shift) {
    const double limiter = engine_limit_fraction();
    // Dyno banks are net torque: recover indicated torque before scaling.
    double torque = (drive.torque(rpm) + friction) * gas * limiter;
    // MWM 454A15: blend the limited dry output toward the wet bank; the
    // nitrous input is independent of the accelerator (unlike visual flames).
    if (drive.nitro > 0 && drive.nitro_torque)
      torque = std::lerp(torque, drive.nitro_torque(rpm) + friction,
                         std::clamp(drive.nitro, 0.0, 1.0) * limiter);
    combustion_torque = (torque + idle_torque * limiter) * combustion;
  }
  previous_rpm_ = rpm;
  // The dyno supplies net WOT torque. Recover its indicated contribution
  // before scaling combustion, then apply the shared mechanical loss once.
  // The existing drag curve is incremental closed-throttle pumping, not a
  // second WOT friction loss. Fuel cut leaves mechanical friction active.
  const double net = combustion_torque + (starting ? 4 * drive.starter : 0) -
                     friction - pumping * (1 - gas);
  engine_omega = std::max(0.0, engine_omega + net * dt_ / drive.inertia);

  double drive_impulse = 0;
  if (weights > 0 && ratio != 0 && shift_time_ == 0 &&
      (engine_running || !drive.auto_clutch)) {
    const double transmission =
        wheel_inertia * weights * weights / square_weights;
    const double reflected = transmission / (ratio * ratio);
    const double mix = reflected / (drive.inertia + reflected);
    const double mismatch = shaft * ratio - engine_omega;
    constexpr double exchange_rate = .9990000128746033;
    const double closed_delta = mismatch * mix * exchange_rate;
    double grip = drive.clutch;
    const double stall_risk = std::clamp(
        (drive.idle * 1.5 -
         std::min(rpm, std::abs(shaft * ratio) * rpm_per_rad)) /
            std::max(1.0, drive.idle * .5),
        0.0, 1.0);
    if (drive.auto_clutch && closed_delta < 0) {
      double engagement_rpm = drive.idle * .85;
      if (drive.automatic_shift && (gear == 1 || gear == 7) &&
          drive.brake < .05 && drive.handbrake < .05 && pedal > 0) {
        engagement_rpm = std::lerp(
            engagement_rpm, std::max(engagement_rpm, drive.limit * .4), pedal);
      }
      grip = std::min(grip,
                      std::max(0.0, engine_omega - engagement_rpm / rpm_per_rad) /
                          -closed_delta);
    }
    if (drive.auto_clutch && pedal <= .05) {
      const double low_speed = std::clamp((12 - kph) / 10, 0.0, 1.0);
      grip *= 1 - stall_risk * low_speed;
    }
    engine_omega = std::max(0.0, engine_omega + closed_delta * grip);
    drive_impulse =
        -mismatch * (1 - mix) * grip * exchange_rate / ratio * transmission;
    for (size_t i = 0; i < wheels.size(); ++i)
      if (wheels[i].enabled)
        state[i].spin +=
            drive_impulse * drive.wheels[i].weight / (weights * wheel_inertia);
  } else if (automatic_shift && engine_running) {
    const double target =
        std::max(drive.idle, std::abs(road_shaft * ratio) * rpm_per_rad);
    engine_omega = std::lerp(engine_omega, target / rpm_per_rad,
                             -std::expm1(-dt_ / .07));
  }

  // Différentiel autobloquant (LSD 1.5-way avec précharge minimale)
  if (drive.differential_lock > 0 && weights > 0) {
    const double lock_ratio = std::clamp(drive.differential_lock, 0.0, 1.0);
    for (size_t left = 0; left < wheels.size(); left += 2) {
      const size_t right = left + 1;
      if (right >= wheels.size())
        break;
      const double left_weight = drive.wheels[left].weight;
      const double right_weight = drive.wheels[right].weight;
      if (!wheels[left].enabled || !wheels[right].enabled ||
          left_weight <= 0 || right_weight <= 0)
        continue;

      double transfer =
          (state[left].spin - state[right].spin) * wheel_inertia * 0.5;
      if (lock_ratio < 1.0) {
        const double axle_share = (left_weight + right_weight) / weights;
        const double preload = 35.0 * lock_ratio * dt_;
        // Plein verrouillage sous couple positif, ~40% en décélération (1.5-way)
        const double ramp = drive_impulse >= 0.0 ? 1.0 : 0.4;
        const double limit =
            preload +
            std::abs(drive_impulse) * axle_share * lock_ratio * ramp;
        transfer = std::clamp(transfer, -limit, limit);
      }
      state[left].spin -= transfer / wheel_inertia;
      state[right].spin += transfer / wheel_inertia;
    }
  }

  const double next_rpm = engine_omega * rpm_per_rad;
  if (engine_running && next_rpm < std::max(250.0, drive.idle * .35))
    engine_running = false;
  else if (!engine_running && next_rpm >= std::max(500.0, drive.idle * .65) &&
           (starting || (!drive.auto_clutch && gas > 0))) {
    engine_running = true;
    starter_remaining_ = 0;
  }

  // ABS: the hydraulic unit sets each caliper's pressure from the pedal's.
  abs_.update(dt_, service_brake_, drive.abs_strength, reference, std::max(0.0, -acceleration),
              wheel_speed, wheel_acceleration_, enabled, contact, 2);

  for (size_t i = 0; i < wheels.size(); ++i) {
    const auto &wheel = wheels[i];
    const auto &cfg = drive.wheels[i];
    auto &s = state[i];
    if (!wheel.enabled)
      continue;
    const auto brake = [](double speed, double amount) {
      return std::copysign(std::max(0.0, std::abs(speed) - amount), speed);
    };
    const Vec3 heading = wheel_forward(body, cfg.steer, s.hub_rotation);
    const Vec3 forward =
        (heading - s.normal * heading.dot(s.normal)).normalized();
    const Vec3 side = forward.cross(s.normal);
    const Vec3 speed = body.point_velocity(s.patch);
    const double road = speed.dot(forward), lateral = speed.dot(side);

    // Consume one brake-impulse budget, before traction sees the driven spin.
    // Any unused budget can hold a locked wheel against the tyre reaction
    // below; applying the full brake a second time would double its torque.
    // The friction brakes' work heats the rotor (#446); the bearing's does
    // not. `remaining_friction` is the friction brakes' part of the budget.
    double remaining_brake = 0, remaining_friction = 0, heat = 0;
    const auto apply_brake = [&](double amount, bool friction) {
      amount = std::max(0.0, amount);
      const double before = s.spin;
      s.spin = brake(s.spin, amount);
      const double unused = std::max(0.0, amount - std::abs(before - s.spin));
      remaining_brake += unused;
      if (friction) {
        remaining_friction += unused;
        heat += .5 * wheel_inertia * (before * before - s.spin * s.spin);
      }
    };
    apply_brake(500 * dt_ * cfg.bearing * drive.bearing_scale, false);
    const double torque_step = dt_ / wheel_inertia;
    // The caliper's pressure (ABS, or the ASR braking a spinning wheel) and
    // the pad friction at the rotor's temperature.
    double pressure = std::max(abs_.pressure(i), asr_.brake(i));
    // With ABS its electronic distribution replaces the valve (#446).
    if (i >= 2 && drive.abs_strength <= 0 && drive.rear_valve_knee > 0 &&
        pressure > drive.rear_valve_knee)
      pressure = drive.rear_valve_knee + (pressure - drive.rear_valve_knee) * drive.rear_valve_slope;
    s.brake_pressure = pressure;
    apply_brake(torque_step * pressure * cfg.brake * drive.brake_scale *
                    pad_friction_factor(cfg.hardware, s.brake_temperature),
                true);
    apply_brake(torque_step * drive.handbrake * cfg.handbrake * drive.handbrake_scale, true);

    if (!s.contact || s.force <= 0) {
      s.spin *= 1 - .25 * dt_ / wheel_inertia;
      s.slip_angle = 0;
    } else {
      s.slip_speed = s.spin * wheel.radius - road;
      s.lateral_speed = lateral;
      // Relaxation length, integrated implicitly: the deflection follows
      // lateral / speed with a lag of relaxation / speed (12 ms at 90 km/h
      // for the 0.3 m of a tyre at its optimal pressure). The patched Tyre
      // script lengthens it as the pressure falls.
      const double reference = std::max(1.5, std::abs(road));
      const double relaxation = std::clamp(cfg.tyre.relaxation_length, .05, 2.0);
      s.slip_angle = (relaxation * s.slip_angle + lateral * dt_) /
                     (relaxation + reference * dt_);
      const double relaxed_lateral = s.slip_angle * reference;
      // Camber against the road (#418): the hub's down axis leaning along the
      // tyre's lateral axis; positive when the tyre's top leans to +side.
      const Vec3 hub_down = body.orientation.rotate(s.hub_rotation.rotate({0, -1, 0}));
      const double camber = std::asin(std::clamp(-hub_down.dot(side), -1.0, 1.0));
      TireForces force{};
      if (wheel.metal_contact) {
        // Hub/disc contact is sliding metal, never the removed tyre's curve.
        const double slip = std::hypot(s.slip_speed, lateral);
        const double factor = slip > 1e-9 ? .4 * s.force / slip : 0;
        force = {s.slip_speed * factor, -lateral * factor};
      } else {
        force = tire_forces(s.force, 1, s.spin * wheel.radius, road, relaxed_lateral,
                            cfg.tyre, camber, lateral);
      }

      const Vec3 arm = s.patch - body.position;
      const double inverse_mass =
          1 / body.mass +
          forward.dot(
              body.inverse_world_inertia(arm.cross(forward)).cross(arm));
      const double inverse_spin = wheel.radius * wheel.radius / wheel_inertia;
      const double limit =
          std::abs(s.slip_speed) / (dt_ * (inverse_mass + inverse_spin));
      force.longitudinal = std::clamp(force.longitudinal, -limit, limit);
      s.tire_force = force;
      body.add_force(forward * force.longitudinal + side * force.lateral,
                     s.patch);
      const double rolling_torque =
          s.spin == 0 ? 0
                      : std::copysign(
                            s.force * wheel.radius * cfg.rolling_resistance,
                            s.spin);
      s.spin -=
          (force.longitudinal * wheel.radius + rolling_torque + s.spin * .15) *
          dt_ / wheel_inertia;
    }
    const double before_hold = s.spin;
    s.spin = brake(s.spin, remaining_brake);
    if (remaining_brake > 0)
      heat += .5 * wheel_inertia * (before_hold * before_hold - s.spin * s.spin) *
              remaining_friction / remaining_brake;
    s.brake_temperature =
        step_rotor_temperature(cfg.hardware, s.brake_temperature, heat, road, dt_);
    const double unspent = std::max(0.0, remaining_brake - std::abs(before_hold - s.spin));
    s.holding_impulse = unspent * wheel_inertia / std::max(.01, wheel.radius);
    s.rotation =
        std::remainder(s.rotation + s.spin * dt_, 6.283185307179586);
  }
}

void VehicleDynamics::step(const Terrain &terrain) {
  apply_aero();
  double ceiling = body.position.y;
  for (Vec3 p : body.corners())
    ceiling = std::max(ceiling, p.y);
  std::array<RestContact, 12> rest_contacts{};
  size_t rest_count = 0;
  std::array<FixedSupport, 12> fixed_contacts{};
  size_t fixed_count = 0;
  for (size_t i = 0; i < std::min(body_support_count, body_supports.size()); ++i) {
    const Vec3 point = body_point(body, body_supports[i]);
    GroundHit hit;
    if (!terrain(point.x, point.z, ceiling, hit)) continue;
    const Vec3 normal = hit.normal.normalized();
    if (normal.y <= .05) continue;
    const double depth = (hit.height - point.y) * normal.y;
    if (depth < 0) continue;
    const Vec3 surface = point + normal * depth;
    fixed_contacts[fixed_count++] = fixed_support(body, surface, normal, depth, true, dt_);
  }
  for (size_t i = 0; i < wheels.size(); ++i) {
    const auto &w = wheels[i];
    auto &s = state[i];
    s.contact = false;
    s.force = 0;
    s.slip_speed = s.lateral_speed = 0;
    s.holding_impulse = 0;
    s.tire_force = {};
    if (!s.seeded)
      s.length = w.maximum;
    if (!w.enabled)
      continue;
    if (w.rigid_contact) {
      s.length = w.minimum;
      s.speed = 0;
      s.seeded = true;
      const auto pose = wheel_pose(w, s.length);
      const Vec3 hub = body_point(body, pose.hub_origin);
      GroundHit hit;
      if (terrain(hub.x, hub.z, ceiling, hit)) {
        s.normal = hit.normal.normalized();
        s.patch = hub - s.normal * w.radius;
        const double depth = w.radius -
            (hub - Vec3{hub.x, hit.height, hub.z}).dot(s.normal);
        s.contact = depth >= 0;
        if (s.contact) s.patch += s.normal * depth;
        const double speed = body.point_velocity(s.patch).dot(s.normal);
        const double damper = speed >= 0 ?
            (w.rebound_damping > 0 ? w.rebound_damping : .7 * w.bump_damping) : w.bump_damping;
        s.hub_rotation = pose.hub_rotation;
        if (s.contact)
          fixed_contacts[fixed_count++] = fixed_support(body, s.patch, s.normal,
              depth, w.metal_contact, dt_, static_cast<int>(i), damper);
      }
      continue;
    }
    if (w.linked()) {
      solve_linked(body, w, s, terrain, ceiling, dt_, drive.wheels[i].steer);
      continue;
    }
    s.length = w.maximum;
    Vec3 down = body.orientation.rotate(w.down.normalized());
    Vec3 hard =
        body.position +
        body.orientation.rotate(
            w.center - w.down.normalized() * w.anchor_length() - body.center);
    GroundHit hit;
    if (down.y >= -.05 ||
        !terrain(hard.x, hard.z, std::max(ceiling, hard.y), hit))
      continue;
    const double length = (hard.y - hit.height - w.radius) / -down.y;
    s.length = std::clamp(length, w.minimum, w.maximum);
    s.hub_rotation = w.hub_alignment;
    s.contact = length < w.maximum;
    if (!s.contact)
      continue;
    s.normal = hit.normal.normalized();
    double force = suspension_force(
        length, body.point_velocity(hard).dot(s.normal), w.rest, w.maximum,
        w.spring, w.bump_damping, w.rebound_damping);
    force += std::max(0.0, w.minimum - length) * w.spring * 8;
    s.force = std::clamp(force, 0.0, body.mass * 9.81 * 3);
    body.add_force(s.normal * s.force, hard);

    const Vec3 point =
        body_point(body, wheel_pose(w, s.length).hub_origin) - s.normal * w.radius;
    s.patch = point;
  }

  solve_fixed_support(body, fixed_contacts, fixed_count, dt_);
  for (size_t i = 0; i < fixed_count; ++i) {
    const auto& c = fixed_contacts[i];
    const double force = c.impulse / dt_;
    body.add_force(c.normal * force, c.point);
    if (c.wheel >= 0) state[static_cast<size_t>(c.wheel)].force = force;
    else if (force > 0)
      rest_contacts[rest_count++] = {c.point, c.normal, {}, .4 * c.impulse, .4 * c.impulse, {}};
  }

  if (drive.torque && drive.inertia > 0)
    step_drive();
  if (drive.native_traffic) {
    for (size_t i = 0; i < wheels.size(); ++i) {
      auto &s = state[i];
      if (!wheels[i].enabled || !s.contact)
        continue;
      auto forward = wheel_forward(body, drive.wheels[i].steer, s.hub_rotation);
      forward = (forward - s.normal * forward.dot(s.normal)).normalized();
      auto side = forward.cross(s.normal).normalized(),
           velocity = body.point_velocity(s.patch);
      double speed = velocity.dot(forward),
             sign = speed > 0 ? 1 : speed < 0 ? -1 : 0;
      double braking =
          sign * std::min(drive.brake * 8, std::abs(speed) / dt_);
      double hold = drive.brake > .5 ? 9.81 * forward.y : 0;
      double longitudinal =
          (drive.throttle * 5 - braking - speed * .05 + hold) * body.mass / 4;
      double lateral = -velocity.dot(side) * body.mass / 4 * 8;
      auto force = forward * longitudinal + side * lateral;
      double limit = s.force * .9;
      if (force.length() > limit)
        force = force.normalized() * limit;
      body.add_force(force, s.patch);
      s.rotation += speed / wheels[i].radius * dt_;
      s.spin = speed / wheels[i].radius;
    }
  }
  // Bars also work without an engine/drive, and never feed back into the tyre
  // forces already evaluated above. Always apply both sides as one couple.
  solve_anti_roll(body, wheels, state, anti_roll);
  const bool parked = !drive.torque && !drive.native_traffic;
  for (size_t i = 0; i < wheels.size(); ++i) {
    const auto& s = state[i];
    const auto& cfg = drive.wheels[i];
    if (!wheels[i].enabled || !s.contact || !(s.force > 0)) continue;
    const bool braked = (drive.brake > .05 && cfg.brake > 0) ||
                        (drive.handbrake > .05 && cfg.handbrake > 0) ||
                        (drive.native_traffic && drive.brake > .5);
    const Vec3 v = body.point_velocity(s.patch);
    const double speed = (v - s.normal * v.dot(s.normal)).length();
    if (!parked && !(braked && speed < .05 && std::abs(s.spin * wheels[i].radius) < .05))
      continue;
    const double mu = wheels[i].metal_contact ? .4 : .8;
    TireForces caps{mu * s.force, mu * s.force};
    if (!wheels[i].metal_contact && !drive.native_traffic) {
      const double tilt = body.orientation.rotate(s.hub_rotation.rotate({0, -1, 0})).dot(s.normal);
      const double camber = std::acos(std::clamp(std::abs(tilt), 0.0, 1.0));
      const auto peaks = tire_friction_limits(s.force, 1, cfg.tyre, camber);
      caps.longitudinal = std::min(caps.longitudinal, peaks.longitudinal);
      caps.lateral = std::min(caps.lateral, peaks.lateral);
    }
    const double used = std::hypot(
        caps.longitudinal > 0 ? s.tire_force.longitudinal / caps.longitudinal : 0,
        caps.lateral > 0 ? s.tire_force.lateral / caps.lateral : 0);
    const bool no_capacity = (caps.longitudinal <= 0 && s.tire_force.longitudinal != 0) ||
                             (caps.lateral <= 0 && s.tire_force.lateral != 0);
    const double remaining = no_capacity ? 0 : std::max(0.0, 1 - used);
    double limit_x = caps.longitudinal * remaining * dt_;
    const double limit_y = caps.lateral * remaining * dt_;
    if (!parked && !drive.native_traffic) limit_x = std::min(limit_x, s.holding_impulse);
    Vec3 heading = wheel_forward(body, cfg.steer, s.hub_rotation);
    heading = (heading - s.normal * heading.dot(s.normal)).normalized();
    Vec3 surface = s.patch;
    GroundHit hit;
    if (terrain(surface.x, surface.z, ceiling, hit))
      surface += s.normal * ((hit.height - surface.y) * s.normal.y);
    rest_contacts[rest_count++] = {surface, s.normal, heading, limit_x, limit_y, {}};
  }
  body.integrate_forces(dt_);
  solve_rest_contacts(body, rest_contacts, rest_count);
  body.integrate_pose(dt_);
  for (int iteration = 0; iteration < 4; ++iteration) {
    double depth = 0;
    Vec3 correction;
    auto solve = [&](Vec3 point, double restitution, bool chassis = false,
                     bool tyre = false, double allowance = 0) {
      GroundHit ground;
      if (!terrain(point.x, point.z, ceiling, ground))
        return false;
      double overlap = ground.height - point.y - allowance;
      if (overlap <= 0)
        return false;
      if (chassis && iteration == 0) {
        const double speed = -body.point_velocity(point).dot(ground.normal);
        if (speed > ground_impact.speed)
          ground_impact = {point, ground.normal, speed};
      }
      const double friction = tyre && drive.torque && drive.inertia > 0 ? 0 : .6;
      body.contact(point, ground.normal, restitution, friction);
      if (overlap > depth) {
        depth = overlap;
        correction = ground.normal.normalized();
      }
      return true;
    };
    // An AABB corner is not necessarily material on the underside of a car.
    // Prefer installed-mesh low points; keep the old box fallback for callers
    // without them, and for the roof/side after a rollover.
    if (!body_support_count || body.orientation.rotate({0, 1, 0}).y < .5)
      for (Vec3 point : body.corners()) solve(point, .12, true);
    for (size_t i = 0; i < std::min(body_support_count, body_supports.size()); ++i)
      solve(body_point(body, body_supports[i]), 0, true, false, .03);
    for (size_t i = 0; i < wheels.size(); ++i) {
      const auto &wheel = wheels[i];
      if (!wheel.enabled)
        continue;
      const bool linked = wheel.linked() && !wheel.rigid_contact;
      if (linked && state[i].length > wheel.minimum + .002)
        continue;
      const auto down = wheel.down.normalized();
      if (!wheel.rigid_contact && body.orientation.rotate(down).y >= -.05)
        continue;
      const auto local = wheel_pose(wheel, wheel.minimum).hub_origin;
      const auto point = body.position +
                         body.orientation.rotate(local - body.center) -
                         Vec3{0, wheel.radius, 0};
      // Fixed-travel corners carry a same-step compliant load above. This
      // is only a deep-penetration safety stop, not the normal support path.
      // A linked wheel at its bump stop squashes its tyre first (solve_linked).
      if (solve(point, 0, false, true, wheel.rigid_contact ? .03 : linked ? kTyreDeflection : 0) &&
          wheel.rigid_contact)
        state[i].contact = true;
    }
    if (depth > .001)
      body.position += correction * ((depth - .001) * .6);
    else
      break;
  }
  sync_wheels();
}

void VehicleDynamics::sync_wheels() {
  for (size_t i = 0; i < wheels.size(); ++i) {
    const auto &w = wheels[i];
    auto &s = state[i];
    const auto pose = wheel_pose(w, s.length);
    s.hub = pose.hub_origin;
    s.hub_rotation = pose.hub_rotation;
    s.center = body_point(body, s.hub);
  }
}

void VehicleDynamics::set_frequency(unsigned hz) {
  if (hz < 60 || hz > 4000)
    throw std::invalid_argument("solver rate must be 60-4000 Hz");
  frequency = hz;
  timestep = 1.0 / hz;
}

unsigned VehicleDynamics::steps_in_host_tick(uint64_t tick) {
  return static_cast<unsigned>((tick + 1) * frequency / update_frequency -
                               tick * frequency / update_frequency);
}

unsigned VehicleDynamics::advance(double seconds, const Terrain &terrain, bool merge) {
  if (!std::isfinite(seconds) || seconds < 0)
    throw std::invalid_argument("invalid elapsed physics time");
  ground_impact = {};
  accumulator_ += seconds;
  dt_ = timestep;
  if (merge) {
    // One step over every owed solver step (#380 multi-rate traffic).
    const unsigned owed = static_cast<unsigned>(
        std::min(64.0, std::floor((accumulator_ + timestep * 1e-10) / timestep)));
    if (!owed) return 0;
    dt_ = owed * timestep;
    step(terrain);
    dt_ = timestep;
    accumulator_ = std::max(0.0, accumulator_ - owed * timestep);
    ticks += owed;
    return owed;
  }
  unsigned count = 0;
  const unsigned max_steps =
      (frequency + update_frequency - 1) / update_frequency;
  while (accumulator_ + timestep * 1e-10 >= timestep && count < max_steps) {
    step(terrain);
    accumulator_ = std::max(0.0, accumulator_ - timestep);
    ++ticks;
    ++count;
  }
  return count;
}

void VehicleDynamics::reset_clock() {
  accumulator_ = 0;
  ticks = 0;
}

void VehicleDynamics::reset_drive() {
  engine_omega = previous_rpm_ = shift_time_ = 0;
  starter_remaining_ = service_brake_ = 0;
  gear = previous_shift_ = 0;
  abs_.reset();
  asr_.reset();
  sensors_seeded_ = false;
  accelerator_down_ = engine_running = forced_reverse_ = false;
}

void VehicleDynamics::release_wheels() {
  for (auto &wheel : state)
    wheel = {};
}

uint64_t VehicleDynamics::backlog() const {
  return static_cast<uint64_t>((accumulator_ + timestep * 1e-10) / timestep);
}

} // namespace slrr::physics