// Settling check for the vehicle rigid-body integrator. Built twice by
// CMake (native and Project Chrono backends); both must bring a sprung box
// to rest on flat ground with the wheel loads carrying its weight.
#include "slrr/vehicle_dynamics.hpp"
#include "slrr/suspension_geometry.hpp"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <limits>

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
  // Semi-implicit Euler over one second lags by g * dt / 2.
  return height_error < 9.81 * VehicleDynamics::timestep * .5 + .01 &&
         speed_error < 1e-6 &&
         std::fabs(vd.body.position.x) < 1e-9 &&
         std::fabs(vd.body.position.z) < 1e-9;
}

#if SLRR_HAS_CHRONO
Matrix frame_at(float x, float y, float z) {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
}
Matrix frame_roll(float angle) {
  const float c = std::cos(angle), s = std::sin(angle);
  return {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, .8f, 0, 1};
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
bool suspension_contacts(bool arms, bool mixed, bool mesh = true,
                         float roll = 0) {
  const Terrain flat = [](double, double, double, GroundHit &hit) {
    hit.height = 0;
    hit.normal = {0, 1, 0};
    return true;
  };
  VehicleDynamics vd;
  const double mass = 1000;
  vd.body.configure(mass, {-.8, -.2, -1.5}, {.8, .4, 1.5}, {0, 0, 0});
  vd.body.set_frame(roll ? frame_roll(roll) : frame_at(0, .8f, 0));
  int constrained = 0;
  double expected_load = mass * 9.81;
  double expected_normal = expected_load;
  for (int i = 0; i < 4; ++i) {
    auto &wheel = vd.wheels[i];
    wheel.enabled = true;
    wheel.center = {i % 2 ? .7 : -.7, -.1, i < 2 ? 1.1 : -1.1};
    wheel.radius = .3;
    wheel.rest = .25;
    wheel.minimum = .05;
    wheel.maximum = .35;
    wheel.spring = 40000;
    wheel.bump_damping = 3000;
    wheel.rebound_damping = 4000;
    if (arms) {
      wheel.arm_length = .4;
      wheel.arm_pivot = wheel.center + Vec3{0, .1, -.4};
      wheel.arm_axis = {1, 0, 0};
    }
    if (!mixed || i != 0) {
      wheel.unsprung = 20;
      expected_normal += wheel.unsprung * 9.81;
      if (mesh)
        ++constrained;
    }
  }
  if (mesh)
    vd.collision = test_mesh(false);
  const double tick = 1.0 / VehicleDynamics::update_frequency;
  double load = 0, contacts = 0, normal = 0;
  for (int frame = 0; frame < 6 * 60; ++frame) {
    vd.advance(tick, flat);
    if (frame >= 5 * 60) {
      for (const auto &state : vd.state) {
        load += state.force;
        normal += state.normal_load;
        contacts += state.contact;
      }
    }
  }
  load /= 60;
  normal /= 60;
  contacts /= 60;
  int constrained_states = 0;
  bool geometry = true;
  for (size_t i = 0; i < vd.state.size(); ++i) {
    const auto &state = vd.state[i];
    const auto &wheel = vd.wheels[i];
    constrained_states += state.constrained;
    geometry = geometry && state.center.finite() && state.hub.finite() &&
               state.length >= wheel.minimum - .02 &&
               state.length <= wheel.maximum + .02;
    if (state.constrained && arms) {
      const Vec3 pivot =
          vd.body.position +
          vd.body.orientation.rotate(wheel.arm_pivot - vd.body.center);
      geometry = geometry &&
                 std::fabs((state.center - pivot).length() -
                           wheel.arm_length) < .01;
    }
  }
  const bool ok = constrained_states == constrained && contacts > 3.9 &&
                  std::fabs(load - expected_load) < expected_load * .08 &&
                  (!mesh || std::fabs(normal - expected_normal) <
                                expected_normal * .08) &&
                  vd.body.velocity.length() < .08 &&
                  vd.body.omega().length() < .08 && geometry &&
                  vd.body.position.y > .05 && vd.body.position.y < 1;
  std::printf("suspension arms=%d mixed=%d mesh=%d height=%.5f "
              "support=%.1f/%.1f normal=%.1f/%.1f contacts=%.2f "
              "constrained=%d roll=%.2f\n",
              arms ? 1 : 0, mixed ? 1 : 0, mesh ? 1 : 0,
              vd.body.position.y, load, expected_load, normal,
              expected_normal, contacts, constrained_states, roll);
  return ok;
}
bool seeded_suspension() {
  const Terrain flat = [](double, double, double, GroundHit &hit) {
    hit.height = 0;
    hit.normal = {0, 1, 0};
    return true;
  };
  double maximum_error = 0, maximum_force_error = 0;
  int force_checks = 0;
  for (int arms = 0; arms < 2; ++arms) {
    VehicleDynamics vd;
    vd.body.configure(1000, {-.8, -.2, -1.5}, {.8, .4, 1.5}, {0, 0, 0});
    for (int i = 0; i < 4; ++i) {
      auto &wheel = vd.wheels[i];
      wheel.enabled = true;
      wheel.center = {i % 2 ? .7 : -.7, -.1, i < 2 ? 1.1 : -1.1};
      wheel.radius = .3;
      wheel.rest = .25;
      wheel.minimum = .05;
      wheel.maximum = .35;
      wheel.spring = 40000;
      wheel.bump_damping = 3000;
      wheel.rebound_damping = 4000;
      wheel.unsprung = 20;
      if (arms) {
        wheel.arm_length = .4;
        wheel.arm_pivot = wheel.center + Vec3{0, .1, -.4};
        wheel.arm_axis = {1, 0, 0};
      }
      vd.state[i].seeded = true;
      vd.state[i].length = .17;
    }
    Vec3 initial_hub;
    const auto &first = vd.wheels[0];
    const auto ray = first.center + first.down.normalized() *
                                         (.17 - first.anchor_length());
    if (!hinge_hub(first, ray, initial_hub))
      return false;
    vd.body.set_frame(frame_at(0, first.radius - initial_hub.y, 0));
    vd.collision = test_mesh(false);
    vd.advance(VehicleDynamics::timestep, flat);
    for (const auto &state : vd.state) {
      if (!state.constrained)
        return false;
      maximum_error =
          std::max(maximum_error, std::fabs(state.length - .17));
      if (arms && state.contact) {
        const auto &wheel = vd.wheels[force_checks];
        const double expected = suspension_force(
            state.length, state.speed, wheel.rest, wheel.maximum, wheel.spring,
            wheel.bump_damping, wheel.rebound_damping);
        if (expected > 100) {
          maximum_force_error =
              std::max(maximum_force_error,
                       std::fabs(state.force - expected) / expected);
          ++force_checks;
        }
      }
    }
    if (arms) {
      CornerGeometry geometry;
      geometry.reference = first.center;
      const Vec3 pivot = first.arm_pivot - first.center;
      geometry.arm = {float(first.arm_length), float(pivot.x), float(pivot.y),
                      float(pivot.z), float(first.arm_axis.x),
                      float(first.arm_axis.y), float(first.arm_axis.z)};
      geometry.hub[0] = -1;
      CornerFrames frames;
      if (!solve_corner_geometry(geometry, vd.state[0].hub, frames) ||
          (frames.arm_origin - vd.state[0].hub).length() > 1e-5 ||
          Vec3{frames.arm_rotation.x, frames.arm_rotation.y,
               frames.arm_rotation.z}
                  .length() < 1e-6)
        return false;
    }
    vd.release_wheels();
    for (auto &state : vd.state) {
      state.seeded = true;
      state.length = .19;
    }
    vd.advance(VehicleDynamics::timestep, flat);
    for (const auto &state : vd.state) {
      if (!state.constrained)
        return false;
      maximum_error =
          std::max(maximum_error, std::fabs(state.length - .19));
    }
  }
  std::printf("suspension seeded-travel-error=%.6f arm-force-error=%.6f\n",
              maximum_error, maximum_force_error);
  return maximum_error < .01 &&
         (force_checks == 0 ||
          (force_checks == 4 && maximum_force_error < .05));
}
bool ownership_suspension() {
  const Terrain flat = [](double, double, double, GroundHit &hit) {
    hit.height = 0;
    hit.normal = {0, 1, 0};
    return true;
  };
  for (int invalid_seed = 0; invalid_seed < 2; ++invalid_seed) {
    VehicleDynamics vd;
    vd.body.configure(1000, {-.8, -.2, -1.5}, {.8, .4, 1.5}, {0, 0, 0});
    vd.body.set_frame(frame_at(0, .8f, 0));
    for (int i = 0; i < 4; ++i) {
      auto &wheel = vd.wheels[i];
      wheel.enabled = true;
      wheel.center = {i % 2 ? .7 : -.7, -.1, i < 2 ? 1.1 : -1.1};
      wheel.radius = .3;
      wheel.rest = .25;
      wheel.minimum = .05;
      wheel.maximum = .35;
      wheel.spring = 40000;
      wheel.bump_damping = 3000;
      wheel.rebound_damping = 4000;
      wheel.unsprung = 20;
      if (invalid_seed) {
        vd.state[i].seeded = true;
        vd.state[i].length = std::numeric_limits<double>::quiet_NaN();
      }
    }
    vd.collision = test_mesh(false);
    vd.advance(VehicleDynamics::timestep, flat);
    if (!invalid_seed) {
      for (auto &state : vd.state)
        state.length = std::numeric_limits<double>::quiet_NaN();
      vd.advance(VehicleDynamics::timestep, flat);
    }
    for (const auto &state : vd.state)
      if (!state.constrained || !std::isfinite(state.length) ||
          !std::isfinite(state.speed))
        return false;
  }
  VehicleDynamics degenerate;
  degenerate.body.configure(1000, {-.8, -.2, -1.5}, {.8, .4, 1.5},
                            {0, 0, 0});
  degenerate.body.set_frame(frame_at(0, .8f, 0));
  for (int i = 0; i < 4; ++i) {
    auto &wheel = degenerate.wheels[i];
    wheel.enabled = true;
    wheel.center = {i % 2 ? .7 : -.7, -.1, i < 2 ? 1.1 : -1.1};
    wheel.radius = .3;
    wheel.rest = .25;
    wheel.minimum = .05;
    wheel.maximum = .35;
    wheel.spring = 40000;
    wheel.bump_damping = 3000;
    wheel.rebound_damping = 4000;
    wheel.unsprung = 20;
    wheel.arm_length = .7;
    wheel.arm_pivot = wheel.center + Vec3{0, -.1, 0};
    wheel.arm_axis = {1, 0, 0};
    degenerate.state[i].seeded = true;
    degenerate.state[i].length = .1;
  }
  degenerate.collision = test_mesh(false);
  degenerate.advance(VehicleDynamics::timestep, flat);
  int native = 0;
  for (const auto &state : degenerate.state)
    native += !state.constrained;
  if (native != 4)
    return false;
  std::puts("suspension ownership-transitions=OK degenerate-rest-native=4");
  return true;
}

bool ridge_contacts() {
  auto mesh = std::make_shared<CollisionMesh>();
  mesh->positions = {-2, 0, -4, 0, .2f, -4, 0, .2f, 4, -2, 0, 4,
                     0, .2f, -4, 2, 0, -4, 2, 0, 4, 0, .2f, 4};
  mesh->indices = {0, 2, 1, 0, 3, 2, 4, 6, 5, 4, 7, 6};
  VehicleDynamics vd;
  const double mass = 1000;
  vd.body.configure(mass, {-.8, -.2, -1.5}, {.8, .4, 1.5}, {0, 0, 0});
  vd.body.set_frame(frame_at(0, .8f, 0));
  const double z[] = {1.1, .5, -.5, -1.1};
  for (int i = 0; i < 4; ++i) {
    auto &wheel = vd.wheels[i];
    wheel.enabled = true;
    wheel.center = {0, -.1, z[i]};
    wheel.radius = .3;
    wheel.rest = .25;
    wheel.minimum = .05;
    wheel.maximum = .35;
    wheel.spring = 40000;
    wheel.bump_damping = 3000;
    wheel.rebound_damping = 4000;
    wheel.unsprung = 20;
  }
  vd.collision = mesh;
  const Terrain ridge = [](double x, double, double, GroundHit &hit) {
    hit.height = .2 * (1 - std::min(1.0, std::fabs(x) / 2));
    hit.normal = {x < 0 ? -.1 : .1, 1, 0};
    return true;
  };
  const double tick = 1.0 / VehicleDynamics::update_frequency;
  double support = 0, normal = 0, contacts = 0;
  for (int frame = 0; frame < 6 * 60; ++frame) {
    vd.advance(tick, ridge);
    if (frame >= 5 * 60) {
      for (const auto &state : vd.state) {
        support += state.force;
        normal += state.normal_load;
        contacts += state.contact;
      }
    }
  }
  support /= 60;
  normal /= 60;
  contacts /= 60;
  bool geometry = true;
  for (const auto &state : vd.state)
    geometry = geometry && state.patch.finite() &&
               state.normal.length() > .9;
  const double expected_support = mass * 9.81;
  const double expected_normal = (mass + 4 * 20) * 9.81;
  const bool ok = contacts > 3.8 && geometry &&
                  std::fabs(support - expected_support) <
                      expected_support * .1 &&
                  std::fabs(normal - expected_normal) < expected_normal * .1;
  std::printf("suspension ridge-support=%.1f normal=%.1f contacts=%.2f\n",
              support, normal, contacts);
  return ok;
}

bool singular_suspension() {
  VehicleDynamics vd;
  vd.body.configure(1000, {-.8, -.2, -1.5}, {.8, .4, 1.5}, {0, 0, 0});
  vd.body.set_frame(frame_at(0, .8f, 0));
  for (int i = 0; i < 4; ++i) {
    auto &wheel = vd.wheels[i];
    wheel.enabled = true;
    wheel.center = {i % 2 ? .7 : -.7, -.1, i < 2 ? 1.1 : -1.1};
    wheel.radius = .3;
    wheel.rest = .25;
    wheel.minimum = .05;
    wheel.maximum = .35;
    wheel.spring = 40000;
    wheel.bump_damping = 3000;
    wheel.rebound_damping = 4000;
    wheel.unsprung = 20;
    wheel.arm_length = .4;
    wheel.arm_pivot = wheel.center + Vec3{0, .4, 0};
    wheel.arm_axis = {1, 0, 0};
  }
  vd.collision = test_mesh(false);
  const Terrain flat = [](double, double, double, GroundHit &hit) {
    hit.height = 0;
    hit.normal = {0, 1, 0};
    return true;
  };
  double support = 0, normal = 0, contacts = 0;
  const double tick = 1.0 / VehicleDynamics::update_frequency;
  for (int frame = 0; frame < 4 * 60; ++frame) {
    vd.advance(tick, flat);
    if (frame >= 3 * 60) {
      for (const auto &state : vd.state) {
        support += state.force;
        normal += state.normal_load;
        contacts += state.contact;
      }
    }
  }
  support /= 60;
  normal /= 60;
  contacts /= 60;
  int rejected = 0;
  for (const auto &state : vd.state)
    rejected += !state.constrained;
  // This geometry reaches maximum droop in the native solver: spring support
  // is zero, but native normal_load still includes each unsprung weight.
  const double expected_support = 0;
  const double expected_normal = 4 * 20 * 9.81;
  std::printf("suspension singular-rejected=%d support=%.1f normal=%.1f "
              "contacts=%.2f\n",
              rejected, support, normal, contacts);
  return rejected == 4 && contacts > 3.5 &&
         std::fabs(support - expected_support) <=
             std::max(1.0, expected_support * .15) &&
         std::fabs(normal - expected_normal) < expected_normal * .15;
}
#else
bool mesh_contacts() { return true; }
bool suspension_contacts(bool, bool, bool = true, float = 0) { return true; }
bool seeded_suspension() { return true; }
bool singular_suspension() { return true; }
bool ridge_contacts() { return true; }
bool ownership_suspension() { return true; }
#endif

// Upstream drivetrain (DriveInput, 120 Hz) must propel and brake a car on
// both the native suspension and, when available, Chrono-owned corners.
bool drive_case(bool chrono_wheels) {
  VehicleDynamics vd;
  vd.body.configure(1000, {-.8, -.2, -1.5}, {.8, .4, 1.5}, {0, 0, 0});
  Matrix frame{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, .8f, 0, 1};
  vd.body.set_frame(frame);
  for (int i = 0; i < 4; ++i) {
    auto &wheel = vd.wheels[i];
    wheel.enabled = true;
    wheel.center = {i % 2 ? .7 : -.7, -.1, i < 2 ? -1.1 : 1.1};
    wheel.radius = .3;
    wheel.rest = .25;
    wheel.minimum = .05;
    wheel.maximum = .35;
    wheel.spring = 40000;
    wheel.bump_damping = 3000;
    wheel.rebound_damping = 4000;
    if (chrono_wheels)
      wheel.unsprung = 20;
  }
#if SLRR_HAS_CHRONO
  if (chrono_wheels) {
    // A long road: the car covers well over 20 m during the throttle phase.
    auto road = std::make_shared<CollisionMesh>();
    road->positions = {-20, 0, -500, 20, 0, -500, 20, 0, 50, -20, 0, 50};
    road->indices = {0, 2, 1, 0, 3, 2};
    vd.collision = road;
  }
#endif
  const Terrain flat = [](double, double, double, GroundHit &hit) {
    hit.height = 0;
    hit.normal = {0, 1, 0};
    return true;
  };
  const double tick = 1.0 / VehicleDynamics::update_frequency;
  for (int i = 0; i < 60; ++i)
    vd.advance(tick, flat);
  auto &drive = vd.drive;
  drive.torque = [](double) { return 220.0; };
  drive.inertia = .3;
  drive.idle = 800;
  drive.limit = 6500;
  drive.starter = 60;
  drive.engaged = true;
  drive.gears = 4;
  drive.ratios = {0, 12.9, 8.1, 5.5, 4.1, 0, 0, -12.9};
  drive.brake_scale = 40;
  drive.bearing_scale = .1;
  for (size_t i = 0; i < 4; ++i) {
    drive.wheels[i].weight = i >= 2 ? 1 : 0;
    drive.wheels[i].brake = 60;
  }
  const Vec3 forward{0, 0, -1};
  drive.throttle = 1;
  for (int i = 0; i < 5 * 60; ++i)
    vd.advance(tick, flat);
  const double forward_speed = vd.body.velocity.dot(forward);
  drive.throttle = 0;
  drive.brake = 1;
  for (int i = 0; i < 3 * 60; ++i)
    vd.advance(tick, flat);
  const double brake_speed = vd.body.velocity.dot(forward);
  int constrained = 0;
  for (const auto &state : vd.state)
    constrained += state.constrained;
  const bool ok = vd.engine_running && forward_speed > 3 &&
                  brake_speed < forward_speed * .3 &&
                  vd.body.position.y > .05 && vd.body.position.y < 2 &&
                  (!chrono_wheels || !SLRR_HAS_CHRONO || constrained == 4);
  std::printf("drive chrono=%d constrained=%d engine=%d forward=%.3f "
              "brake=%.3f height=%.3f\n",
              chrono_wheels ? 1 : 0, constrained, vd.engine_running ? 1 : 0,
              forward_speed, brake_speed, vd.body.position.y);
  return ok;
}

bool driver_controls() { return drive_case(false) && drive_case(true); }

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
  const bool driving = driver_controls();
  const bool suspension = suspension_contacts(false, true) &&
                          suspension_contacts(true, false) &&
                          suspension_contacts(false, false, false) &&
                          suspension_contacts(false, false, true, .12f) &&
                          suspension_contacts(true, false, true, .12f) &&
                          seeded_suspension() && singular_suspension() &&
                          ridge_contacts() && ownership_suspension();
  std::puts(settled && fallen && collisions && driving && suspension ? "PASS"
                                                                     : "FAIL");
  return settled && fallen && collisions && driving && suspension ? 0 : 1;
}
