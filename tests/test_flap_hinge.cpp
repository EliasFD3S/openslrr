#include "standalone_test.hpp"

#include "slrr/flap_hinge.hpp"

#include <cmath>
#include <initializer_list>

namespace {

using slrr::physics::FlapGeometry;
using slrr::physics::FlapLoads;
using slrr::physics::FlapState;
using slrr::physics::FlapTuning;
using slrr::physics::flap_carrier_impulse;
using slrr::physics::flap_geometry_from_cfg;
using slrr::physics::flap_hit;
using slrr::physics::flap_matrix;
using slrr::physics::flap_size_from_points;
using slrr::physics::flap_step;
using slrr::physics::flap_toggle;

constexpr double kStep = 1.0 / 240;

bool near(double a, double b, double tolerance = 1e-4) {
  return std::fabs(a - b) < tolerance;
}

// Stock lines (pivot, axis, dir, lo, hi) from MWM CFGs.
FlapGeometry cfg(float px, float py, float pz, float ax, float ay, float az,
                 float dx, float dy, float dz, float lo, float hi) {
  const float pivot[3] = {px, py, pz}, axis[3] = {ax, ay, az}, dir[3] = {dx, dy, dz};
  FlapGeometry g;
  SLRR_REQUIRE(flap_geometry_from_cfg(pivot, axis, dir, lo, hi, g));
  return g;
}

// Plate from the hinge along the arm, `width` along the axis, 3 cm thick.
FlapGeometry sized(FlapGeometry g, double length, double width, double mass) {
  double corners[8][3];
  int n = 0;
  for (double a : {0., length})
    for (double w : {-width / 2, width / 2})
      for (double t : {-.015, .015}) {
        double normal[3] = {g.axis[1] * g.arm[2] - g.axis[2] * g.arm[1],
                            g.axis[2] * g.arm[0] - g.axis[0] * g.arm[2],
                            g.axis[0] * g.arm[1] - g.axis[1] * g.arm[0]};
        for (int k = 0; k < 3; ++k)
          corners[n][k] = g.pivot[k] + g.arm[k] * a + g.axis[k] * w + normal[k] * t;
        ++n;
      }
  flap_size_from_points(g, corners, 8, mass);
  return g;
}

FlapGeometry baiern_left_door() {
  return sized(cfg(-.865f, -.105f, -.818f, 0, -1, 0, 0, 0, 1, 0, 1.134f), 1.1, .9, 22);
}
FlapGeometry baiern_hood() {
  return sized(cfg(0, .21f, -.807f, 1, 0, 0, 0, 0, -1, 0, .873f), 1.2, 1.3, 14);
}

// Free edge of the panel at `angle`, in the parent frame.
void edge_at(const FlapGeometry& g, double angle, double out[3]) {
  float m[16];
  flap_matrix(g, angle, m);
  const double p[3] = {g.pivot[0] + g.arm[0] * g.length, g.pivot[1] + g.arm[1] * g.length,
                       g.pivot[2] + g.arm[2] * g.length};
  for (int r = 0; r < 3; ++r) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
}

void run(FlapState& s, const FlapGeometry& g, const FlapLoads& loads, double seconds) {
  for (int i = 0; i < static_cast<int>(seconds / kStep); ++i) flap_step(s, g, loads, kStep);
}

}  // namespace

SLRR_TEST(stock_hinge_lines_open_outward_and_upward) {
  // Baiern CoupeSport: FL door, hood, trunk.
  const auto door = cfg(-.865f, -.105f, -.818f, 0, -1, 0, 0, 0, 1, 0, 1.134f);
  SLRR_REQUIRE(near(door.upper, 1.134) && near(door.lower, 0));
  double edge[3];
  edge_at(door, door.upper, edge);
  SLRR_REQUIRE(edge[0] < -.865 - .3);  // swings out of the left side
  float m[16];
  flap_matrix(door, .7, m);
  // The pivot does not move.
  SLRR_REQUIRE(near(m[0] * -.865 + m[4] * -.105 + m[8] * -.818 + m[12], -.865));
  SLRR_REQUIRE(near(m[2] * -.865 + m[6] * -.105 + m[10] * -.818 + m[14], -.818));

  const auto hood = cfg(0, .21f, -.807f, 1, 0, 0, 0, 0, -1, 0, .873f);
  edge_at(hood, hood.upper, edge);
  SLRR_REQUIRE(edge[1] > .21 + .3);  // front edge rises

  const auto trunk = cfg(0, .267f, 1.589f, -1, 0, 0, 0, 0, 1, 0, 1.396f);
  edge_at(trunk, trunk.upper, edge);
  SLRR_REQUIRE(edge[1] > .267 + .3);

  // Erbilac trunk opens towards negative angles: `-0.960 0`. The old pose
  // only used hi, so it never opened.
  const auto erbilac = cfg(0, .229f, 1.698f, 1, 0, 0, 0, 0, 1, -.96f, 0);
  SLRR_REQUIRE(near(erbilac.upper, .96) && near(erbilac.lower, 0));
  SLRR_REQUIRE(near(erbilac.axis[0], -1));
  edge_at(erbilac, erbilac.upper, edge);
  SLRR_REQUIRE(edge[1] > .229 + .3);

  // CivilVan rear barn door `-2.618 0` swings out behind the van.
  const auto barn = cfg(-.743f, .075f, 2.165f, 0, 1, 0, 1, 0, 0, -2.618f, 0);
  edge_at(barn, 1.5, edge);
  SLRR_REQUIRE(edge[2] > 2.165 + .3);
}

SLRR_TEST(degenerate_lines_are_fixed_mounts) {
  FlapGeometry g;
  const float pivot[3] = {}, none[3] = {}, dir[3] = {0, 0, 1}, axis[3] = {0, 1, 0};
  SLRR_REQUIRE(!flap_geometry_from_cfg(pivot, none, dir, 0, 1, g));
  SLRR_REQUIRE(!flap_geometry_from_cfg(pivot, axis, dir, 0, 0, g));
  SLRR_REQUIRE(!flap_geometry_from_cfg(pivot, axis, dir, 0, NAN, g));
  // The dangling dice line has a 1 mm axis and `1.000 2.400`; zero stays the
  // closed pose inside the travel.
  const auto dice = cfg(0, .53f, -.11f, 0, 0, -.001f, 0, -.001f, 0, 1, 2.4f);
  SLRR_REQUIRE(near(dice.lower, 0) && near(dice.upper, 2.4));
  SLRR_REQUIRE(near(dice.axis[2], -1));
}

SLRR_TEST(script_toggle_opens_holds_closes_and_latches) {
  const auto door = baiern_left_door();
  FlapState s;
  FlapLoads loads;
  SLRR_REQUIRE(!s.open && s.latched);
  SLRR_REQUIRE(flap_toggle(s));
  SLRR_REQUIRE(s.open && !s.latched);
  run(s, door, loads, .6);
  SLRR_REQUIRE(near(s.angle, door.upper, 1e-3));
  run(s, door, loads, 2);
  SLRR_REQUIRE(near(s.angle, door.upper, 1e-3));
  SLRR_REQUIRE(!flap_toggle(s));
  run(s, door, loads, .6);
  SLRR_REQUIRE(s.latched && s.angle == 0 && !s.open);

  // A hood is held open against gravity once the script opened it.
  const auto hood = baiern_hood();
  FlapState h;
  flap_toggle(h);
  run(h, hood, loads, 3);
  SLRR_REQUIRE(near(h.angle, hood.upper, 1e-3));
  // Toggling back mid-way reverses without a jump.
  flap_toggle(h);
  flap_step(h, hood, loads, kStep);
  SLRR_REQUIRE(h.angle > hood.upper - .05);
  run(h, hood, loads, 1);
  SLRR_REQUIRE(h.latched);
}

SLRR_TEST(latched_panels_ignore_weak_loads_and_a_side_hit_swings_a_door_open) {
  const auto door = baiern_left_door();
  FlapState s;
  FlapLoads loads;
  const double dv[3] = {5, 0, 3}, dw[3] = {0, 2, 0};
  flap_carrier_impulse(s, door, dv, dw);
  SLRR_REQUIRE(s.latched && s.rate == 0);
  // A pole on the left pushes the car to +X. Mid-door, 2 m/s: no release.
  const double point[3] = {-.9, -.1, -.818 + .55}, normal[3] = {1, 0, 0};
  SLRR_REQUIRE(!flap_hit(s, door, point, normal, 2));
  SLRR_REQUIRE(s.latched);
  // 8 m/s releases the latch; the inward push comes back off the closed stop.
  SLRR_REQUIRE(flap_hit(s, door, point, normal, 8));
  SLRR_REQUIRE(!s.latched && s.open && !s.hold);
  SLRR_REQUIRE(s.rate < 0);
  double peak = 0;
  for (int i = 0; i < 240; ++i) {
    flap_step(s, door, loads, kStep);
    peak = std::max(peak, s.angle);
  }
  SLRR_REQUIRE(peak > .5);
  SLRR_REQUIRE(!s.latched);
}

SLRR_TEST(free_door_follows_the_carrier_and_relatches_when_slammed) {
  const auto door = baiern_left_door();
  FlapState s;
  FlapLoads loads;
  flap_toggle(s);
  run(s, door, loads, .6);
  // Released strut: a swung-open door.
  s.hold = false;
  s.since_release = 1;
  // Braking (moving to -Z, slowing): the door keeps going forward and opens.
  s.angle = .5;
  s.rate = 0;
  const double brake[3] = {0, 0, .1}, none[3] = {};
  flap_carrier_impulse(s, door, brake, none);
  SLRR_REQUIRE(s.rate > 0);
  // Accelerating at 4 m/s^2 swings it shut, and it latches.
  s.rate = 0;
  for (int i = 0; i < 480 && !s.latched; ++i) {
    const double dv[3] = {0, 0, -4 * kStep};
    flap_carrier_impulse(s, door, dv, none);
    flap_step(s, door, loads, kStep);
  }
  SLRR_REQUIRE(s.latched && !s.open && s.angle == 0);
  // Yawing the car about the hinge swings a free door the other way.
  FlapState t;
  t.latched = false;
  t.open = true;
  t.angle = .5;
  const double yaw[3] = {0, 1, 0};
  flap_carrier_impulse(t, door, none, yaw);
  SLRR_REQUIRE(t.rate > 0);  // door axis is -Y: the car turning +Y leaves it behind
}

SLRR_TEST(a_door_left_open_gives_way_when_the_car_pulls_away) {
  const auto door = baiern_left_door();
  FlapState s;
  FlapLoads loads;
  flap_toggle(s);
  run(s, door, loads, 1);
  SLRR_REQUIRE(s.hold && !s.driving);
  // 0.1 g does not move it off its check.
  const double none[3] = {};
  for (int i = 0; i < 240; ++i) {
    const double dv[3] = {0, 0, -1 * kStep};
    flap_carrier_impulse(s, door, dv, none);
    flap_step(s, door, loads, kStep);
  }
  SLRR_REQUIRE(s.hold && s.angle > door.upper - .1);
  // 0.4 g forward does: it swings shut and latches.
  for (int i = 0; i < 480 && !s.latched; ++i) {
    const double dv[3] = {0, 0, -4 * kStep};
    flap_carrier_impulse(s, door, dv, none);
    flap_step(s, door, loads, kStep);
  }
  SLRR_REQUIRE(s.latched && !s.open);
}

SLRR_TEST(gravity_and_wind_act_on_free_panels) {
  const auto hood = baiern_hood();
  FlapLoads still;
  FlapState h;
  h.latched = false;
  h.open = true;
  h.angle = .4;
  h.since_release = 1;
  run(h, hood, still, 2);
  // A free hood falls shut and latches on the slam.
  SLRR_REQUIRE(h.latched);

  // At 30 m/s a popped hood is blown open; a popped door is blown shut.
  FlapLoads wind;
  wind.air[2] = 30;  // car moving to -Z
  FlapState popped;
  popped.latched = false;
  popped.open = true;
  popped.angle = .3;
  popped.since_release = 1;
  run(popped, hood, wind, 1);
  SLRR_REQUIRE(near(popped.angle, hood.upper, .05));
  const auto door = baiern_left_door();
  FlapState ajar = popped;
  ajar.angle = .5;
  ajar.rate = 0;
  run(ajar, door, wind, 1);
  SLRR_REQUIRE(ajar.latched);
}

SLRR_TEST(end_stops_bound_motion_without_nan) {
  const auto door = baiern_left_door();
  FlapState s;
  s.latched = false;
  s.open = true;
  s.rate = 50;  // clamped
  FlapLoads loads;
  for (int i = 0; i < 2400; ++i) {
    flap_step(s, door, loads, kStep);
    SLRR_REQUIRE(std::isfinite(s.angle) && std::isfinite(s.rate));
    SLRR_REQUIRE(s.angle <= door.upper + 1e-9 && s.angle >= door.lower - 1e-9);
  }
  SLRR_REQUIRE(std::fabs(s.rate) <= FlapTuning::max_rate);
  // Non-finite input is ignored.
  const double bad[3] = {NAN, 0, 0}, normal[3] = {1, 0, 0};
  SLRR_REQUIRE(!flap_hit(s, door, bad, normal, 10));
  flap_carrier_impulse(s, door, bad, bad);
  SLRR_REQUIRE(std::isfinite(s.rate));
  SLRR_REQUIRE(!flap_step(s, door, loads, NAN));
}

int main() {
  const slrr::standalone::Test tests[] = {
      stock_hinge_lines_open_outward_and_upward,
      degenerate_lines_are_fixed_mounts,
      script_toggle_opens_holds_closes_and_latches,
      latched_panels_ignore_weak_loads_and_a_side_hit_swings_a_door_open,
      free_door_follows_the_carrier_and_relatches_when_slammed,
      a_door_left_open_gives_way_when_the_car_pulls_away,
      gravity_and_wind_act_on_free_panels,
      end_stops_bound_motion_without_nan,
  };
  return slrr::standalone::run("flap_hinge", tests);
}
