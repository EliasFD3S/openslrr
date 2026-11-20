// Opt-in font integration regression: bake data/game first (see README).
#include "save_io_fixture.hpp"
#include "../src/render/outline_font.hpp"
#include <cmath>
#include <cstdlib>
using namespace slrr;
namespace {
std::vector<uint8_t> bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  SLRR_REQUIRE(bool(in));
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void test_fonts() {
  const char* env = std::getenv("SLRR_GAME_ROOT");
  const fs::path source = fs::absolute(env ? env : "data/game");
  Fixture f;
  fs::create_directories(f.root / "frontend/fonts");
  for (const char* name : {"simple20", "simple40", "console10", "console5", "sl14", "sl28", "slii11", "slii17", "slii24"}) {
    Object key, alias;
    const auto ttf = source / "frontend/fonts" / (std::string(name) + ".ttf");
    fs::copy_file(ttf, f.root / "frontend/fonts" / ttf.filename());
    slrr_font_set_game_root(f.root.string());
    SLRR_REQUIRE(slrr_font_load(&key, name)); // No SCX or PNG present in this root.
    const auto* font = slrr_font_get(&key);
    SLRR_REQUIRE(font && font->distance_field && font->glyphs.size() == 256);
    SLRR_REQUIRE(slrr_font_load(&alias, name) && slrr_font_get(&alias) == font);
    // Compare all byte advances with the authored SCX, not inferred cell widths.
    const auto scx = bytes(source / "frontend/meshes/Fonts" / (std::string(name) + ".scx"));
    uint32_t header = 0; std::memcpy(&header, scx.data() + 8, 4);
    uint32_t stride = 44;
    if (header > 0x3c) std::memcpy(&stride, scx.data() + 8 + 0x3c, 4);
    for (int ch = 0; ch < 256; ++ch) {
      float advance = 0;
      std::memcpy(&advance, scx.data() + 12 + header + (ch * 4 + 1) * stride, 4);
      SLRR_REQUIRE(std::fabs(font->glyphs[ch].adv_px - std::max(0.f, advance * .1f)) < .05f);
    }
    SLRR_REQUIRE(font->glyphs['A'].u1 > font->glyphs['A'].u0);
    const auto asset = font->cache_key;
    slrr_font_forget(&key); SLRR_REQUIRE(slrr_font_asset_live(asset));
    slrr_font_forget(&alias); SLRR_REQUIRE(!slrr_font_asset_live(asset));
    std::printf("PASS %s TTF without legacy assets, byte advances and shared lifetime\n", name);
  }
  // Invalid preferred file falls back to the valid sibling format.
  const auto simple = f.root / "frontend/fonts/simple20.ttf";
  fs::rename(simple, f.root / "frontend/fonts/simple20.otf");
  { std::ofstream bad(simple, std::ios::binary); bad << "bad font"; }
  Object key;
  SLRR_REQUIRE(slrr_font_load(&key, "simple20"));
  SLRR_REQUIRE(slrr_font_get(&key)->distance_field);
  Object text;
  slrr_text_create(&text, &key, 0, 0);
  SlrrFontEffects effects; effects.outline_px = 100; effects.shadow_x = -100; effects.shadow_y = 100;
  slrr_text_set_effects(&text, effects);
  slrr_text_set_string(&text, "A"); slrr_text_update(&text);
  SLRR_REQUIRE(slrr_osd_texts().back().custom_effects);
  SLRR_REQUIRE(slrr_osd_texts().back().effects.outline_px == 2.f);
  SLRR_REQUIRE(slrr_osd_texts().back().effects.shadow_x == -3.f);
  SLRR_REQUIRE(slrr_osd_texts().back().effects.shadow_y == 3.f);
  slrr_text_destroy(&text); slrr_font_forget(&key);
  Fixture legacy;
  fs::create_directories(legacy.root / "frontend/fonts");
  fs::create_directories(legacy.root / "frontend/meshes/Fonts");
  fs::create_directories(legacy.root / "frontend/textures/Fonts");
  fs::copy_file(source / "frontend/meshes/Fonts/slii24.scx", legacy.root / "frontend/meshes/Fonts/slii24.scx");
  fs::copy_file(source / "frontend/textures/Fonts/SLII24.png", legacy.root / "frontend/textures/Fonts/SLII24.png");
  { std::ofstream bad(legacy.root / "frontend/fonts/slii24.ttf"); bad << "invalid"; }
  slrr_font_set_game_root(legacy.root.string());
  SLRR_REQUIRE(slrr_font_load(&key, "slii24"));
  SLRR_REQUIRE(!slrr_font_get(&key)->distance_field);
  slrr_font_forget(&key);
  // Standalone CFF OTF fixture is generated with fontTools for this regression.
  const auto otf = bytes("build/font-fixtures/test.otf");
  SlrrFont cff; cff.px_height = 20;
  SLRR_REQUIRE(rasterize_outline_font(otf, cff));
  SLRR_REQUIRE(cff.glyphs['A'].u1 > cff.glyphs['A'].u0);
  SLRR_REQUIRE(std::fabs(cff.glyphs['A'].adv_px - 12.f) < .001f);
  const auto& circle = cff.glyphs['O'];
  // The synthetic CFF O has cubic circles at (300,300), radii 250/125.
  // Test real coverage, including its hole and curved (not square) corner.
  stbtt_fontinfo info{}; SLRR_REQUIRE(stbtt_InitFont(&info, otf.data(), 0));
  int ascent=0, descent=0, gap=0; stbtt_GetFontVMetrics(&info,&ascent,&descent,&gap);
  const float baseline = ascent * .02f;
  auto alpha_at = [&](const SlrrFontGlyph& glyph, float x, float y) {
    const int ax = int(glyph.u0*cff.atlas_w) + int(std::floor(x-glyph.px[0]));
    const int ay = int(glyph.v0*cff.atlas_h) + int(std::floor(y-glyph.py[0]));
    SLRR_REQUIRE(ax>=0 && ax<cff.atlas_w && ay>=0 && ay<cff.atlas_h);
    return cff.rgba[(size_t(ay)*cff.atlas_w+ax)*4+3];
  };
  SLRR_REQUIRE(alpha_at(circle,6.f,baseline-6.f) < 128); // hole
  SLRR_REQUIRE(alpha_at(circle,10.f,baseline-6.f) > 128); // right ring
  SLRR_REQUIRE(alpha_at(circle,6.f,baseline-10.f) > 128); // upper ring
  SLRR_REQUIRE(alpha_at(circle,10.f,baseline-2.f) < 128); // outside the curved corner
  // B is two overlapping same-winding rectangles. Their internal edge at
  // x=4.5px must not lower SDF coverage inside the solid union.
  SLRR_REQUIRE(alpha_at(cff.glyphs['B'],4.f,baseline-8.f) > 170);
  auto truncated = otf; truncated.resize(truncated.size() / 2);
  SLRR_REQUIRE(!font_sfnt_complete(truncated));
  SLRR_REQUIRE(!font_sfnt_complete({0, 1, 0, 0}));
  std::puts("PASS effects bounds, sibling fallback, CFF OTF and truncated SFNT rejection");
}
}
int main() { const standalone::Test tests[] = {test_fonts}; return standalone::run("outline_font", tests); }
