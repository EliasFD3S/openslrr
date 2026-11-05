#include "slrr/suspension_geometry.hpp"
#include "slrr/vehicle_dynamics.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>
using namespace slrr::physics;
namespace {
int failures = 0;
void check(bool ok, const char *name) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok)
    ++failures;
}
Matrix identity(double height = 0) {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, float(height), 0, 1};
}
VehicleDynamics car(double height = 2) {
  VehicleDynamics v;
  v.body.configure(1200, {-.8, -.3, -1.7}, {.8, .7, 1.7}, {0, 0, 0});
  v.body.set_frame(identity(height));
  for (size_t i = 0; i < 4; ++i) {
    auto &w = v.wheels[i];
    w.enabled = true;
    w.center = {i % 2 ? .7 : -.7, -.5, i < 2 ? -1.2 : 1.2};
    w.radius = .32;
    w.rest = .35;
    w.minimum = .15;
    w.maximum = .5;
    w.spring = 35000;
    w.bump_damping = 2500;
    w.rebound_damping = 1750;
  }
  return v;
}
Terrain flat(double y = 0) {
  return [=](double, double, double ceiling, GroundHit &hit) {
    if (y > ceiling + .05)
      return false;
    hit.height = y;
    hit.normal = {0, 1, 0};
    return true;
  };
}
void run(VehicleDynamics &v, double seconds, double hz,
         const Terrain &terrain) {
  for (int i = 0; i < int(seconds * hz + .5); ++i)
    v.advance(1 / hz, terrain);
  while (v.backlog())
    v.advance(0, terrain);
}
} // namespace
int main() {
  CornerGeometry hinge;
  hinge.arm = {1, 1, 0, 0, 0, 0, 1};
  hinge.hub[0] = -1;
  CornerFrames h;
  check(solve_corner_geometry(hinge, {0, .6, .2}, h) &&
            std::fabs((h.arm_origin - Vec3{1, 0, 0}).x *
                          (h.arm_origin - Vec3{1, 0, 0}).x +
                      h.arm_origin.y * h.arm_origin.y - 1) < 1e-7 &&
            std::fabs(h.arm_origin.z - .2) < 1e-8,
        "native arm projection preserves hinge-axis displacement and the "
        "configured circle radius");
  CornerGeometry strut;
  strut.reference = {-.683, -.635, -1.333};
  strut.arm = {.296f, .294f, .030f, 0, 0, 0, 1};
  strut.hub = {0, -.077f, -.043f, -.004f, .071f, .407f, 0, .071f, .407f, 0};
  CornerFrames left, right;
  const bool fitted = solve_corner_geometry(strut, {-.683, -.46, -1.333}, left);
  const Vec3 lower{strut.hub[1], strut.hub[2], strut.hub[3]};
  const Vec3 upper =
      strut.reference + Vec3{strut.hub[7], strut.hub[8], strut.hub[9]} - lower;
  const Vec3 line = left.hub_rotation.rotate(
      Vec3{strut.hub[4], strut.hub[5], strut.hub[6]}.normalized());
  check(
      fitted &&
          (left.hub_origin + left.hub_rotation.rotate(lower) - left.arm_origin)
                  .length() < 1e-7 &&
          (upper - left.hub_origin).cross(line).length() < 1e-7,
      "native strut hub closes its lower attachment and passes through the "
      "upper strut line");
  auto mirror = strut;
  mirror.reference.x = -mirror.reference.x;
  mirror.arm[1] = -mirror.arm[1];
  for (int i : {1, 4, 7})
    mirror.hub[i] = -mirror.hub[i];
  check(solve_corner_geometry(mirror, {.683, -.46, -1.333}, right) &&
            std::fabs(left.hub_origin.x + right.hub_origin.x) < 1e-8 &&
            std::fabs(left.hub_origin.y - right.hub_origin.y) < 1e-8,
        "native strut geometry mirrors left/right without corner-specific "
        "placement offsets");
  CornerFrames compressed;
  check(solve_corner_geometry(strut, {-.683, -.4, -1.333}, compressed) &&
            std::fabs(compressed.hub_origin.x - left.hub_origin.x) > .001,
        "suspension travel changes the solved lateral position rather than "
        "applying a fixed track correction");
  auto unsupported = strut;
  unsupported.hub[0] = 1;
  check(!solve_corner_geometry(unsupported, {-.683, -.46, -1.333}, compressed),
        "unported positive-length hub constraint is explicitly rejected");
  RigidBody b;
  b.configure(1000, {-1, -.5, -2}, {1, .5, 2}, {.2, .1, -.3});
  auto frame = identity(5);
  frame[0] = 0;
  frame[2] = -1;
  frame[8] = 1;
  frame[10] = 0;
  b.set_frame(frame);
  auto roundtrip = b.frame();
  bool same = true;
  for (int i = 0; i < 16; ++i)
    same = same && std::fabs(frame[i] - roundtrip[i]) < 1e-6;
  check(same, "COM frame roundtrip preserves yaw and script origin");
  const double before = b.position.y;
  b.integrate(.1);
  check(std::fabs(b.velocity.y + .981) < 1e-10 &&
            std::fabs(b.position.y - (before - .0981)) < 1e-10,
        "gravity integrates velocity and position, not a ground snap");
  b.impulse({0, 100, 0}, b.position + Vec3{1, 0, 1});
  check(std::fabs(b.omega().x) > 0 && std::fabs(b.omega().z) > 0,
        "off-centre impulse creates angular motion on both axes");
  check(std::fabs(suspension_force(.5, -.1, .9, .9, 1000, 200, 140) - 440) <
            1e-8,
        "SLRR_rev asymmetric bump damping golden value");
  check(std::fabs(suspension_force(.5, .1, .9, .9, 1000, 200, 140) - 372) <
            1e-8,
        "SLRR_rev rebound damping golden value");
  check(suspension_force(1, 0, .9, .9, 1000, 200, 140) == 0,
        "extended spring cannot pull a wheel toward the floor");
  auto v = car();
  run(v, .2, 60, flat());
  check(v.body.velocity.y < -1 && v.body.position.y < 1.9 &&
            !v.state[0].contact,
        "raised vehicle is airborne and falls under gravity");
  run(v, 7, 60, flat());
  double support = 0;
  int contacts = 0;
  for (auto s : v.state) {
    support += s.force;
    contacts += s.contact;
  }
  std::printf("REST y=%.6f vy=%.6f support=%.3f contacts=%d length=%.6f\n",
              v.body.position.y, v.body.velocity.y, support, contacts,
              v.state[0].length);
  const double expected = .5 + .32 - (1200 * 9.81 / 4) / 35000;
  check(contacts == 4 && std::fabs(v.body.position.y - expected) < .01 &&
            std::fabs(v.body.velocity.y) < .01,
        "four spring contacts settle at load-dependent height");
  check(std::fabs(support - 1200 * 9.81) < 120,
        "summed spring forces balance physical body weight");
  auto buried = car(0);
  run(buried, 4, 60, flat());
  check(std::fabs(buried.body.position.y - expected) < .01 &&
            std::fabs(buried.body.velocity.y) < .01,
        "compressed-wheel hard stops recover an overlapping spawn without "
        "launch energy");
  auto low = car(-5);
  run(low, 8, 60, flat(-7));
  check(std::fabs(low.body.position.y - (expected - 7)) < .01,
        "negative terrain height does not introduce a hidden zero plane");
  auto a = car(), c = car();
  run(a, 4, 30, flat());
  run(c, 4, 144, flat());
  check((a.body.position - c.body.position).length() < 1e-10 &&
            a.ticks == c.ticks,
        "same physics trajectory at 30 and 144 render FPS");
  auto empty = car();
  Terrain air = [](double, double, double, GroundHit &) { return false; };
  run(empty, 1, 60, air);
  check(empty.body.position.y < -2 && empty.body.velocity.y < -9 &&
            !empty.state[0].contact,
        "no terrain means free fall, not fabricated support");
  auto anchored = car(), tuned = car();
  for (size_t i = 0; i < 4; ++i) {
    anchored.wheels[i].anchor = tuned.wheels[i].anchor = .4;
    tuned.wheels[i].rest = anchored.wheels[i].rest - .05;
  }
  run(anchored, 6, 60, flat());
  run(tuned, 6, 60, flat());
  check(
      std::fabs((anchored.body.position.y - tuned.body.position.y) - .05) <
          .003,
      "shorter tuned springs lower the body without moving the CFG hardpoints");
  auto debt = car();
  auto count = debt.advance(.1, flat());
  check(count == 8 && debt.backlog() == 16,
        "fixed clock caps work and retains elapsed time debt");
  auto slope = car();
  Terrain ramp = [](double x, double, double ceiling, GroundHit &hit) {
    hit.height = .1 * x;
    hit.normal = Vec3{-.1, 1, 0}.normalized();
    return hit.height <= ceiling + .05;
  };
  run(slope, 6, 60, ramp);
  check(slope.body.position.finite() &&
            std::fabs(slope.body.orientation.z) > .02,
        "unequal supports tilt the actual rigid chassis");
  bool rejected = false;
  try {
    v.advance(-1, flat());
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  check(rejected, "invalid elapsed time is rejected");
  auto armed = car();
  for (size_t i = 0; i < 4; ++i) {
    auto &w = armed.wheels[i];
    const double sign = w.center.x > 0 ? 1 : -1;
    w.arm_length = .25;
    w.arm_axis = {0, 0, 1};
    w.arm_pivot = w.center + Vec3{sign * .15, .05, 0};
  }
  run(armed, 6, 60, flat());
  bool shifted = true;
  for (size_t i = 0; i < 4; ++i) {
    const auto &w = armed.wheels[i];
    const Vec3 ray =
        w.center + w.down.normalized() *
                       (armed.state[i].length - w.anchor_length());
    if (std::fabs(armed.state[i].hub.x - ray.x) < .01)
      shifted = false;
  }
  check(shifted && std::fabs(armed.body.orientation.z) < .02 &&
            std::fabs(armed.body.position.y - expected) < .02 &&
            std::fabs(armed.body.velocity.y) < .02,
        "arm hinges move the support patch without rolling a symmetric car");
  auto heavy = car();
  for (auto &w : heavy.wheels)
    w.unsprung = 20;
  run(heavy, 7, 60, flat());
  support = 0;
  contacts = 0;
  for (auto s : heavy.state) {
    support += s.force;
    contacts += s.contact;
  }
  check(contacts == 4 && std::fabs(heavy.body.position.y - expected) < .02 &&
            std::fabs(heavy.body.velocity.y) < .05 &&
            std::fabs(support - 1200 * 9.81) < 250,
        "unsprung mass stays on the tyre and does not change static ride height");
  auto sunk = car(0);
  for (auto &w : sunk.wheels)
    w.unsprung = 20;
  run(sunk, 6, 60, flat());
  check(std::fabs(sunk.body.position.y - expected) < .03 &&
            std::fabs(sunk.body.velocity.y) < .05,
        "unsprung wheels recover a buried spawn without launching");
  auto loose = car();
  for (auto &w : loose.wheels)
    w.unsprung = 20;
  run(loose, 1, 60, air);
  check(loose.body.position.y < -2 && !loose.state[0].contact,
        "unsprung wheels do not invent a floor");
  auto slow = car(), fast = car();
  for (auto *sample : {&slow, &fast})
    for (auto &w : sample->wheels)
      w.unsprung = 20;
  run(slow, 4, 30, flat());
  run(fast, 4, 144, flat());
  check((slow.body.position - fast.body.position).length() < 1e-8 &&
            slow.ticks == fast.ticks,
        "unsprung trajectory matches at 30 and 144 render FPS");
  auto both = car();
  for (size_t i = 0; i < 4; ++i) {
    auto &w = both.wheels[i];
    w.unsprung = 10;
    const double sign = w.center.x > 0 ? 1 : -1;
    w.arm_length = .25;
    w.arm_axis = {0, 0, 1};
    w.arm_pivot = w.center + Vec3{sign * .15, .05, 0};
  }
  run(both, 7, 60, flat());
  support = 0;
  contacts = 0;
  for (auto s : both.state) {
    support += s.force;
    contacts += s.contact;
  }
  std::printf("HINGE y=%.6f vy=%.6f support=%.3f contacts=%d roll=%.6f\n",
              both.body.position.y, both.body.velocity.y, support, contacts,
              both.body.orientation.z);
  check(contacts == 4 && both.body.position.finite() &&
            std::fabs(both.body.velocity.y) < .05 &&
            std::fabs(both.body.orientation.z) < .02 &&
            std::fabs(support - 1200 * 9.81) < 400,
        "armed unsprung corners stay upright and carry only the chassis weight");
  return failures ? 1 : 0;
}
