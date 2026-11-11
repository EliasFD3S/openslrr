#include "slrr/ptx_image.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#if defined(SLRR_HAS_LIBBGFX)
#define STB_IMAGE_IMPLEMENTATION
// JPEG (PTX mips) + PNG (SLII* font atlases). No ONLY_* — both needed.
#define STBI_NO_STDIO
#include <stb/stb_image.h>
#endif

namespace slrr {
namespace {

// Stock PTX (street-legal slr-fmt-texture / exe 0x507110):
//   u32 0, u32 mip_count, u32 width, u32 height
//   then per mip (smallest-first):
//     u32 total, u32 rgb_size, u32 alpha_size, u32 0, f32 level
//     u8[total] = JPEG rgb [| JPEG alpha]
#pragma pack(push, 1)
struct PtxFileHdr {
  uint32_t zero;
  uint32_t mip_count;
  uint32_t width;
  uint32_t height;
};
struct PtxMipHdr {
  uint32_t total;
  uint32_t rgb_size;
  uint32_t alpha_size;
  uint32_t pad;
  float level;
};
#pragma pack(pop)

struct PtxJpegLevel {
  const uint8_t* rgb = nullptr;
  size_t rgb_size = 0;
  const uint8_t* alpha = nullptr;
  size_t alpha_size = 0;
};

bool parse_ptx_levels(const uint8_t* data, size_t size, uint32_t* w_out,
                      uint32_t* h_out, std::vector<PtxJpegLevel>* levels) {
  if (!data || !levels || size < sizeof(PtxFileHdr) + sizeof(PtxMipHdr) + 4)
    return false;
  PtxFileHdr hdr{};
  std::memcpy(&hdr, data, sizeof(hdr));
  if (hdr.zero != 0 || hdr.mip_count == 0 || hdr.mip_count > 32 ||
      hdr.width == 0 || hdr.height == 0)
    return false;
  if (w_out) *w_out = hdr.width;
  if (h_out) *h_out = hdr.height;

  size_t off = sizeof(PtxFileHdr);
  levels->clear();
  levels->reserve(hdr.mip_count);
  for (uint32_t i = 0; i < hdr.mip_count; ++i) {
    if (off > size || sizeof(PtxMipHdr) > size - off) return false;
    PtxMipHdr mip{};
    std::memcpy(&mip, data + off, sizeof(mip));
    off += sizeof(mip);

    const size_t rgb_size = static_cast<size_t>(mip.rgb_size);
    const size_t alpha_size = static_cast<size_t>(mip.alpha_size);
    if (mip.pad != 0 || rgb_size < 2 || alpha_size == 1 ||
        alpha_size > std::numeric_limits<size_t>::max() - rgb_size)
      return false;
    const size_t total = rgb_size + alpha_size;
    if (total > std::numeric_limits<uint32_t>::max() || mip.total != total ||
        off > size || total > size - off)
      return false;

    // Both JPEG markers and the alpha offset are inside the checked payload.
    const uint8_t* payload = data + off;
    if (payload[0] != 0xff || payload[1] != 0xd8) return false;
    PtxJpegLevel lv;
    lv.rgb = payload;
    lv.rgb_size = rgb_size;
    if (alpha_size > 0) {
      const uint8_t* alpha = payload + rgb_size;
      if (alpha[0] != 0xff || alpha[1] != 0xd8) return false;
      lv.alpha = alpha;
      lv.alpha_size = alpha_size;
    }
    levels->push_back(lv);
    off += total;
  }
  return !levels->empty();
}

std::string lower_ext(std::string p) {
  for (char& c : p)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto dot = p.find_last_of('.');
  return dot == std::string::npos ? std::string{} : p.substr(dot);
}

}  // namespace

DdsImage load_ptx_file(const std::string& path) {
  DdsImage out;
  out.path = path;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    out.error = "open failed";
    return out;
  }
  in.seekg(0, std::ios::end);
  const auto sz = static_cast<size_t>(in.tellg());
  in.seekg(0, std::ios::beg);
  if (sz < sizeof(PtxFileHdr) + sizeof(PtxMipHdr) + 4) {
    out.error = "too small";
    return out;
  }
  std::vector<uint8_t> file(sz);
  in.read(reinterpret_cast<char*>(file.data()),
          static_cast<std::streamsize>(sz));
  if (!in) {
    out.error = "read failed";
    return out;
  }

  uint32_t pw = 0, ph = 0;
  std::vector<PtxJpegLevel> levels;
  if (!parse_ptx_levels(file.data(), file.size(), &pw, &ph, &levels)) {
    out.error = "ptx parse failed";
    return out;
  }
  // Mips are smallest-first; last level is full resolution.
  const PtxJpegLevel& lv = levels.back();

#if defined(SLRR_HAS_LIBBGFX)
  int w = 0, h = 0, comp = 0;
  stbi_uc* rgba = stbi_load_from_memory(lv.rgb, static_cast<int>(lv.rgb_size),
                                        &w, &h, &comp, 4);
  if (!rgba || w <= 0 || h <= 0) {
    out.error = "jpeg rgb decode failed";
    if (rgba) stbi_image_free(rgba);
    return out;
  }

  if (lv.alpha && lv.alpha_size > 0) {
    int aw = 0, ah = 0, ac = 0;
    stbi_uc* achan = stbi_load_from_memory(
        lv.alpha, static_cast<int>(lv.alpha_size), &aw, &ah, &ac, 1);
    if (achan && aw == w && ah == h) {
      const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
      for (size_t i = 0; i < n; ++i) rgba[i * 4 + 3] = achan[i];
    }
    if (achan) stbi_image_free(achan);
  }

  // Premultiply RGB by alpha so opaque JPEG fringes (green/gray plate under
  // white glyphs) do not bleed when alpha-blended over the garage.
  {
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < n; ++i) {
      const uint8_t a = rgba[i * 4 + 3];
      rgba[i * 4 + 0] =
          static_cast<uint8_t>((rgba[i * 4 + 0] * a + 127) / 255);
      rgba[i * 4 + 1] =
          static_cast<uint8_t>((rgba[i * 4 + 1] * a + 127) / 255);
      rgba[i * 4 + 2] =
          static_cast<uint8_t>((rgba[i * 4 + 2] * a + 127) / 255);
    }
  }

  out.width = static_cast<uint32_t>(w);
  out.height = static_cast<uint32_t>(h);
  out.format = DdsFormat::RGBA8;
  out.mip0_size = static_cast<uint32_t>(w * h * 4);
  out.data.assign(rgba, rgba + out.mip0_size);
  stbi_image_free(rgba);
  out.ok = true;
#else
  (void)lv;
  out.width = pw;
  out.height = ph;
  out.error = "ptx needs SLRR_HAS_LIBBGFX (stb_image)";
#endif
  return out;
}

DdsImage load_rgba_file(const std::string& path) {
  DdsImage out;
  out.path = path;
#if defined(SLRR_HAS_LIBBGFX)
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    out.error = "open failed";
    return out;
  }
  const auto sz = static_cast<size_t>(in.tellg());
  in.seekg(0);
  std::vector<uint8_t> buf(sz);
  in.read(reinterpret_cast<char*>(buf.data()),
          static_cast<std::streamsize>(sz));
  if (!in) {
    out.error = "read failed";
    return out;
  }
  int w = 0, h = 0, comp = 0;
  stbi_uc* rgba = stbi_load_from_memory(buf.data(), static_cast<int>(buf.size()),
                                        &w, &h, &comp, 4);
  if (!rgba || w <= 0 || h <= 0) {
    if (rgba) stbi_image_free(rgba);
    out.error = "not an image";
    return out;
  }
  // OSD samples with the premultiplied blend used for PTX. A 32x32 MW_Mod
  // PNG (driver, jack, +1h) stores the glyph in alpha and leaves RGB white,
  // so an unmultiplied upload fills the whole button.
  {
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < n; ++i) {
      const uint8_t a = rgba[i * 4 + 3];
      rgba[i * 4 + 0] =
          static_cast<uint8_t>((rgba[i * 4 + 0] * a + 127) / 255);
      rgba[i * 4 + 1] =
          static_cast<uint8_t>((rgba[i * 4 + 1] * a + 127) / 255);
      rgba[i * 4 + 2] =
          static_cast<uint8_t>((rgba[i * 4 + 2] * a + 127) / 255);
    }
  }
  out.width = static_cast<uint32_t>(w);
  out.height = static_cast<uint32_t>(h);
  out.format = DdsFormat::RGBA8;
  out.mip0_size = static_cast<uint32_t>(w * h * 4);
  out.data.assign(rgba, rgba + out.mip0_size);
  stbi_image_free(rgba);
  out.ok = true;
#else
  out.error = "not DDS";
#endif
  return out;
}

DdsImage load_texture_file(const std::string& path) {
  const std::string ext = lower_ext(path);
  if (ext == ".ptx") return load_ptx_file(path);
  if (ext == ".png" || ext == ".tga" || ext == ".jpg" || ext == ".jpeg" ||
      ext == ".bmp")
    return load_rgba_file(path);
  return load_dds_file(path);
}

}  // namespace slrr
