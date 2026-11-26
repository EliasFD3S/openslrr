#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace slrr {

// The stock java.util.resource.ParticleSystem natives (#454) on the host's
// particle pool (particle_cloud.hpp). A script system names a cloud (its
// RenderRef type: an RPK `particle <cfg>` entry) and a parent; each named
// action is a source or a counter in the parent's frame. Owned by the host
// thread; keys are the script objects, never dereferenced.
using Vec3f = std::array<float, 3>;

// init(parent, type, alias). cloud: particle_clouds() index, 0xffff none.
void script_particles_init(const void *self, const void *parent, uint16_t cloud);
// setSource (continuous, `rate` particles/s) or setDirectSource (`rate`
// particles once): positions in a sphere of radii rmin..rmax around pos,
// velocities in a sphere of radii vmin..vmax around vel, both in the
// parent's frame. A new call with the same alias replaces the action.
void script_particles_source(const void *self, const std::string &alias, const Vec3f &pos,
                             float rmin, float rmax, const Vec3f &vel, float vmin, float vmax,
                             float rate, bool direct);
// setCounter / getCounter: the live particles of the system's cloud within r
// of pos (parent frame).
void script_particles_counter(const void *self, const std::string &alias, const Vec3f &pos,
                              float r);
int script_particles_count(const void *self, const std::string &alias);
void script_particles_delete(const void *self, const std::string &alias);  // delAction
void script_particles_stop(const void *self);  // every source stops emitting
void script_particles_freq(const void *self, float frequency);  // setFreq: a multiplier
void script_particles_permanent(const void *self, bool permanent);  // modePermanent
void script_particles_destroy(const void *self);
bool script_particles_exists(const void *self);

// Once per host frame, before particles_update: emit from every source at
// its parent's current pose. Parents outside the active world (an OSD
// cursor) emit nothing.
void script_particles_update(double dt);
void script_particles_clear();

}  // namespace slrr
