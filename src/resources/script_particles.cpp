#include "slrr/script_particles.hpp"

#include "slrr/particle_cloud.hpp"
#include "slrr/scene_instances.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace slrr {
namespace {

struct Action {
  bool counter = false;
  bool direct_pending = false;
  float direct_count = 0;
  Vec3f pos{}, vel{};
  float rmin = 0, rmax = 0, vmin = 0, vmax = 0, rate = 0, radius = 0;
  ParticleSystem emitter;
};

struct System {
  const void *parent = nullptr;
  uint16_t cloud = 0xffff;
  float frequency = 1.f;
  bool stopped = false;
  bool permanent = false;  // stored: its meaning in the original is not recovered
  std::map<std::string, Action> actions;
  float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  bool placed = false;  // matrix holds the parent's pose this frame
};

std::unordered_map<const void *, System> g_systems;

ParticleDomain sphere(const Vec3f &centre, float r0, float r1) {
  ParticleDomain d;
  d.kind = ParticleDomain::Kind::Sphere;
  d.v = {centre[0], centre[1], centre[2], std::max(0.f, r0), std::max(0.f, r1)};
  d.count = 5;
  return d;
}

Vec3f rotate(const float *m, const Vec3f &v) {
  return {m[0] * v[0] + m[4] * v[1] + m[8] * v[2], m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
          m[2] * v[0] + m[6] * v[1] + m[10] * v[2]};
}

Vec3f transform(const float *m, const Vec3f &v) {
  const Vec3f r = rotate(m, v);
  return {r[0] + m[12], r[1] + m[13], r[2] + m[14]};
}

// The action's emitter, placed at the parent's pose: the source sphere's
// centre becomes the emitter position, the velocity sphere is rotated into
// world space and its offset sphere stays around the origin.
void place(System &system, Action &action) {
  if (!action.emitter.ready() && system.cloud != 0xffff) {
    action.emitter.init(system.cloud);
    action.emitter.mode_permanent(true);
  }
  const Vec3f centre = transform(system.matrix, action.pos);
  const Vec3f velocity = rotate(system.matrix, action.vel);
  // Uniform scale is ignored: the velocity sphere is not rotated, only its
  // centre (the radii are rotation invariant).
  action.emitter.set_domains(sphere({0, 0, 0}, action.rmin, action.rmax),
                             sphere(velocity, action.vmin, action.vmax), action.rate);
  action.emitter.set_source(centre, {0, 0, 0}, {0, 0, 0});
}

}  // namespace

void script_particles_init(const void *self, const void *parent, uint16_t cloud) {
  if (!self) return;
  auto &system = g_systems[self];
  system = System{};
  system.parent = parent;
  system.cloud = cloud;
}

void script_particles_source(const void *self, const std::string &alias, const Vec3f &pos,
                             float rmin, float rmax, const Vec3f &vel, float vmin, float vmax,
                             float rate, bool direct) {
  const auto it = g_systems.find(self);
  if (it == g_systems.end()) return;
  auto &action = it->second.actions[alias];
  action.counter = false;
  action.pos = pos;
  action.vel = vel;
  action.rmin = std::min(rmin, rmax);
  action.rmax = std::max(rmin, rmax);
  action.vmin = std::min(vmin, vmax);
  action.vmax = std::max(vmin, vmax);
  if (direct) {
    action.rate = 0;
    action.direct_pending = true;
    action.direct_count = std::clamp(rate, 0.f, 4096.f);
  } else {
    action.rate = std::clamp(rate, 0.f, 20000.f);
  }
  it->second.stopped = false;
}

void script_particles_counter(const void *self, const std::string &alias, const Vec3f &pos,
                              float r) {
  const auto it = g_systems.find(self);
  if (it == g_systems.end()) return;
  auto &action = it->second.actions[alias];
  action = Action{};
  action.counter = true;
  action.pos = pos;
  action.radius = std::max(0.f, r);
}

int script_particles_count(const void *self, const std::string &alias) {
  const auto it = g_systems.find(self);
  if (it == g_systems.end()) return 0;
  const auto found = it->second.actions.find(alias);
  if (found == it->second.actions.end() || !found->second.counter) return 0;
  const auto &system = it->second;
  const Vec3f centre = transform(system.matrix, found->second.pos);
  const float r2 = found->second.radius * found->second.radius;
  int count = 0;
  for (const auto &p : particles()) {
    if (p.cloud != system.cloud) continue;
    const float dx = p.position[0] - centre[0], dy = p.position[1] - centre[1],
                dz = p.position[2] - centre[2];
    if (dx * dx + dy * dy + dz * dz <= r2) ++count;
  }
  return count;
}

void script_particles_delete(const void *self, const std::string &alias) {
  const auto it = g_systems.find(self);
  if (it != g_systems.end()) it->second.actions.erase(alias);
}

void script_particles_stop(const void *self) {
  const auto it = g_systems.find(self);
  if (it == g_systems.end()) return;
  it->second.stopped = true;
  for (auto &[alias, action] : it->second.actions) action.emitter.stop();
}

void script_particles_freq(const void *self, float frequency) {
  const auto it = g_systems.find(self);
  if (it != g_systems.end()) it->second.frequency = std::isfinite(frequency) ? frequency : 0.f;
}

void script_particles_permanent(const void *self, bool permanent) {
  const auto it = g_systems.find(self);
  if (it != g_systems.end()) it->second.permanent = permanent;
}

void script_particles_destroy(const void *self) { g_systems.erase(self); }

bool script_particles_exists(const void *self) { return g_systems.count(self) != 0; }

void script_particles_update(double dt) {
  if (!(dt > 0)) return;
  for (auto &[self, system] : g_systems) {
    system.placed = system.parent &&
                    scene_instances_script_matrix(const_cast<void *>(system.parent), system.matrix);
    if (!system.placed || system.cloud == 0xffff) continue;
    for (auto &[alias, action] : system.actions) {
      if (action.counter) continue;
      place(system, action);
      if (action.direct_pending) {
        action.direct_pending = false;
        action.emitter.emit_direct(static_cast<int>(std::lround(action.direct_count)));
      }
      if (system.stopped || !(action.rate > 0)) continue;
      action.emitter.set_freq(std::max(0.f, system.frequency));
      action.emitter.advance(dt);
    }
  }
}

void script_particles_clear() { g_systems.clear(); }

}  // namespace slrr
