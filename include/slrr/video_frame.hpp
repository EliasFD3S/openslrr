#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>
namespace slrr {
// Packed RGB32 has no extra row padding. Output is always top-left BGRA.
inline size_t video_rgb32_bytes(int32_t width, int32_t height) {
  if (width <= 0 || height == 0 || height == INT32_MIN) return 0;
  const auto rows = static_cast<size_t>(height < 0 ? -height : height);
  const auto columns = static_cast<size_t>(width);
  if (columns > static_cast<size_t>(INT32_MAX) / 4 / rows) return 0;
  return columns * rows * 4;
}
inline bool video_copy_rgb32(std::span<const uint8_t> sample, int32_t width,
                             int32_t signed_height, std::vector<uint8_t>& output) {
  const auto bytes = video_rgb32_bytes(width, signed_height);
  if (!bytes || sample.size() < bytes) return false;
  const auto rows = static_cast<size_t>(signed_height < 0 ? -signed_height : signed_height);
  const auto pitch = static_cast<size_t>(width) * 4;
  output.resize(bytes);
  for (size_t y = 0; y < rows; ++y) {
    const auto source_y = signed_height < 0 ? y : rows - 1 - y;
    std::memcpy(output.data() + y * pitch, sample.data() + source_y * pitch, pitch);
  }
  return true;
}
}  // namespace slrr
