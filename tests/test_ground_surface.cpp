#include "standalone_test.hpp"

#include "slrr/ground_surface.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using slrr::GroundMaterialEntry;
using slrr::GroundSurface;
using slrr::ground_surface_for_name;
using slrr::ground_surfaces;
using slrr::parse_ground_materials;

std::vector<uint8_t> bytes(const std::string& text) { return {text.begin(), text.end()}; }

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

bool road(const GroundSurface& s) { return s.friction == 1.f && s.rolling == 0.f; }

// Dead_Island's Ground.SCM, as shipped: values on material 3 only, tabs,
// CRLF and trailing comments.
SLRR_TEST(text_scm_sets_the_selected_material) {
  const auto m = parse_ground_materials(bytes(
      "materials 5\r\nmaterial 0 Rock\r\nmaterial 1 Road\r\nmaterial 2 Cactus\r\n"
      "material 3 Sand2\r\nfriction\t0.6\r\nrollres\t\t3.0\r\ncollcoeff\t15.0\r\n"
      "skid_SM\t\t0x00020047 //MW_Mod.rpk\\Grass Skid\r\n\r\nmaterial 4 Sand\r\n"
      "// This material order is differend than order in 3D Max scene\r\n"));
  SLRR_REQUIRE(m.size() == 5);
  SLRR_REQUIRE(m[3].name == "Sand2" && m[3].has_friction && m[3].has_rolling);
  SLRR_REQUIRE(near(m[3].friction, .6f) && near(m[3].rollres, 3.f));
  SLRR_REQUIRE(!m[4].has_friction && !m[4].has_rolling && m[4].name == "Sand");
  SLRR_REQUIRE(m[0].name == "Rock" && !m[0].has_friction);
}

// `params name` selects every material whose first word matches, as the
// original's %31s names do; a later `material` ends the selection.
SLRR_TEST(params_selects_by_name) {
  const auto m = parse_ground_materials(bytes(
      "materials 3\nmaterial 0 Grass\nmaterial 1 asfalt\nmaterial 2 grass\n"
      "params GRASS\nfriction 0.8\nmaterial 1 asfalt\nrollres 0.5\n"));
  SLRR_REQUIRE(m[0].has_friction && m[2].has_friction && !m[1].has_friction);
  SLRR_REQUIRE(m[1].has_rolling && !m[0].has_rolling);
}

// The compiled form (MP1.SCM): count, record size 0x24, texture id + name.
SLRR_TEST(binary_scm_gives_names) {
  std::vector<uint8_t> b(8 + 2 * 36, 0);
  b[0] = 2;
  b[4] = 36;
  std::memcpy(&b[8 + 4], "_p_traFa", 8);
  std::memcpy(&b[8 + 36 + 4], "_p_asfalt", 9);
  const auto m = parse_ground_materials(b);
  SLRR_REQUIRE(m.size() == 2 && m[0].name == "_p_traFa" && m[1].name == "_p_asfalt");
  SLRR_REQUIRE(!m[0].has_friction);
  // A truncated table is rejected rather than read past its end.
  b.resize(50);
  SLRR_REQUIRE(parse_ground_materials(b).empty());
}

SLRR_TEST(name_fallback_classes) {
  SLRR_REQUIRE(near(ground_surface_for_name("_p_traFa").friction, .65f));  // Polish grass
  SLRR_REQUIRE(near(ground_surface_for_name("maps\\textures\\area.dds").friction, .65f));  // city lawn
  SLRR_REQUIRE(road(ground_surface_for_name("szikla+fu")));  // a rock wall texture
  SLRR_REQUIRE(road(ground_surface_for_name("Area51_wall.dds")));
  SLRR_REQUIRE(near(ground_surface_for_name("maps/x/textures/Dead_Island_Sand.dds").friction, .6f));
  SLRR_REQUIRE(near(ground_surface_for_name("grvl_01_material").friction, .7f));
  SLRR_REQUIRE(road(ground_surface_for_name("_p_asfalt")));
  SLRR_REQUIRE(road(ground_surface_for_name("fuel_station")));
  SLRR_REQUIRE(road(ground_surface_for_name("Grass_Track/textures/road.dds")));
}

// SCM values win per key; the name and then the texture fill the rest.
SLRR_TEST(surfaces_combine_values_and_fallback) {
  std::vector<GroundMaterialEntry> m(4);
  m[0].name = "Sand2";
  m[0].has_friction = m[0].has_rolling = true;
  m[0].friction = .6f;
  m[0].rollres = 3.f;
  m[1].name = "Material #38";  // Top_Gear_Track's grass, friction only
  m[1].has_friction = true;
  m[1].friction = .8f;
  m[2].name = "Material #37";
  m[3].name = "Road";
  const auto s = ground_surfaces(m, {"", "", "textures/grass_01.dds", "textures/grass.dds"});
  SLRR_REQUIRE(near(s[0].friction, .6f) && near(s[0].rolling, 3.f * slrr::kRollresCoefficient));
  SLRR_REQUIRE(near(s[1].friction, .8f) && s[1].rolling == 0.f);
  SLRR_REQUIRE(near(s[2].friction, .65f) && near(s[2].rolling, .08f));
  // A material named as paved stays road whatever its texture.
  SLRR_REQUIRE(road(s[3]));
  const auto unnamed = ground_surfaces(std::vector<GroundMaterialEntry>(1), {"maps/t/fu.dds"});
  SLRR_REQUIRE(near(unnamed[0].friction, .65f));
}

// The effect keys: collcoeff (the bounce), skid_SMcolor, skid_SFX with and
// without a scale; ground_friction scales every friction, set or not.
SLRR_TEST(effects_and_ground_friction) {
  const auto m = parse_ground_materials(bytes(
      "materials 3\nmaterial 0 Sand2\nfriction 0.6\ncollcoeff 15.0\n"
      "skid_SMcolor\t0xFF737373\nskid_SFX 0x00050012 0.8\n"
      "material 1 asfalt\nskid_SFX 0x00000007\nmaterial 2 Grass\n"));
  SLRR_REQUIRE(m[0].has_bounce && near(m[0].collcoeff, 15.f));
  SLRR_REQUIRE(m[0].has_mark_color && m[0].mark_color == 0xFF737373u);
  SLRR_REQUIRE(m[0].slide_sound == 0x00050012u && near(m[0].slide_volume, .8f));
  SLRR_REQUIRE(m[1].slide_sound == 7u && near(m[1].slide_volume, 1.f));
  const auto s = ground_surfaces(m, {}, 1.5f);
  SLRR_REQUIRE(near(s[0].friction, .9f) && near(s[0].bounce, 15.f));
  SLRR_REQUIRE(s[0].mark_color == 0xFF737373u && s[0].slide_sound == 0x00050012u);
  SLRR_REQUIRE(near(s[1].friction, 1.5f) && near(s[1].bounce, .2f) && !s[1].mark_color);
  SLRR_REQUIRE(near(s[1].slide_volume, 1.f));
  // The grass fallback brings its own mark; the default sound scale stays.
  SLRR_REQUIRE(near(s[2].friction, .65f * 1.5f) && s[2].mark_color != 0);
  SLRR_REQUIRE(!s[2].slide_sound && near(s[2].slide_volume, .6f));
  // A ground_friction at or below zero is 1, as in the original.
  SLRR_REQUIRE(near(ground_surfaces(m, {}, 0.f)[1].friction, 1.f));
}

}  // namespace

int main() {
  const slrr::standalone::Test tests[] = {
      text_scm_sets_the_selected_material, params_selects_by_name, binary_scm_gives_names,
      name_fallback_classes, surfaces_combine_values_and_fallback, effects_and_ground_friction};
  return slrr::standalone::run("ground_surface", tests);
}
