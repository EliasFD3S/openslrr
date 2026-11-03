#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

enum class DdsFormat : uint8_t {
  Unknown = 0,
  DXT1,
  DXT3,
  DXT5,
  RGBA8,
  RGB8,
};

struct DdsImage {
  bool ok = false;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mip_count = 1;
  DdsFormat format = DdsFormat::Unknown;
  std::vector<uint8_t> data;  // mip0 (+ optional trailing mips packed)
  uint32_t mip0_size = 0;
  std::string path;
  std::string error;
};

DdsImage load_dds_file(const std::string& path);

}  // namespace slrr
