#include "slrr/particle_cloud.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace slrr {
namespace {

using V3 = std::array<float, 3>;

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::vector<std::string> tokens(std::string_view line) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    const size_t start = i;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i > start) out.emplace_back(line.substr(start, i - start));
  }
  return out;
}

bool number(const std::string &text, float &out) {
  if (text.empty()) return false;
  char *end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || *end || !std::isfinite(value)) return false;
  out = value;
  return true;
}

std::string lower(std::string text) {
  for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// The domain at t[i] (`point`, `line`, ...), then up to its count of
// numbers. Advances i past what it read. False for an unknown kind.
bool read_domain(const std::vector<std::string> &t, size_t &i, ParticleDomain &out) {
  if (i >= t.size()) return false;
  const std::string kind = lower(t[i]);
  int max = 0;
  if (kind == "point") out.kind = ParticleDomain::Kind::Point, max = 3;
  else if (kind == "line") out.kind = ParticleDomain::Kind::Line, max = 6;
  else if (kind == "sphere") out.kind = ParticleDomain::Kind::Sphere, max = 5;
  else if (kind == "box") out.kind = ParticleDomain::Kind::Box, max = 6;
  else if (kind == "plane") out.kind = ParticleDomain::Kind::Plane, max = 6;
  else if (kind == "disc") out.kind = ParticleDomain::Kind::Disc, max = 8;
  else return false;
  ++i;
  out.v = {};
  out.count = 0;
  float value = 0;
  while (i < t.size() && out.count < max && number(t[i], value)) {
    out.v[static_cast<size_t>(out.count++)] = value;
    ++i;
  }
  return true;
}

ParticleDomain point(float x, float y = 0, float z = 0) {
  ParticleDomain d;
  d.v = {x, y, z};
  d.count = 3;
  return d;
}

float uniform(std::mt19937 &rng) { return std::uniform_real_distribution<float>(0.f, 1.f)(rng); }

V3 unit_direction(std::mt19937 &rng) {
  const float z = uniform(rng) * 2 - 1, a = uniform(rng) * 6.2831853f;
  const float r = std::sqrt(std::max(0.f, 1 - z * z));
  return {r * std::cos(a), r * std::sin(a), z};
}

std::mt19937 &random_engine() {
  static std::mt19937 rng(0x5eed454u);
  return rng;
}

std::vector<ParticleCloudConfig> g_clouds;
std::unordered_map<uint32_t, uint16_t> g_cloud_keys;
std::vector<Particle> g_particles;
size_t g_overwrite = 0;
float g_wind[2] = {0, 0};
double g_time = 0;

}  // namespace

V3 ParticleDomain::sample(std::mt19937 &rng) const {
  const auto &p = v;
  switch (kind) {
    case Kind::Point:
    case Kind::Plane:
    case Kind::Disc:
      return {p[0], p[1], p[2]};
    case Kind::Line: {
      const float t = uniform(rng);
      return {p[0] + (p[3] - p[0]) * t, p[1] + (p[4] - p[1]) * t, p[2] + (p[5] - p[2]) * t};
    }
    case Kind::Box:
      return {p[0] + (p[3] - p[0]) * uniform(rng), p[1] + (p[4] - p[1]) * uniform(rng),
              p[2] + (p[5] - p[2]) * uniform(rng)};
    case Kind::Sphere: {
      const V3 d = unit_direction(rng);
      const float r = std::cbrt(uniform(rng));
      if (count <= 3) return {d[0] * r * p[0], d[1] * r * p[1], d[2] * r * p[2]};
      const float lo = std::min(p[3], p[4]), hi = std::max(p[3], p[4]);
      const float radius = lo + (hi - lo) * r;
      return {p[0] + d[0] * radius, p[1] + d[1] * radius, p[2] + d[2] * radius};
    }
  }
  return {};
}

ParticleCloudConfig parse_particle_cloud(std::string_view text) {
  ParticleCloudConfig out;
  out.source_size = out.dest_size = point(1);
  out.source_color = out.dest_color = point(1, 1, 1);
  out.source_alpha = out.dest_alpha = point(1);
  out.lifetime = point(1);
  bool any = false;
  while (!text.empty()) {
    const size_t eol = text.find_first_of("\r\n");
    std::string_view line = text.substr(0, eol);
    text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
    if (const size_t c = line.find(';'); c != std::string_view::npos) line = line.substr(0, c);
    if (const size_t c = line.find("//"); c != std::string_view::npos) line = line.substr(0, c);
    line = trim(line);
    if (line.empty() || line.front() == '[') continue;
    const auto t = tokens(line);
    const std::string key = lower(t[0]);
    size_t i = 1;
    float value = 0;
    ParticleDomain *state = nullptr;
    if (key == "source_size") state = &out.source_size;
    else if (key == "dest_size") state = &out.dest_size;
    else if (key == "source_color") state = &out.source_color;
    else if (key == "dest_color") state = &out.dest_color;
    else if (key == "source_alpha") state = &out.source_alpha;
    else if (key == "dest_alpha") state = &out.dest_alpha;
    else if (key == "source_angle") state = &out.source_angle;
    else if (key == "dest_angle") state = &out.dest_angle;
    else if (key == "lifetime") state = &out.lifetime;
    if (state) {
      if (read_domain(t, i, *state)) any = true;
    } else if (key == "type" && t.size() > 1 && number(t[1], value)) {
      out.type = static_cast<int>(value);
    } else if (key == "velscale" && t.size() > 1 && number(t[1], value)) {
      out.velscale = value;
    } else if (key == "source" && t.size() > 2 && number(t[2], value)) {
      // source <name> <rate> <position domain> <velocity domain>
      out.rate = std::max(0.f, value);
      i = 3;
      if (read_domain(t, i, out.position)) read_domain(t, i, out.velocity);
      any = true;
    } else if ((key == "gravity" || key == "damping") && t.size() > 4) {
      V3 v{};
      bool good = true;
      for (size_t k = 0; k < 3; ++k) good = good && number(t[2 + k], v[k]);
      if (!good) continue;
      if (key == "gravity") out.gravity = v;
      else {
        for (auto &d : v) d = std::clamp(d, 0.f, 1.f);
        out.damping = v;
        out.has_damping = true;
      }
    } else if (key == "move" && t.size() > 3 && number(t[2], value)) {
      out.move_velocity = value != 0;
      out.move_rotation = !number(t[3], value) || value != 0;
    } else if (key == "bounce" && t.size() > 4) {
      out.bounce = true;
      number(t[2], out.bounce_friction);
      number(t[3], out.bounce_resilience);
    }
  }
  out.ok = any;
  return out;
}

namespace {
float mean_x(const ParticleDomain &d) {
  return d.kind == ParticleDomain::Kind::Line || d.kind == ParticleDomain::Kind::Box
             ? (d.v[0] + d.v[3]) * .5f
             : d.v[0];
}
}  // namespace

double particle_cloud_coverage(const ParticleCloudConfig &config) {
  const double size = mean_x(config.dest_size);
  return size * size * std::max(0.f, mean_x(config.lifetime));
}

// asphalt.cfg: dest_size 2-5.25 m, lifetime 1.5-5 s.
double particle_cloud_coverage_reference() { return 3.625 * 3.625 * 3.25; }

void ParticleSystem::set_source(const V3 &position, const V3 &velocity, const V3 &ground_normal) {
  if (!has_last_) last_position_ = position;
  else last_position_ = position_;
  has_last_ = true;
  position_ = position;
  velocity_ = velocity;
  normal_ = ground_normal;
}

void ParticleSystem::spawn() {
  if (cloud_ >= g_clouds.size()) return;
  const auto &c = g_clouds[cloud_];
  auto &rng = random_engine();
  Particle p;
  // Spread a frame's particles along the path the source took.
  const float along = uniform(rng);
  const V3 offset = (custom_ ? position_domain_ : c.position).sample(rng);
  V3 velocity = (custom_ ? velocity_domain_ : c.velocity).sample(rng);
  if (c.velscale > 0) for (auto &v : velocity) v *= c.velscale;
  for (size_t k = 0; k < 3; ++k) {
    p.position[k] = last_position_[k] + (position_[k] - last_position_[k]) * along + offset[k];
    p.velocity[k] = velocity_[k] + velocity[k];
    p.ground[k] = last_position_[k] + (position_[k] - last_position_[k]) * along;
  }
  p.ground_normal = normal_;
  p.color0 = c.source_color.sample(rng);
  p.color1 = c.dest_color.sample(rng);
  for (auto *colour : {&p.color0, &p.color1})
    for (auto &channel : *colour) channel = std::clamp(channel, 0.f, 1.f);
  p.size0 = std::max(0.f, c.source_size.sample(rng)[0]);
  p.size1 = std::max(0.f, c.dest_size.sample(rng)[0]);
  p.alpha0 = std::clamp(c.source_alpha.sample(rng)[0], 0.f, 1.f);
  p.alpha1 = std::clamp(c.dest_alpha.sample(rng)[0], 0.f, 1.f);
  p.angle0 = c.source_angle.sample(rng)[0];
  p.angle1 = c.dest_angle.sample(rng)[0];
  p.life = std::max(.05f, c.lifetime.sample(rng)[0]);
  p.seed = uniform(rng);
  p.cloud = cloud_;
  if (g_particles.size() < kMaxParticles) {
    g_particles.push_back(p);
  } else {
    g_overwrite = (g_overwrite + 1) % g_particles.size();
    g_particles[g_overwrite] = p;
  }
  ++counter_;
}

void ParticleSystem::emit_direct(int count) {
  for (int i = 0; i < count; ++i) spawn();
}

void ParticleSystem::advance(double dt) {
  if (!ready_ || cloud_ >= g_clouds.size() || !(dt > 0)) return;
  carry_ += static_cast<double>(custom_ ? rate_ : g_clouds[cloud_].rate) *
            std::max(0.f, frequency_) * dt;
  // A dropped frame must not flush a burst of hundreds of particles.
  carry_ = std::min(carry_, 64.0);
  while (carry_ >= 1) {
    spawn();
    carry_ -= 1;
  }
  if (!permanent_) frequency_ = 0;
  last_position_ = position_;
}

uint16_t particle_cloud_register(uint32_t key, ParticleCloudConfig config) {
  if (const auto it = g_cloud_keys.find(key); it != g_cloud_keys.end()) return it->second;
  if (g_clouds.size() >= 0xfffe) return 0xffff;
  const auto index = static_cast<uint16_t>(g_clouds.size());
  g_clouds.push_back(std::move(config));
  g_cloud_keys.emplace(key, index);
  return index;
}

uint16_t particle_cloud_find(uint32_t key) {
  const auto it = g_cloud_keys.find(key);
  return it == g_cloud_keys.end() ? 0xffff : it->second;
}

const std::vector<ParticleCloudConfig> &particle_clouds() { return g_clouds; }

void particles_set_wind(float x, float z) {
  g_wind[0] = std::isfinite(x) ? x : 0.f;
  g_wind[1] = std::isfinite(z) ? z : 0.f;
}

void particles_update(double dt) {
  if (!(dt > 0)) return;
  const float h = static_cast<float>(std::min(dt, .1));
  g_time += h;
  const float time = static_cast<float>(std::fmod(g_time, 1000.0));
  for (auto &p : g_particles) {
    p.age += h;
    if (p.cloud >= g_clouds.size()) continue;
    const auto &c = g_clouds[p.cloud];
    const V3 wind = {g_wind[0], 0.f, g_wind[1]};
    const bool cloud = c.type != 1;
    for (size_t k = 0; k < 3; ++k) {
      p.velocity[k] += c.gravity[k] * h;
      // The air carries the particle: damping relaxes it toward the wind,
      // not toward rest, so lingering smoke drifts away.
      const float keep = std::exp(-(1.f - c.damping[k]) * h);
      p.velocity[k] = wind[k] + (p.velocity[k] - wind[k]) * keep;
    }
    if (cloud) {
      // Slow eddies so a cloud of puffs rolls and spreads instead of rising
      // as a column (an OpenSLRR addition; the cfgs have no turbulence).
      const float s = p.seed * 6.2831853f;
      const auto &q = p.position;
      p.velocity[0] += std::sin(q[1] * 1.3f + time * .9f + s) * .55f * h;
      p.velocity[1] += std::sin(q[2] * 1.1f + time * .7f + s * 1.7f) * .25f * h;
      p.velocity[2] += std::cos(q[0] * 1.2f + time * 1.1f + s * .6f) * .55f * h;
    }
    if (c.move_velocity || !cloud)
      for (size_t k = 0; k < 3; ++k) p.position[k] += p.velocity[k] * h;
    // The ground plane at the emission point: a puff spreads along it, a
    // pebble lands and slides to rest.
    const float t = std::clamp(p.age / p.life, 0.f, 1.f);
    const float radius = (p.size0 + (p.size1 - p.size0) * t) * .5f;
    const float floor = cloud ? radius * .3f : .01f;
    const auto &n = p.ground_normal;
    if (n[0] == 0.f && n[1] == 0.f && n[2] == 0.f) continue;  // no floor
    const float height = (p.position[0] - p.ground[0]) * n[0] +
                         (p.position[1] - p.ground[1]) * n[1] +
                         (p.position[2] - p.ground[2]) * n[2];
    if (height < floor) {
      for (size_t k = 0; k < 3; ++k) p.position[k] += n[k] * (floor - height);
      const float vn = p.velocity[0] * n[0] + p.velocity[1] * n[1] + p.velocity[2] * n[2];
      if (vn < 0) {
        const float bounce = cloud ? 0.f : (c.bounce ? c.bounce_resilience : .25f);
        for (size_t k = 0; k < 3; ++k) p.velocity[k] -= n[k] * vn * (1 + bounce);
        if (!cloud) {
          const float grip = std::exp(-6.f * h);
          for (size_t k = 0; k < 3; ++k) p.velocity[k] *= grip;
        }
      }
    }
  }
  g_particles.erase(std::remove_if(g_particles.begin(), g_particles.end(),
                                   [](const Particle &p) { return !(p.age < p.life); }),
                    g_particles.end());
  if (g_overwrite >= g_particles.size()) g_overwrite = 0;
}

const std::vector<Particle> &particles() { return g_particles; }

void particles_clear() {
  g_particles.clear();
  g_overwrite = 0;
}

}  // namespace slrr
