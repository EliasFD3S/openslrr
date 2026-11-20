#include "standalone_test.hpp"

#include "slrr/dds_image.hpp"
#include "slrr/paint_texture.hpp"
#include "slrr/scx_mesh.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace {

using slrr::PaintImage;

PaintImage image(uint32_t w, uint32_t h, uint32_t argb) {
  PaintImage out;
  out.width = w;
  out.height = h;
  out.rgba.resize(size_t(w) * h * 4);
  slrr::paint_fill(out, argb);
  return out;
}

const uint8_t* texel(const PaintImage& img, uint32_t x, uint32_t y) {
  return &img.rgba[(size_t(y) * img.width + x) * 4];
}

// One 2 m square in the XY plane at z = 0, facing +Z, UV 0..1 across it.
// Vertex positions are SCX centimetres, as the loader leaves them.
slrr::ScxGeometry quad(bool normals = true) {
  slrr::ScxGeometry g;
  g.position_scale = .01f;
  const float p[4][2] = {{-100, -100}, {100, -100}, {100, 100}, {-100, 100}};
  const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  for (int i = 0; i < 4; ++i) {
    g.positions.insert(g.positions.end(), {p[i][0], p[i][1], 0.f});
    if (normals) g.normals.insert(g.normals.end(), {0.f, 0.f, 1.f});
    g.uvs.insert(g.uvs.end(), {uv[i][0], uv[i][1]});
  }
  g.indices = {0, 1, 2, 0, 2, 3};
  slrr::ScxGroup group;
  group.index_start = 0;
  group.index_count = 6;
  group.texture_index = 0;
  g.groups.push_back(group);
  return g;
}

}  // namespace

SLRR_TEST(png_encoding_round_trips_flat_and_detailed_images) {
  PaintImage flat = image(256, 256, 0xff1d2e3fu);
  const auto flat_png = slrr::paint_png_encode(flat);
  // The stock flat paint JPEGs are a few KiB; the flat PNG must not be raw.
  SLRR_REQUIRE(!flat_png.empty() && flat_png.size() < 8 * 1024);
  PaintImage back;
  SLRR_REQUIRE(slrr::paint_image_decode(flat_png.data(), flat_png.size(), &back));
  SLRR_REQUIRE(back.width == 256 && back.height == 256 && back.rgba == flat.rgba);

  PaintImage noisy = image(67, 33, 0);
  uint32_t state = 12345;
  for (auto& byte : noisy.rgba) {
    state = state * 1103515245u + 12345u;
    byte = static_cast<uint8_t>(state >> 16);
  }
  const auto noisy_png = slrr::paint_png_encode(noisy);
  SLRR_REQUIRE(slrr::paint_image_decode(noisy_png.data(), noisy_png.size(), &back));
  SLRR_REQUIRE(back.width == 67 && back.height == 33 && back.rgba == noisy.rgba);
}

SLRR_TEST(dxt_textures_decode_to_straight_rgba) {
  slrr::DdsImage dxt1;
  dxt1.ok = true;
  dxt1.width = dxt1.height = 4;
  dxt1.format = slrr::DdsFormat::DXT1;
  dxt1.path = "paint.dds";
  // c0 = pure red (0xf800), c1 = pure blue (0x001f); index 0 then index 1.
  dxt1.data = {0x00, 0xf8, 0x1f, 0x00, 0x04, 0x00, 0x00, 0x00};
  PaintImage out;
  SLRR_REQUIRE(slrr::paint_image_from_texture(dxt1, &out));
  SLRR_REQUIRE(texel(out, 0, 0)[0] == 255 && texel(out, 0, 0)[2] == 0 && texel(out, 0, 0)[3] == 255);
  SLRR_REQUIRE(texel(out, 1, 0)[0] == 0 && texel(out, 1, 0)[2] == 255);

  slrr::DdsImage dxt5 = dxt1;
  dxt5.format = slrr::DdsFormat::DXT5;
  // Alpha endpoints 0x40 / 0x80 with every index 0, then the DXT1 block.
  dxt5.data = {0x40, 0x80, 0, 0, 0, 0, 0, 0, 0x00, 0xf8, 0x1f, 0x00, 0, 0, 0, 0};
  SLRR_REQUIRE(slrr::paint_image_from_texture(dxt5, &out));
  SLRR_REQUIRE(texel(out, 3, 3)[3] == 0x40 && texel(out, 3, 3)[0] == 255);

  slrr::DdsImage premultiplied;
  premultiplied.ok = true;
  premultiplied.width = premultiplied.height = 1;
  premultiplied.format = slrr::DdsFormat::RGBA8;
  premultiplied.path = "brush.png";
  premultiplied.data = {64, 32, 0, 128};
  SLRR_REQUIRE(slrr::paint_image_from_texture(premultiplied, &out));
  SLRR_REQUIRE(out.rgba[0] == 128 && out.rgba[1] == 64 && out.rgba[3] == 128);
  premultiplied.path = "brush.dds";  // BGRA, straight
  SLRR_REQUIRE(slrr::paint_image_from_texture(premultiplied, &out));
  SLRR_REQUIRE(out.rgba[0] == 0 && out.rgba[2] == 64 && out.rgba[3] == 128);
}

SLRR_TEST(paint_texture_ids_temp_strokes_and_release) {
  slrr::paint_texture_clear_all();
  slrr::paint_texture_take_released();
  const uint32_t id = slrr::paint_texture_create(image(8, 8, 0xffffffffu));
  SLRR_REQUIRE((id & 0xffff0000u) == slrr::kPaintTextureHigh);
  SLRR_REQUIRE(slrr::paint_texture_exists(id) && !slrr::paint_texture_exists(id + 1));
  SLRR_REQUIRE(slrr::paint_texture_from_path(slrr::paint_texture_path(id)) == id);
  SLRR_REQUIRE(slrr::paint_texture_from_path("cars/textures/paint.png") == 0);
  const uint32_t revision = slrr::paint_texture_revision(id);

  // A temporary stroke shows over the committed pixels and is replaced, not
  // accumulated, by the next one; clear_temp restores the committed pixels.
  auto red = [](PaintImage& img) { slrr::paint_fill(img, 0xffff0000u); };
  SLRR_REQUIRE(slrr::paint_texture_draw(id, true, red));
  SLRR_REQUIRE(slrr::paint_texture_image(id)->rgba[1] == 0);
  SLRR_REQUIRE(slrr::paint_texture_committed(id)->rgba[1] == 255);
  SLRR_REQUIRE(slrr::paint_texture_has_temp(id));
  SLRR_REQUIRE(slrr::paint_texture_revision(id) > revision);
  SLRR_REQUIRE(slrr::paint_texture_clear_temp(id));
  SLRR_REQUIRE(slrr::paint_texture_image(id)->rgba[1] == 255);
  SLRR_REQUIRE(!slrr::paint_texture_clear_temp(id));
  SLRR_REQUIRE(slrr::paint_texture_draw(id, false, red));
  SLRR_REQUIRE(slrr::paint_texture_committed(id)->rgba[1] == 0);
  SLRR_REQUIRE(slrr::paint_texture_image(id)->rgba[1] == 0);

  slrr::paint_texture_release(id);
  SLRR_REQUIRE(!slrr::paint_texture_exists(id));
  const auto released = slrr::paint_texture_take_released();
  SLRR_REQUIRE(released.size() == 1 && released[0] == id);
  SLRR_REQUIRE(slrr::paint_texture_create(PaintImage{}) == 0);
}

SLRR_TEST(stamp_paints_the_facing_paint_slot_under_the_brush_only) {
  const auto geometry = quad();
  PaintImage target = image(64, 64, 0xffffffffu);
  const PaintImage brush = image(4, 4, 0xffffffffu);
  slrr::PaintStamp stamp;
  stamp.normal[2] = 1.f;
  stamp.size = 1.f;  // half of the 2 m quad
  stamp.argb = 0xff0000ffu;
  const size_t changed = slrr::paint_stamp_mesh(target, geometry, 0, brush, stamp);
  // A 1 m square over a 2 m quad covers about a quarter of the texels.
  SLRR_REQUIRE(changed > 900 && changed < 1200);
  SLRR_REQUIRE(texel(target, 32, 32)[0] == 0 && texel(target, 32, 32)[2] == 255);
  SLRR_REQUIRE(texel(target, 2, 2)[0] == 255 && texel(target, 61, 61)[0] == 255);

  // Another slot, a stamp from behind or one far off the surface paints nothing.
  PaintImage untouched = image(64, 64, 0xffffffffu);
  SLRR_REQUIRE(slrr::paint_stamp_mesh(untouched, geometry, 1, brush, stamp) == 0);
  slrr::PaintStamp behind = stamp;
  behind.normal[2] = -1.f;
  SLRR_REQUIRE(slrr::paint_stamp_mesh(untouched, geometry, 0, brush, behind) == 0);
  slrr::PaintStamp far = stamp;
  far.hit[2] = 3.f;
  SLRR_REQUIRE(slrr::paint_stamp_mesh(untouched, geometry, 0, brush, far) == 0);
}

SLRR_TEST(stamp_reach_matches_the_panels_it_would_paint) {
  const auto geometry = quad();
  slrr::PaintStamp stamp;
  stamp.normal[2] = 1.f;
  stamp.size = 1.f;
  // A stamp centred just past the quad's edge still overlaps it: the
  // neighbouring panel of a seam gets its share.
  stamp.hit[0] = 1.3f;
  SLRR_REQUIRE(slrr::paint_stamp_reaches(geometry, 0, stamp));
  PaintImage target = image(64, 64, 0xffffffffu);
  const PaintImage brush = image(4, 4, 0xffffffffu);
  SLRR_REQUIRE(slrr::paint_stamp_mesh(target, geometry, 0, brush, stamp) > 0);
  // Beyond its half width, behind, off the surface or on another slot it does not.
  slrr::PaintStamp clear = stamp;
  clear.hit[0] = 1.6f;
  SLRR_REQUIRE(!slrr::paint_stamp_reaches(geometry, 0, clear));
  slrr::PaintStamp behind = stamp;
  behind.normal[2] = -1.f;
  SLRR_REQUIRE(!slrr::paint_stamp_reaches(geometry, 0, behind));
  slrr::PaintStamp far = stamp;
  far.hit[2] = 3.f;
  SLRR_REQUIRE(!slrr::paint_stamp_reaches(geometry, 0, far));
  SLRR_REQUIRE(!slrr::paint_stamp_reaches(geometry, 1, stamp));
}

SLRR_TEST(stamp_blends_each_texel_once_and_orients_the_brush) {
  const auto geometry = quad(false);
  PaintImage target = image(32, 32, 0xff000000u);
  const PaintImage brush = image(2, 2, 0xffffffffu);
  slrr::PaintStamp stamp;
  stamp.normal[2] = 1.f;
  stamp.size = 4.f;  // covers the whole quad and both triangles' shared edge
  stamp.argb = 0x80ffffffu;
  SLRR_REQUIRE(slrr::paint_stamp_mesh(target, geometry, 0, brush, stamp) == 32 * 32);
  for (uint32_t y = 0; y < 32; ++y)
    for (uint32_t x = 0; x < 32; ++x) SLRR_REQUIRE(std::abs(texel(target, x, y)[0] - 128) <= 1);

  // Left half of the brush red, right half green. Mesh +X is texture +U
  // here, so an unflipped stamp puts red on the left of the texture.
  PaintImage split = image(2, 1, 0xffff0000u);
  split.rgba[4] = 0, split.rgba[5] = 255;
  PaintImage oriented = image(32, 32, 0xff000000u);
  stamp.size = 2.f;
  stamp.argb = 0xffffffffu;
  slrr::paint_stamp_mesh(oriented, geometry, 0, split, stamp);
  SLRR_REQUIRE(texel(oriented, 4, 16)[0] == 255 && texel(oriented, 27, 16)[1] == 255);
  stamp.flip = true;
  slrr::paint_stamp_mesh(oriented, geometry, 0, split, stamp);
  SLRR_REQUIRE(texel(oriented, 4, 16)[1] == 255 && texel(oriented, 27, 16)[0] == 255);
  stamp.flip = false;
  stamp.rotation = 3.14159265f;
  slrr::paint_stamp_mesh(oriented, geometry, 0, split, stamp);
  SLRR_REQUIRE(texel(oriented, 4, 16)[1] == 255 && texel(oriented, 27, 16)[0] == 255);
}

SLRR_TEST(stamp_rejects_invalid_coordinates_before_rasterization) {
  auto geometry = quad();
  auto target = image(32, 32, 0xff000000u);
  const auto brush = image(2, 2, 0xffffffffu);
  slrr::PaintStamp stamp;
  stamp.normal[2] = 1.f;
  stamp.size = 4.f;
  for (auto& uv : geometry.uvs) uv = 1e30f;
  SLRR_REQUIRE(slrr::paint_stamp_mesh(target, geometry, 0, brush, stamp) == 0);
  geometry = quad();
  for (auto& p : geometry.positions) p = std::numeric_limits<float>::quiet_NaN();
  SLRR_REQUIRE(slrr::paint_stamp_mesh(target, geometry, 0, brush, stamp) == 0);
}

SLRR_TEST(unused_textures_have_rebuild_grace_and_are_then_reclaimed) {
  slrr::paint_texture_clear_all();
  const auto id = slrr::paint_texture_create(image(4, 4, 0xff123456));
  const auto revision = slrr::paint_texture_revision(id);
  auto unused = [](uint32_t) { return false; };
  slrr::paint_texture_collect(unused);
  slrr::paint_texture_collect(unused);
  SLRR_REQUIRE(slrr::paint_texture_exists(id));
  slrr::paint_texture_collect([&](uint32_t v) { return v == id; });
  slrr::paint_texture_collect(unused);
  slrr::paint_texture_collect(unused);
  SLRR_REQUIRE(slrr::paint_texture_exists(id));
  slrr::paint_texture_collect(unused);
  SLRR_REQUIRE(!slrr::paint_texture_exists(id));
  const auto next = slrr::paint_texture_create(image(4, 4, 0xffabcdef));
  SLRR_REQUIRE(slrr::paint_texture_revision(next) != revision);
  slrr::paint_texture_release(next);
}

SLRR_TEST(stamp_does_not_wrap_uv_borders_and_respects_screen_up) {
  auto geometry = quad();
  auto target = image(64, 64, 0xff000000);
  const auto brush = image(2, 2, 0xffffffff);
  slrr::PaintStamp stamp;
  stamp.normal[2] = 1.f;
  stamp.size = .5f;
  stamp.hit[0] = -.95f;
  slrr::paint_stamp_mesh(target, geometry, 0, brush, stamp);
  SLRR_REQUIRE(texel(target, 0, 32)[0] == 255);
  SLRR_REQUIRE(texel(target, 63, 32)[0] == 0);
  // Rotating screen up rather than rotating the mesh does not change the
  // meaning of left/right for a split decal.
  PaintImage split = image(2, 1, 0xffff0000u);
  split.rgba[4] = 0; split.rgba[5] = 255;
  stamp.hit[0] = 0;
  stamp.size = 2;
  stamp.up[0] = 1; stamp.up[1] = 0;
  slrr::paint_stamp_mesh(target, geometry, 0, split, stamp);
  SLRR_REQUIRE(texel(target, 32, 6)[0] == 255);
  SLRR_REQUIRE(texel(target, 32, 57)[1] == 255);
}

int main() {
  const slrr::standalone::Test tests[] = {
      png_encoding_round_trips_flat_and_detailed_images,
      dxt_textures_decode_to_straight_rgba,
      paint_texture_ids_temp_strokes_and_release,
      stamp_paints_the_facing_paint_slot_under_the_brush_only,
      stamp_reach_matches_the_panels_it_would_paint,
      stamp_blends_each_texel_once_and_orients_the_brush,
      stamp_rejects_invalid_coordinates_before_rasterization,
      unused_textures_have_rebuild_grace_and_are_then_reclaimed,
      stamp_does_not_wrap_uv_borders_and_respects_screen_up,
  };
  return slrr::standalone::run("paint_texture", tests);
}
