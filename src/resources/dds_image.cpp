#include "slrr/dds_image.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace slrr {
namespace {

uint32_t rd_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t block_bytes(DdsFormat fmt) {
  return (fmt == DdsFormat::DXT1) ? 8u : 16u;
}

size_t mip_size(DdsFormat fmt, uint32_t w, uint32_t h) {
  if (fmt == DdsFormat::RGBA8) return size_t(w) * h * 4;
  if (fmt == DdsFormat::RGB8) return size_t(w) * h * 3;
  const size_t bw = (size_t(w) + 3) / 4;
  const size_t bh = (size_t(h) + 3) / 4;
  return bw * bh * block_bytes(fmt);
}

}  // namespace

DdsImage load_dds_file(const std::string& path) {
  DdsImage img;
  img.path = path;
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    img.error = "open failed";
    return img;
  }
  const auto sz = static_cast<size_t>(in.tellg());
  if (sz < 128) {
    img.error = "too small";
    return img;
  }
  in.seekg(0);
  std::vector<uint8_t> buf(sz);
  in.read(reinterpret_cast<char*>(buf.data()),
          static_cast<std::streamsize>(sz));
  if (!in || std::memcmp(buf.data(), "DDS ", 4) != 0) {
    img.error = "not DDS";
    return img;
  }
  // DDS_HEADER at +4, size usually 124.
  const uint32_t header_size = rd_u32(buf.data() + 4);
  if (header_size < 124) {
    img.error = "bad header size";
    return img;
  }
  const uint32_t flags = rd_u32(buf.data() + 8);
  img.height = rd_u32(buf.data() + 12);
  img.width = rd_u32(buf.data() + 16);
  if (!img.width || !img.height || img.width > UINT16_MAX || img.height > UINT16_MAX) {
    img.error = "invalid dimensions";
    return img;
  }
  img.mip_count = rd_u32(buf.data() + 28);
  if (img.mip_count == 0) img.mip_count = 1;
  (void)flags;

  // DDS_PIXELFORMAT at +76 (4+72)
  const uint8_t* pf = buf.data() + 4 + 72;
  const uint32_t pf_flags = rd_u32(pf + 4);
  const char fourcc[5] = {static_cast<char>(pf[8]), static_cast<char>(pf[9]),
                          static_cast<char>(pf[10]), static_cast<char>(pf[11]),
                          0};
  const uint32_t rgb_bits = rd_u32(pf + 12);

  constexpr uint32_t kDDPF_FOURCC = 0x4;
  constexpr uint32_t kDDPF_RGB = 0x40;
  constexpr uint32_t kDDPF_ALPHAPIXELS = 0x1;

  if (pf_flags & kDDPF_FOURCC) {
    if (std::strcmp(fourcc, "DXT1") == 0) img.format = DdsFormat::DXT1;
    else if (std::strcmp(fourcc, "DXT3") == 0) img.format = DdsFormat::DXT3;
    else if (std::strcmp(fourcc, "DXT5") == 0) img.format = DdsFormat::DXT5;
    else {
      img.error = std::string("unsupported FOURCC ") + fourcc;
      return img;
    }
  } else if (pf_flags & kDDPF_RGB) {
    if (rgb_bits == 32 && (pf_flags & kDDPF_ALPHAPIXELS))
      img.format = DdsFormat::RGBA8;
    else if (rgb_bits == 24)
      img.format = DdsFormat::RGB8;
    else {
      img.error = "unsupported RGB bitcount";
      return img;
    }
  } else {
    img.error = "unknown pixelformat";
    return img;
  }

  size_t data_off = 4 + header_size;  // typically 128
  // DX10 extended header
  if ((pf_flags & kDDPF_FOURCC) && std::strcmp(fourcc, "DX10") == 0) {
    img.error = "DX10 DDS not supported yet";
    return img;
  }
  if (data_off >= sz) {
    img.error = "no payload";
    return img;
  }
  const size_t top_size = mip_size(img.format, img.width, img.height);
  if (top_size > sz - data_off || top_size > UINT32_MAX) {
    img.error = "truncated mip0";
    return img;
  }
  img.mip0_size = static_cast<uint32_t>(top_size);
  img.data.assign(buf.begin() + static_cast<std::ptrdiff_t>(data_off), buf.end());
  size_t chain_size = 0;
  uint32_t levels = 0;
  const bool compressed = img.format == DdsFormat::DXT1 || img.format == DdsFormat::DXT3 ||
                          img.format == DdsFormat::DXT5;
  std::vector<uint8_t> tail_block;
  for (uint32_t w = img.width, h = img.height; w && h;) {
    const size_t level_size = mip_size(img.format, w, h);
    if (levels >= img.mip_count) {
      // Stock DDS chains can end just before the final BC blocks. bgfx
      // allocates either one mip or the full chain. Fill the tiny tail from
      // the coarsest authored mip so the whole authored chain can be used.
      if (!compressed || levels < 2 || w > 4 || h > 4) break;
      if (tail_block.empty()) {
        const size_t offset = chain_size - level_size;
        tail_block.assign(img.data.data() + offset, img.data.data() + chain_size);
        img.data.resize(chain_size);
      }
      img.data.insert(img.data.end(), tail_block.begin(), tail_block.end());
    } else if (level_size > img.data.size() - chain_size) {
      break;
    }
    chain_size += level_size;
    ++levels;
    if (w == 1 && h == 1) {
      if (levels > 1 && chain_size <= UINT32_MAX)
        img.complete_mip_size = static_cast<uint32_t>(chain_size);
      break;
    }
    if (w > 1) w /= 2;
    if (h > 1) h /= 2;
  }
  img.ok = img.width > 0 && img.height > 0 && !img.data.empty();
  if (!img.ok) img.error = "empty image";
  return img;
}

}  // namespace slrr
