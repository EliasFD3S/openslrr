#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace slrr {

struct DdsImage;
struct ScxGeometry;

// Straight (not premultiplied) RGBA8, rows top-down: pixel (x, y) is the
// texel a mesh samples at (u, v) = ((x + .5) / width, (y + .5) / height).
struct PaintImage {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  bool valid() const {
    return width && height && width <= 4096 && height <= 4096 &&
           rgba.size() == size_t(width) * height * 4;
  }
};

// Host paint textures: the textures Painter creates on a part. The stock
// engine gives such a runtime texture a resource id outside every pack and
// saves it beside the save file; File.write(ResourceRef) already wires ids
// with the high word 0xFFFF as unsaved type-7 blobs, so these ids use it.
// Each texture keeps the committed pixels and the shown pixels; a temporary
// stroke (the decal preview that follows the cursor) changes only the shown
// copy until the next temporary stroke, a committed stroke or clear_temp.
constexpr uint32_t kPaintTextureHigh = 0xFFFF0000u;
bool paint_texture_exists(uint32_t id);
// 0 when the image is invalid or every id is in use.
uint32_t paint_texture_create(PaintImage image);
const PaintImage* paint_texture_image(uint32_t id);  // shown pixels
const PaintImage* paint_texture_committed(uint32_t id);
uint32_t paint_texture_revision(uint32_t id);
// Draw one stroke. temp draws over a copy of the committed pixels.
bool paint_texture_draw(uint32_t id, bool temp,
                        const std::function<void(PaintImage&)>& draw);
// Drops a temporary stroke. True when the shown pixels changed.
bool paint_texture_clear_temp(uint32_t id);
bool paint_texture_has_temp(uint32_t id);
void paint_texture_release(uint32_t id);
// Reclaim textures unused by parts, resource wrappers or save readers for three host
// frames. The grace period preserves ids through a synchronous part rebuild.
void paint_texture_collect(const std::function<bool(uint32_t)>& used);
// Ids released since the last call, for GPU handles to follow.
std::vector<uint32_t> paint_texture_take_released();
void paint_texture_clear_all();
size_t paint_texture_count();
// "paint:ffff0001" for the renderer's texture path cache. 0 when the path is
// not one of these.
std::string paint_texture_path(uint32_t id);
uint32_t paint_texture_from_path(const std::string& path);

// Decoded DXT1/3/5, BGRA/BGR DDS or an RGBA image from load_texture_file.
// Images that load_texture_file premultiplied are restored to straight alpha.
bool paint_image_from_texture(const DdsImage& texture, PaintImage* out);

// PNG (RGBA8, deflate with fixed Huffman codes) for the save trailer.
std::vector<uint8_t> paint_png_encode(const PaintImage& image);
// PNG or JPEG bytes; JPEG is what the stock game writes beside its saves.
bool paint_image_decode(const uint8_t* data, size_t size, PaintImage* out);

void paint_fill(PaintImage& image, uint32_t argb);

// Projected decal or spray stamp. hit and normal are in the mesh's scaled
// local frame (metres, before the render Z reflection); up is screen up in
// that frame. The brush square is
// size metres wide, turned by rot radians about the normal and mirrored by
// flip. Only triangles of the paint slot that face the stamp are painted;
// each covered texel blends brush RGB x color RGB at brush alpha x color
// alpha. Returns the number of texels changed.
struct PaintStamp {
  float hit[3]{}, normal[3]{};
  float up[3] = {0.f, 1.f, 0.f};
  float size = 0.7f, rotation = 0.f;
  bool flip = false;
  uint32_t argb = 0xffffffffu;
};
size_t paint_stamp_mesh(PaintImage& target, const ScxGeometry& geometry,
                        int paint_slot, const PaintImage& brush,
                        const PaintStamp& stamp);
// Whether that stamp would reach any paint-slot triangle of the mesh (the
// same culling, no rasterizing): neighbouring panels only take a texture
// when a stamp across their seam actually covers them.
bool paint_stamp_reaches(const ScxGeometry& geometry, int paint_slot,
                         const PaintStamp& stamp);

}  // namespace slrr
