#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace slrr {

// Stock particle clouds (#454): `particles/scripts/particle_clouds/*.cfg`,
// the format of David McAllister's Particle System API that the original's
// ParticleSystem natives drive. `[STATE]` sets what a new particle starts
// and ends with (each a domain: `point x y z`, `line x0 y0 z0 x1 y1 z1`,
// `sphere ...`, `box ...`), `[ACTION]` what runs every step (`source`,
// `gravity`, `damping`, `move`, `bounce`).
struct ParticleDomain {
  enum class Kind : uint8_t { Point, Line, Sphere, Box, Plane, Disc };
  Kind kind = Kind::Point;
  std::array<float, 9> v{};
  int count = 0;  // numbers the cfg gave
  // A point inside the domain. A three-number sphere is an ellipsoid around
  // the origin with those radii (`sphere 1 1 1` in asphalt.cfg: a unit
  // spread, not a fixed (1, 1, 1) velocity); with more numbers it is a centre
  // and a shell between the two radii (grass.cfg `sphere 0 0 0 0.1 2.5`).
  std::array<float, 3> sample(std::mt19937 &rng) const;
};

struct ParticleCloudConfig {
  bool ok = false;
  // [STATE]. Sizes are the particle's width in metres (x only), colours
  // rgb 0..1, alpha x only, angle the billboard's rotation in radians.
  ParticleDomain source_size, dest_size, source_color, dest_color, source_alpha, dest_alpha,
      source_angle, dest_angle, lifetime;
  int type = 0;          // 1: debris (grass blades, pebbles), else a soft cloud
  float velscale = 0.f;  // type 1: stretch along the velocity
  // [ACTION]
  float rate = 0.f;  // `source s0 <rate>`: particles per second at frequency 1
  ParticleDomain position, velocity;
  std::array<float, 3> gravity{};
  // PAPI damping: each second the velocity keeps `damping` per axis
  // (v -= (1 - damping) v dt).
  std::array<float, 3> damping{1.f, 1.f, 1.f};
  bool has_damping = false;
  // PAPI pMove(move_velocity, move_rotational_velocity): `move m0 1 1` in
  // the smoke cfgs. grass.cfg writes `move m0 0 0` for blades that fly: a
  // `type 1` particle is physical debris the engine moves itself (its own
  // integration, gravity and ground), so the flags only hold back clouds.
  bool move_velocity = true, move_rotation = true;
  // `bounce b0 friction resilience cutoff plane ...` (commented out in every
  // stock cfg). The host floor stands in for the plane.
  bool bounce = false;
  // Sparks (#454): drawn glowing and added to the scene, stretched along
  // their motion, never lit or shaded. Set for a cfg named spark*.
  bool emissive = false;
  float bounce_friction = 0.f, bounce_resilience = .35f;
  std::string texture;  // resolved by the caller (RPK `texture`)
};

ParticleCloudConfig parse_particle_cloud(std::string_view text);

// The mean end width squared times the mean lifetime (m^2 s): how much
// screen a cloud's particle covers over its life. Stock asphalt.cfg's is the
// reference the tyre emitters scale their rate by.
double particle_cloud_coverage(const ParticleCloudConfig &config);
double particle_cloud_coverage_reference();

// One live particle, in script world space.
struct Particle {
  std::array<float, 3> position{}, velocity{};
  std::array<float, 3> color0{}, color1{};
  std::array<float, 3> ground{}, ground_normal{0, 1, 0};
  float size0 = 0, size1 = 0, alpha0 = 0, alpha1 = 0, angle0 = 0, angle1 = 0;
  float age = 0, life = 1;
  float seed = 0;
  uint16_t cloud = 0;  // index in particle_clouds()
  // A young particle turns about an axis (a tyre's), the swirl fading over
  // its first half second: smoke wrapping round the wheel (#454).
  std::array<float, 3> swirl_center{}, swirl_axis{};
  float swirl = 0;  // rad/s about swirl_axis, 0 none
  // The share of the cfg's gravity this particle feels (tyre smoke hangs
  // in the air instead of rising at the cfg's 2 m/s).
  float buoyancy = 1;
};

// A particle's width now: it swells fast, then slowly (sqrt of its age
// fraction), so a fresh puff joins the cloud instead of a small drop.
float particle_width(const Particle &p);

// One particle placed by the caller (emit), with the cloud's sizes, colours
// and lifetimes; the cfg's velocity domain adds a little scatter.
struct ParticleSpawn {
  std::array<float, 3> position{}, velocity{};
  std::array<float, 3> ground{}, ground_normal{0, 1, 0};
  std::array<float, 3> swirl_center{}, swirl_axis{};
  float swirl = 0;
  float min_size = 0;  // the birth width at least this (a tyre's)
  float buoyancy = 1;
  // Mixed into the cloud's colours by tint_weight (dust takes its ground's).
  std::array<float, 3> tint{};
  float tint_weight = 0;
};

// The emitter, shaped like the stock ParticleSystem natives (init, setFreq,
// setSource, setDirectSource, setCounter/getCounter, stop, modePermanent).
// Particles go into the shared pool below; a stopped or deleted system
// leaves its particles to finish their lives.
class ParticleSystem {
 public:
  // init: the cloud by index (particle_cloud_register).
  void init(uint16_t cloud) {
    cloud_ = cloud;
    ready_ = true;
  }
  bool ready() const { return ready_; }
  // setFreq: a multiplier on the cfg's source rate.
  void set_freq(float frequency) { frequency_ = frequency; }
  float freq() const { return frequency_; }
  // setSource: where the source domain is centred and the velocity the
  // particles inherit (the tyre's throw plus the drag of the car's wake),
  // with the ground plane they must not sink through (a zero normal: none).
  void set_source(const std::array<float, 3> &position, const std::array<float, 3> &velocity,
                  const std::array<float, 3> &ground_normal);
  // The script natives' setSource / setDirectSource: their position and
  // velocity spheres and rate replace the cfg's source.
  void set_domains(const ParticleDomain &position, const ParticleDomain &velocity, float rate) {
    custom_ = true;
    position_domain_ = position;
    velocity_domain_ = velocity;
    rate_ = rate;
  }
  // setDirectSource: emit `count` particles now, whatever the frequency.
  void emit_direct(int count);
  // The particles due after dt seconds at the current frequency (the cfg's
  // rate times it), for a caller that places each one itself (emit).
  int due(double dt);
  void emit(const ParticleSpawn &spawn);
  void set_counter(uint32_t counter) { counter_ = counter; }
  uint32_t counter() const { return counter_; }
  // modePermanent: keep emitting while the frequency is above zero, without
  // the per-frame `set_freq` the tyres send.
  void mode_permanent(bool permanent) { permanent_ = permanent; }
  void stop() {
    frequency_ = 0;
    carry_ = 0;
  }
  // Emits for dt seconds at the current frequency. Non-permanent systems
  // drop to zero afterwards, so a tyre that stops sliding stops smoking.
  void advance(double dt);

 private:
  void spawn();
  uint16_t cloud_ = 0;
  bool ready_ = false, permanent_ = false;
  float frequency_ = 0;
  double carry_ = 0;
  uint32_t counter_ = 0;
  std::array<float, 3> position_{}, velocity_{}, normal_{0, 1, 0};
  std::array<float, 3> last_position_{};
  bool has_last_ = false;
  bool custom_ = false;
  ParticleDomain position_domain_, velocity_domain_;
  float rate_ = 0;
};

// Clouds by RPK id: the caller fills the config (cfg text and texture) once.
uint16_t particle_cloud_register(uint32_t key, ParticleCloudConfig config);
// 0xffff when the key was never registered.
uint16_t particle_cloud_find(uint32_t key);
const std::vector<ParticleCloudConfig> &particle_clouds();

// The shared pool, capped at kMaxParticles (the oldest give way).
constexpr size_t kMaxParticles = 6000;
// Steps every particle: the cloud's gravity and damping toward the wind,
// a little turbulence, the floor plane of its emission.
void particles_update(double dt);
// Wind in script metres per second (x, z), shared with the sky's clouds.
void particles_set_wind(float x, float z);
const std::vector<Particle> &particles();
void particles_clear();

}  // namespace slrr
