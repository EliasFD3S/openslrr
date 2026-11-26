#include "standalone_test.hpp"

#include "slrr/particle_cloud.hpp"
#include "slrr/skid_marks.hpp"

#include <cmath>
#include <random>

namespace {

using namespace slrr;

bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) < tolerance; }

// particles/scripts/particle_clouds/asphalt.cfg, as shipped (#454).
constexpr const char *kAsphalt =
    "[STATE]\r\nsource_angle\tpoint\t0.00 0.00 0.00\r\n"
    "dest_angle\tline\t0.50 0.00 0.00 -0.50 0.00 0.00\r\n"
    "source_size\tpoint\t0.30 0.00 0.00  \r\n"
    "dest_size\tline\t2.00 0.00 0.00\t5.25 0.00 0.00\r\n"
    "source_color\tpoint\t0.90 0.90 0.95\r\n"
    "dest_color\tline\t0.25 0.25 0.30\t0.55 0.55 0.60\r\n"
    "source_alpha\tline\t0.45 0.00 0.00\t1.00 0.00 0.00\r\n"
    "dest_alpha\tpoint\t0.00 0.00 0.00\r\n"
    "lifetime\tline\t1.50 0.00 0.00\t5.00 0.00 0.00\r\n\r\n[ACTION]\r\n"
    "gravity g0\t\t0.00 2.00 0.00\r\n"
    ";bounce b0\t\t0.01 0.35 0.00\tplane\t0.00 5.00 0.00 0.00 1.00 0.00\r\n"
    "move m0\t\t\t1 1\r\ndamping d0\t\t0.01 0.01 0.01\r\n"
    "source s0 20\tpoint\t0.00 0.00 0.00\tsphere\t1.00 1.00 1.00\r\n";

// particles/scripts/particle_clouds/grass.cfg: debris, a five-number sphere.
constexpr const char *kGrass =
    "[STATE]\ntype\t\t1\nvelscale\t2.00\nsource_size\tline\t0.01 0.00 0.00\t0.04 0.00 0.00\n"
    "lifetime\tpoint\t3.00 0.00 0.00\n[ACTION]\n"
    "source s0 300\tline\t-0.10 0.00 0.00\t0.10 0.00 0.00\tsphere\t0.00 0.00 0.00 0.10 2.50\n"
    "gravity g0\t\t0.00 -9.81 0.00\nmove m0\t\t\t0 0\ndamping d0\t\t0.70 0.70 0.70\n";

SLRR_TEST(parses_stock_asphalt_cloud) {
  const auto c = parse_particle_cloud(kAsphalt);
  SLRR_REQUIRE(c.ok && c.type == 0 && near(c.rate, 20.f));
  SLRR_REQUIRE(c.dest_size.kind == ParticleDomain::Kind::Line && near(c.dest_size.v[3], 5.25f));
  SLRR_REQUIRE(near(c.source_color.v[2], .95f) && near(c.lifetime.v[3], 5.f));
  SLRR_REQUIRE(near(c.gravity[1], 2.f) && c.has_damping && near(c.damping[0], .01f));
  // The commented bounce line stays off.
  SLRR_REQUIRE(!c.bounce);
  SLRR_REQUIRE(c.position.kind == ParticleDomain::Kind::Point);
  SLRR_REQUIRE(c.velocity.kind == ParticleDomain::Kind::Sphere && c.velocity.count == 3);
}

SLRR_TEST(parses_debris_and_samples_domains) {
  const auto c = parse_particle_cloud(kGrass);
  SLRR_REQUIRE(c.ok && c.type == 1 && near(c.velscale, 2.f) && near(c.rate, 300.f));
  // move m0 0 0: physical debris moves itself (debris_lands_on_its_ground).
  SLRR_REQUIRE(!c.move_velocity && !c.move_rotation && parse_particle_cloud(kAsphalt).move_velocity);
  SLRR_REQUIRE(c.position.kind == ParticleDomain::Kind::Line && c.velocity.count == 5);
  std::mt19937 rng(1);
  for (int i = 0; i < 200; ++i) {
    const auto v = c.velocity.sample(rng);
    const float r = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    SLRR_REQUIRE(r >= .1f - 1e-4f && r <= 2.5f + 1e-4f);  // the shell, radii in either order
    const auto p = c.position.sample(rng);
    SLRR_REQUIRE(p[0] >= -.1001f && p[0] <= .1001f && p[1] == 0.f);
  }
  // A three-number sphere is an ellipsoid around the origin, not a point.
  const auto a = parse_particle_cloud(kAsphalt);
  float spread = 0;
  for (int i = 0; i < 100; ++i) spread = std::max(spread, std::fabs(a.velocity.sample(rng)[0]));
  SLRR_REQUIRE(spread > .3f && spread <= 1.f);
}

// The emitter: frequency times the cfg's rate, lifetimes, buoyant smoke kept
// above its ground, the pool cleared.
SLRR_TEST(emits_rises_and_dies) {
  particles_clear();
  particles_set_wind(0, 0);
  const uint16_t cloud = particle_cloud_register(0x7fff0002u, parse_particle_cloud(kAsphalt));
  ParticleSystem system;
  system.init(cloud);
  system.set_source({0, 0, 0}, {0, 0, 0}, {0, 1, 0});
  system.set_freq(1);
  system.advance(1.0);
  SLRR_REQUIRE(particles().size() == 20 && system.counter() == 20);
  // Not permanent: the frequency drops after each frame.
  system.advance(1.0);
  SLRR_REQUIRE(particles().size() == 20);
  for (int i = 0; i < 10; ++i) particles_update(.1);
  for (const auto &p : particles()) {
    SLRR_REQUIRE(p.position[1] > 0.f);  // gravity 2 m/s^2 upward, damped
    SLRR_REQUIRE(p.life >= 1.5f && p.life <= 5.f);
  }
  for (int i = 0; i < 60; ++i) particles_update(.1);
  SLRR_REQUIRE(particles().empty());
  system.emit_direct(5);
  SLRR_REQUIRE(particles().size() == 5);
  particles_clear();
  SLRR_REQUIRE(particles().empty());
}

SLRR_TEST(debris_lands_on_its_ground) {
  particles_clear();
  const uint16_t cloud = particle_cloud_register(0x7fff0102u, parse_particle_cloud(kGrass));
  ParticleSystem system;
  system.init(cloud);
  system.set_source({0, 1, 0}, {0, 0, 0}, {0, 1, 0});
  system.emit_direct(50);
  for (int i = 0; i < 25; ++i) particles_update(.1);
  for (const auto &p : particles()) SLRR_REQUIRE(p.position[1] >= 1.f - 1e-3f);
  particles_clear();
}

// MW_Mod/scripts/Skid_Grass.cfg and the stock sliding asphalt cfg.
SLRR_TEST(parses_skid_configs) {
  const auto grass = parse_skid_mark_config(
      "maxcount\t100\ntexture\t\t0\nwidth\t\t0.100 2.000\nheight\t\t0.700\n"
      "uv_end\t\t0.195 0.005\t0.313 0.900\nuv_normal\t0.691 0.000\t0.809 1.000\nfade\t\t0.200\n");
  SLRR_REQUIRE(grass.maxcount == 100 && near(grass.width_min, .1f) && near(grass.width_max, 2.f));
  SLRR_REQUIRE(near(grass.height, .7f) && near(grass.fade, .2f));
  SLRR_REQUIRE(near(grass.uv_end[0], .195f) && near(grass.uv_end[3], .9f));
  SLRR_REQUIRE(near(grass.uv_normal[2], .809f));
}

// A textured mark: the first tile is the end cap, the rest repeat uv_normal,
// quads split at the tile boundaries and the finish mirrors the cap.
SLRR_TEST(textured_mark_tiles) {
  skid_marks_clear();
  auto style = parse_skid_mark_config(
      "width 0.3 0.4\nheight 0.3\nuv_end 0.125 0 0.375 1\nuv_normal 0.625 0 0.875 1\nfade 0.1\n");
  style.texture = "asphalt.png";
  const uint16_t index = skid_mark_style_register(0x7fff000au, style);
  SLRR_REQUIRE(index > 0 && skid_mark_styles()[index].texture == "asphalt.png");
  SkidWheelSample s;
  s.contact = true;
  s.force = 3000;
  s.slip_speed = 8;
  s.slip_ratio = 1;
  s.style = index;
  for (int i = 0; i <= 10; ++i) {
    s.point = {0, 0, i * .1};
    skid_marks_update(1, 0, s, .016, 5);
  }
  const auto &v = skid_mark_vertices();
  SLRR_REQUIRE(v.size() >= 16);
  bool cap = false, repeat = false;
  for (size_t q = 0; q < v.size() / 4; ++q) {
    const auto *quad = &v[q * 4];
    SLRR_REQUIRE(quad[0].style == index);
    // No quad crosses a tile: both ends lie in one uv region.
    const bool in_cap = near(quad[0].u, .125f) && near(quad[1].u, .375f);
    const bool in_repeat = near(quad[0].u, .625f) && near(quad[1].u, .875f);
    SLRR_REQUIRE(in_cap || in_repeat);
    cap = cap || in_cap;
    repeat = repeat || in_repeat;
    // The full scrub gives the cfg's widest mark.
    const float dx = quad[1].x - quad[0].x;
    SLRR_REQUIRE(near(std::fabs(dx), .4f, 1e-3f));
  }
  SLRR_REQUIRE(cap && repeat);
  skid_marks_forget(1);
  // The last tile now shows the cap, its faint end at the track's end.
  const auto *last = &skid_mark_vertices()[skid_mark_vertices().size() - 4];
  SLRR_REQUIRE(near(last[0].u, .125f) && near(last[1].u, .375f));
  SLRR_REQUIRE(last[2].v < last[0].v && near(last[2].v, 0.f, 1e-3f));
  skid_marks_clear();
}

// maxcount (#454): each style keeps its own ring of maxcount x 14 quads, so
// one ground's marks never erase another's.
SLRR_TEST(style_rings_follow_maxcount) {
  skid_marks_clear();
  auto small = parse_skid_mark_config("maxcount 1\nheight 0.3\nfade 0\n");
  small.texture = "small.png";
  const uint16_t a = skid_mark_style_register(0x7fff0301u, small);
  const uint16_t b = skid_mark_style_register(0x7fff0302u, parse_skid_mark_config("maxcount 100\n"));
  SLRR_REQUIRE(skid_mark_capacity(skid_mark_styles()[a]) == 14);
  SkidWheelSample s;
  s.contact = true;
  s.force = 3000;
  s.slip_speed = 8;
  s.slip_ratio = 1;
  s.style = b;
  for (int i = 0; i <= 10; ++i) {
    s.point = {5, 0, i * .1};
    skid_marks_update(3, 0, s, .016, 5);
  }
  s.style = a;
  for (int i = 0; i <= 100; ++i) {
    s.point = {0, 0, i * .1};
    skid_marks_update(3, 1, s, .016, 5);
  }
  size_t count_a = 0, count_b = 0;
  const auto &v = skid_mark_vertices();
  for (size_t q = 0; q < v.size() / 4; ++q) (v[q * 4].style == a ? count_a : count_b) += 1;
  SLRR_REQUIRE(count_a == 14 && count_b >= 4);
  skid_marks_clear();
}

// The script natives' domains replace the cfg's source; a zero ground normal
// (a scripted effect) has no floor.
SLRR_TEST(script_domains_without_floor) {
  particles_clear();
  particles_set_wind(0, 0);
  const uint16_t cloud = particle_cloud_register(0x7fff0024u, parse_particle_cloud(kGrass));
  ParticleSystem system;
  system.init(cloud);
  system.mode_permanent(true);
  ParticleDomain point_domain, velocity;
  point_domain.kind = ParticleDomain::Kind::Sphere;
  point_domain.v = {0, 0, 0, 0, 0};
  point_domain.count = 5;
  velocity = point_domain;
  velocity.v = {0, 4, 0, 0, 0};
  system.set_domains(point_domain, velocity, 10.f);
  system.set_source({0, 10, 0}, {0, 0, 0}, {0, 0, 0});
  system.set_freq(1);
  system.advance(1.0);
  SLRR_REQUIRE(particles().size() == 10);
  for (const auto &p : particles()) SLRR_REQUIRE(near(p.velocity[1], 8.f) && near(p.position[1], 10.f));  // velscale 2
  // Permanent: it keeps its frequency.
  system.advance(1.0);
  SLRR_REQUIRE(particles().size() == 20);
  for (int i = 0; i < 20; ++i) particles_update(.1);
  bool below = false;
  for (const auto &p : particles()) below = below || p.position[1] < 9.f;
  SLRR_REQUIRE(below);
  particles_clear();
}

// The exe's combined slip and effect laws (#454): a standing burnout
// saturates the relaxed slip at 1; a 22 degree drift without wheelspin
// stays under the 0.4 gate, a little wheelspin crosses it.
SLRR_TEST(original_effect_law) {
  SLRR_REQUIRE(near(static_cast<float>(tyre_combined_slip(0, 0, 10)), 0.f));
  SLRR_REQUIRE(near(static_cast<float>(tyre_combined_slip(20, 0, 0)), 1.f));
  const double drift = tyre_combined_slip(0, 8, 20);
  SLRR_REQUIRE(drift > .36 && drift < .38);
  SLRR_REQUIRE(tyre_combined_slip(4, 8, 20) > kSkidEffectSlip);
  SLRR_REQUIRE(tyre_effect_frequency(.3) == 0 && tyre_effect_frequency(1) == 1);
  SLRR_REQUIRE(near(static_cast<float>(tyre_effect_frequency(10)), 1.92f));
  SLRR_REQUIRE(near(static_cast<float>(tyre_effect_intensity(1)), .6f));
  SkidWheelSample s;
  s.contact = true;
  s.force = 3000;
  s.sigma = .39;
  SLRR_REQUIRE(skid_scrub(s) == 0);
  s.sigma = .5;
  SLRR_REQUIRE(skid_scrub(s) > 0);
}

// Placed emission (emit_tyre_dust): due() counts the cloud's rate times the
// frequency, emit() keeps the position, the birth width floor and a swirl
// that turns a young particle about the axle.
SLRR_TEST(placed_emission_swirls) {
  particles_clear();
  particles_set_wind(0, 0);
  const uint16_t cloud = particle_cloud_register(0x7fff0402u, parse_particle_cloud(kAsphalt));
  ParticleSystem system;
  system.init(cloud);
  system.set_freq(2);
  SLRR_REQUIRE(system.due(.5) == 20);  // 20/s x 2 x 0.5 s
  SLRR_REQUIRE(system.due(.5) == 0);   // not permanent: the frequency drops
  ParticleSpawn spawn;
  spawn.position = {0, .3f, -1};
  spawn.ground_normal = {0, 1, 0};
  spawn.swirl_center = {0, .3f, 0};
  spawn.swirl_axis = {1, 0, 0};
  spawn.swirl = 8;
  spawn.min_size = .5f;
  system.emit(spawn);
  SLRR_REQUIRE(particles().size() == 1 && particles()[0].size0 >= .5f);
  SLRR_REQUIRE(near(particles()[0].position[2], -1.f));
  particles_update(.05);
  // About +x at r = (0, 0, -1): the tangent cross(x, r) is +y.
  SLRR_REQUIRE(particles()[0].velocity[1] > .5f);
  particles_clear();
}

}  // namespace

int main() {
  const slrr::standalone::Test tests[] = {
      parses_stock_asphalt_cloud, parses_debris_and_samples_domains, emits_rises_and_dies,
      debris_lands_on_its_ground, parses_skid_configs, textured_mark_tiles,
      style_rings_follow_maxcount, script_domains_without_floor, original_effect_law,
      placed_emission_swirls};
  return slrr::standalone::run("tyre_effects", tests);
}
