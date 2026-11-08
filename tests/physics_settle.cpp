// Settling check for the vehicle rigid-body integrator. Built twice by
// CMake (native and Project Chrono backends); both must bring a sprung box
// to rest on flat ground with the wheel loads carrying its weight.
#include "slrr/vehicle_dynamics.hpp"

#include <cmath>
#include <cstdio>
#include <algorithm>

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

#if SLRR_HAS_CHRONO
Matrix frame_at(float x, float y, float z) {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
}

std::shared_ptr<const CollisionMesh> test_mesh(bool with_wall) {
  auto mesh = std::make_shared<CollisionMesh>();
  mesh->positions = {-10, 0, -10, 10, 0, -10, 10, 0, 10, -10, 0, 10};
  // Counter-clockwise when viewed from above: upward contact normals.
  mesh->indices = {0, 2, 1, 0, 3, 2};
  if (with_wall) {
    const uint32_t base = 4;
    const float wall[] = {1, 0, -10, 1, 0, 10, 1, 3, 10, 1, 3, -10};
    mesh->positions.insert(mesh->positions.end(), wall, wall + 12);
    const uint32_t triangles[] = {base, base + 1, base + 2,
                                  base, base + 2, base + 3};
    mesh->indices.insert(mesh->indices.end(), triangles, triangles + 6);
  }
  return mesh;
}

bool mesh_contacts() {
  const Terrain no_ground = [](double, double, double, GroundHit &) {
    return false;
  };
  const Terrain flat = [](double, double, double, GroundHit &hit) {
    hit.height = 0;
    hit.normal = {0, 1, 0};
    return true;
  };
  const double tick = 1.0 / VehicleDynamics::update_frequency;
  auto run = [&](VehicleDynamics &vd, int frames, const Terrain &terrain) {
    for (int i = 0; i < frames; ++i)
      vd.advance(tick, terrain);
  };

  VehicleDynamics drop;
  drop.body.configure(900, {-.5, -.2, -1}, {.5, .2, 1}, {0, 0, 0});
  drop.body.set_frame(frame_at(0, 1, 0));
  drop.collision = test_mesh(false);
  run(drop, 4 * 60, no_ground);
  const bool floor_ok = drop.body.position.y > .15 &&
                        drop.body.position.y < .27 &&
                        drop.body.velocity.length() < .05 &&
                        drop.body.omega().length() < .05;

  VehicleDynamics buried;
  buried.body.configure(900, {-.5, -.2, -1}, {.5, .2, 1}, {0, 0, 0});
  buried.body.set_frame(frame_at(0, 0, 0));
  buried.collision = test_mesh(false);
  run(buried, 4 * 60, no_ground);
  const bool buried_ok = buried.body.position.y > .15 &&
                         buried.body.position.y < .27 &&
                         buried.body.velocity.length() < .05;

  // External placement must clear warm contacts and preserve the new frame.
  drop.body.set_frame(frame_at(-3, 1, 0));
  run(drop, 2 * 60, no_ground);
  const bool teleport_ok = std::fabs(drop.body.position.x + 3) < .05 &&
                           drop.body.position.y > .15 &&
                           drop.body.position.y < .27 &&
                           drop.body.velocity.length() < .05;

  VehicleDynamics wall;
  wall.body.configure(900, {-.5, -.2, -.5}, {.5, .2, .5}, {0, 0, 0});
  wall.body.set_frame(frame_at(0, .4f, 0));
  wall.body.velocity = {3, 0, 0};
  wall.collision = test_mesh(true);
  double maximum_x = wall.body.position.x;
  for (int i = 0; i < 2 * 60; ++i) {
    wall.advance(tick, no_ground);
    maximum_x = std::max(maximum_x, wall.body.position.x);
  }
  const bool wall_ok = maximum_x < .56 && wall.body.position.y > .15;

  VehicleDynamics rigid;
  rigid.body.configure(900, {-.7, -.05, -1.2}, {.7, .15, 1.2}, {0, 0, 0});
  rigid.body.set_frame(frame_at(0, 1, 0));
  for (int i = 0; i < 4; ++i) {
    auto &wheel = rigid.wheels[i];
    wheel.enabled = true;
    wheel.rigid_contact = true;
    wheel.center = {i % 2 ? .6 : -.6, -.1, i < 2 ? .9 : -.9};
    wheel.radius = .3;
    wheel.rest = wheel.minimum = wheel.maximum = wheel.anchor = .1;
  }
  rigid.collision = test_mesh(false);
  run(rigid, 4 * 60, flat);
  bool supported = true;
  for (const auto &state : rigid.state)
    supported = supported && state.contact;
  const bool rigid_ok = supported && rigid.body.position.y > .35 &&
                        rigid.body.position.y < .47 &&
                        rigid.body.velocity.length() < .08 &&
                        rigid.body.omega().length() < .08;
  std::printf("mesh floor-height=%.5f buried-height=%.5f teleport-x=%.5f "
              "wall-max-x=%.5f rigid-height=%.5f\n",
              drop.body.position.y, buried.body.position.y,
              drop.body.position.x, maximum_x, rigid.body.position.y);
  return floor_ok && buried_ok && teleport_ok && wall_ok && rigid_ok;
}
#else
bool mesh_contacts() { return true; }
#endif

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
  const bool collisions = mesh_contacts();
  std::puts(settled && fallen && collisions ? "PASS" : "FAIL");
  return settled && fallen && collisions ? 0 : 1;
}
