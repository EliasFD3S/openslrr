// Settling check for the vehicle rigid-body integrator. Built twice by
// CMake (native and Project Chrono backends); both must bring a sprung box
// to rest on flat ground with the wheel loads carrying its weight.
#include "slrr/vehicle_dynamics.hpp"

#include <cmath>
#include <cstdio>

using namespace slrr::physics;

bool free_fall() {
  VehicleDynamics vd;
  vd.body.configure(1200, {-0.9, -0.3, -2.1}, {0.9, 0.5, 2.1}, {0, 0, 0});
  Matrix frame{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 10, 0, 1};
  vd.body.set_frame(frame);
  const Terrain no_ground = [](double, double, double, GroundHit &) {
    return false;
  };
  const double tick = 1.0 / VehicleDynamics::update_frequency;
  for (int frame_index = 0; frame_index < 60; ++frame_index)
    vd.advance(tick, no_ground);
  const double height_error = std::fabs(vd.body.position.y - (10 - .5 * 9.81));
  const double speed_error = std::fabs(vd.body.velocity.y + 9.81);
  std::printf("free-fall height=%.5f vertical-speed=%.5f\n",
              vd.body.position.y, vd.body.velocity.y);
  return height_error < .02 && speed_error < 1e-6 &&
         std::fabs(vd.body.position.x) < 1e-9 &&
         std::fabs(vd.body.position.z) < 1e-9;
}

int main() {
  VehicleDynamics vd;
  const double mass = 1200;
  vd.body.configure(mass, {-0.9, -0.3, -2.1}, {0.9, 0.5, 2.1}, {0, 0, 0});
  Matrix frame{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0.8f, 0, 1};
  vd.body.set_frame(frame);
  for (int i = 0; i < 4; ++i) {
    auto &w = vd.wheels[i];
    w.enabled = true;
    w.center = {i % 2 ? 0.75 : -0.75, -0.1, i < 2 ? 1.3 : -1.3};
    w.radius = 0.3;
    w.rest = 0.25;
    w.minimum = 0.05;
    w.maximum = 0.35;
    w.spring = 40000;
    w.bump_damping = 3000;
    w.rebound_damping = 4000;
  }
  const Terrain flat = [](double, double, double, GroundHit &hit) {
    hit.height = 0;
    hit.normal = {0, 1, 0};
    return true;
  };
  const double tick = 1.0 / VehicleDynamics::update_frequency;
  for (int frame_index = 0; frame_index < 5 * 60; ++frame_index)
    vd.advance(tick, flat);
  double force = 0;
  bool contact = true;
  for (const auto &s : vd.state) {
    force += s.force;
    contact = contact && s.contact;
  }
  const double weight = mass * 9.81;
  const double speed = vd.body.velocity.length(), spin = vd.body.omega().length();
  std::printf("backend=%s ticks=%llu height=%.5f speed=%.6f spin=%.6f load=%.1f/%.1f\n",
              backend_name(), static_cast<unsigned long long>(vd.ticks),
              vd.body.position.y, speed, spin, force, weight);
  const bool settled = contact && speed < .02 && spin < .02 &&
                       std::fabs(force - weight) < weight * .02;
  const bool fallen = free_fall();
  std::puts(settled && fallen ? "PASS" : "FAIL");
  return settled && fallen ? 0 : 1;
}
