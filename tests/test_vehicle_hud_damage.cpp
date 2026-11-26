#include "standalone_test.hpp"
#include "slrr/vehicle_hud_damage.hpp"

#include <limits>

namespace {
using namespace slrr;
using Kind = VehicleHudPart::Kind;

std::array<HudDamageBlip, 8> blips() {
  return {{{'q', Kind::Body, -190, -480, -50, -130},
           {'p', Kind::Body, 40, -480, 180, -130},
           {'u', Kind::Body, -180, 50, -100, 430},
           {'v', Kind::Body, 100, 50, 180, 430},
           {'h', Kind::Body, -180, -450, 170, -140},
           {'E', Kind::Engine, -70, -420, 90, -170},
           {'d', Kind::Wheel, -190, -380, -110, -230},
           {'c', Kind::Wheel, 110, -380, 190, -230}}};
}
const HudDamageBlip outline{'A', Kind::Body, -190, -470, 190, 440};
VehicleHudPart panel(float x, float z, float condition = 1) {
  VehicleHudPart p;
  p.quarterpanel = true;
  p.x = x;
  p.z = z;
  p.condition = condition;
  return p;
}
ScxGeometry stock_uv_mesh() {
  ScxGeometry mesh;
  mesh.uvs.resize(256 * 8);
  const float p[] = {509.f/512,54.f/256,509.f/512,1.f/256,
                     380.f/512,54.f/256,380.f/512,1.f/256};
  const float q[] = {380.f/512,54.f/256,380.f/512,1.f/256,
                     509.f/512,54.f/256,509.f/512,1.f/256};
  std::copy_n(p, 8, mesh.uvs.begin() + size_t('p') * 8);
  std::copy_n(q, 8, mesh.uvs.begin() + size_t('q') * 8);
  return mesh;
}

SLRR_TEST(damage_color_is_green_when_intact_and_red_when_destroyed) {
  SLRR_REQUIRE(hud_damage_color(1.f) == 0xff00ff00u);
  SLRR_REQUIRE(hud_damage_color(0.f) == 0xffff0000u);
  SLRR_REQUIRE(hud_damage_color(.5f) == 0xffffff00u);
  SLRR_REQUIRE(hud_damage_color(std::nanf("")) == 0xff00ff00u);
}

SLRR_TEST(fender_uses_its_own_condition_not_nearby_accessory) {
  auto b = blips();
  const auto fender = panel(-.7f, -.6f, .8f);
  auto light = fender;
  light.quarterpanel = false;
  light.condition = .05f;
  const VehicleHudPart parts[] = {fender, light};
  hud_assign_damage(b, parts, outline, true);
  SLRR_REQUIRE(b[0].present && b[0].condition == .8f);
  SLRR_REQUIRE(!b[1].present);
  // Changing iteration order does not change who owns the fender.
  const VehicleHudPart reversed[] = {light, fender};
  hud_assign_damage(b, reversed, outline, true);
  SLRR_REQUIRE(b[0].present && b[0].condition == .8f);
}
SLRR_TEST(absent_fender_is_not_resurrected_by_accessories) {
  auto b = blips();
  auto light = panel(-.7f, -.6f, .1f);
  light.quarterpanel = false;
  hud_assign_damage(b, std::span(&light, 1), outline, true);
  for (size_t i = 0; i < 4; ++i) SLRR_REQUIRE(!b[i].present);
}
SLRR_TEST(four_quarterpanels_have_independent_regions) {
  auto b = blips();
  const VehicleHudPart parts[] = {panel(-.7f, -.6f, .1f), panel(.7f, -.6f, .9f),
                                 panel(-.7f, .6f, .4f), panel(.7f, .6f, .6f)};
  hud_assign_damage(b, parts, outline, true);
  for (size_t i = 0; i < 4; ++i) {
    SLRR_REQUIRE(b[i].present);
    SLRR_REQUIRE(b[i].condition == parts[i].condition);
  }
  SLRR_REQUIRE(!b[4].present); // A quarterpanel must not color the hood.
}
SLRR_TEST(quarterpanel_does_not_switch_to_hood_at_overlapping_bounds) {
  auto b = blips();
  const auto fender = panel(-.05f, -.2f, .3f); // Inside hood, outside q's quad.
  hud_assign_damage(b, std::span(&fender, 1), outline, true);
  SLRR_REQUIRE(b[0].present && b[0].condition == .3f);
  SLRR_REQUIRE(!b[4].present);
}
SLRR_TEST(engine_and_wheel_damage_do_not_color_fender) {
  auto b = blips();
  auto engine = panel(-.2f, -.5f, .1f);
  engine.kind = Kind::Engine;
  auto wheel = engine;
  wheel.kind = Kind::Wheel;
  wheel.corner = 0;
  const VehicleHudPart parts[] = {engine, wheel, panel(-.7f, -.6f)};
  hud_assign_damage(b, parts, outline, true);
  SLRR_REQUIRE(b[0].condition == 1);
  SLRR_REQUIRE(b[5].present && b[5].condition == .1f);
  SLRR_REQUIRE(b[6].present && b[6].condition == .1f);
  SLRR_REQUIRE(!b[7].present);
}
SLRR_TEST(removal_and_repair_refresh_presence_and_color) {
  auto b = blips();
  auto p = panel(-.7f, -.6f, .2f);
  hud_assign_damage(b, std::span(&p, 1), outline, true);
  SLRR_REQUIRE(b[0].condition == .2f);
  p.condition = 1;
  hud_assign_damage(b, std::span(&p, 1), outline, true);
  SLRR_REQUIRE(b[0].condition == 1);
  hud_assign_damage(b, {}, outline, true);
  SLRR_REQUIRE(!b[0].present);
}
SLRR_TEST(left_fender_uv_is_horizontal_not_longitudinal_mirror) {
  const auto mesh = stock_uv_mesh();
  const auto before = mesh.uvs;
  std::array<float, 8> uv;
  SLRR_REQUIRE(hud_stock_left_fender_uv(mesh, uv));
  const size_t p = size_t('p') * 8;
  constexpr size_t mirror[] = {1, 0, 3, 2};
  for (size_t i = 0; i < 4; ++i)
    for (size_t axis = 0; axis < 2; ++axis)
      SLRR_REQUIRE(uv[i * 2 + axis] == mesh.uvs[p + mirror[i] * 2 + axis]);
  SLRR_REQUIRE(uv[0] == 509.f/512 && uv[1] == 1.f/256);
  SLRR_REQUIRE(uv[4] == 380.f/512 && uv[5] == 1.f/256);
  SLRR_REQUIRE(mesh.uvs == before); // Shared SCX and other HUDs are untouched.
}
SLRR_TEST(custom_and_short_font_uvs_are_not_rewritten) {
  std::array<float, 8> uv{};
  SLRR_REQUIRE(!hud_stock_left_fender_uv(ScxGeometry{}, uv));
  auto mesh = stock_uv_mesh();
  mesh.uvs[size_t('q') * 8] = .2f;
  SLRR_REQUIRE(!hud_stock_left_fender_uv(mesh, uv));
}
SLRR_TEST(custom_font_preserves_spatial_mapping_without_fender_glyphs) {
  std::array<HudDamageBlip, 1> custom{{{'h', Kind::Body, -190, -470, 190, 440}}};
  const auto p = panel(-.7f, -.6f, .4f);
  hud_assign_damage(custom, std::span(&p, 1), outline, false);
  SLRR_REQUIRE(custom[0].present && custom[0].condition == .4f);
}
SLRR_TEST(custom_fender_quads_are_not_forced_to_stock_quadrants) {
  // A custom font may place q on the right rather than use the stock layout.
  std::array<HudDamageBlip, 1> custom{{{'q', Kind::Body, 40, -480, 180, -130}}};
  const auto p = panel(.7f, -.6f, .4f);
  hud_assign_damage(custom, std::span(&p, 1), outline, false);
  SLRR_REQUIRE(custom[0].present && custom[0].condition == .4f);
}
SLRR_TEST(nonfinite_part_does_not_color_regions) {
  auto b = blips();
  auto p = panel(-.7f, -.6f);
  p.condition = std::numeric_limits<float>::quiet_NaN();
  hud_assign_damage(b, std::span(&p, 1), outline, true);
  SLRR_REQUIRE(!b[0].present);
}
SLRR_TEST(only_font_render_flags_spell_a_readout) {
  // cars.rpk 0x58 lcdfont / 0x5C sliderfont carry "flags 4.000".
  SLRR_REQUIRE(hud_render_draws_text("4.000"));
  SLRR_REQUIRE(hud_render_draws_text("1028.000"));
  // Easy Forza OSD puts its gauge plate (render 0xBAA4, "flags 1152.000",
  // osd.SCX: one textured quad, no glyphs) on osd_rpm: drawn whole, not spelled.
  SLRR_REQUIRE(!hud_render_draws_text("1152.000"));
  // Stock gauge/needle renders (0x55, 0x56) have no flags line.
  SLRR_REQUIRE(!hud_render_draws_text(""));
  SLRR_REQUIRE(!hud_render_draws_text("font"));
  SLRR_REQUIRE(!hud_render_draws_text("-4"));
}
} // namespace

int main(int argc, char** argv) {
  const slrr::standalone::Test tests[] = {
      damage_color_is_green_when_intact_and_red_when_destroyed,
      fender_uses_its_own_condition_not_nearby_accessory,
      absent_fender_is_not_resurrected_by_accessories,
      four_quarterpanels_have_independent_regions,
      quarterpanel_does_not_switch_to_hood_at_overlapping_bounds,
      engine_and_wheel_damage_do_not_color_fender,
      removal_and_repair_refresh_presence_and_color,
      left_fender_uv_is_horizontal_not_longitudinal_mirror,
      custom_and_short_font_uvs_are_not_rewritten,
      custom_font_preserves_spatial_mapping_without_fender_glyphs,
      custom_fender_quads_are_not_forced_to_stock_quadrants,
      nonfinite_part_does_not_color_regions,
      only_font_render_flags_spell_a_readout,
  };
  if (const int result = slrr::standalone::run("vehicle_hud_damage", tests)) return result;
  // Optional integration check using the user's original, unmodified SCX.
  if (argc > 1) {
    const auto mesh = slrr::load_scx_geometry(argv[1]);
    std::array<float, 8> uv;
    if (!mesh.info.ok || !slrr::hud_stock_left_fender_uv(mesh, uv)) {
      std::fprintf(stderr, "stock SCX integration check failed\n");
      return 1;
    }
    std::fprintf(stderr, "stock SCX integration check passed\n");
  }
  return 0;
}
