#pragma once
#include "slrr/slrr_font.hpp"
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace slrr {
struct OsdTextVertex { float x, y, z, u, v; };
// Keep every batch below 65536 vertices with local 16-bit indices.
constexpr uint32_t kOsdTextBatchGlyphs = 16383;
inline uint32_t osd_text_capacity(uint32_t vertices, uint32_t indices) {
  return std::min({vertices / 4, indices / 6, kOsdTextBatchGlyphs});
}

// Byte-oriented charset and authored corner order match the legacy text path.
// A cursor continues the pen across chunks; spaces consume advance, not quads.
class OsdTextRun {
 public:
  OsdTextRun(const SlrrFont& font, std::string_view text, float x, float y,
             float screen_w, float screen_h, int align, float measured_width)
      : font_(font), text_(text), x_(x), y_(y),
        sx_(2.f / screen_w), sy_(2.f / screen_h) {
    if (align == 1) x_ -= measured_width * sx_ * .5f;
    else if (align == 0) x_ -= measured_width * sx_;
  }
  bool next(uint32_t capacity, std::vector<OsdTextVertex>& vertices,
            std::vector<uint16_t>& indices) {
    vertices.clear(); indices.clear();
    capacity = std::min(capacity, kOsdTextBatchGlyphs);
    const auto reserve = std::min<size_t>(capacity, text_.size() - offset_);
    vertices.reserve(reserve * 4); indices.reserve(reserve * 6);
    while (offset_ < text_.size() && vertices.size() / 4 < capacity) {
      const auto ch = static_cast<unsigned char>(text_[offset_++]);
      const auto* g = ch < font_.glyphs.size() ? &font_.glyphs[ch] : nullptr;
      if (g && g->u1 > g->u0 + 1e-6f && g->v1 > g->v0 + 1e-6f) {
        const auto base = static_cast<uint16_t>(vertices.size());
        for (int corner : {0, 1, 3, 2})
          vertices.push_back({x_ + g->px[corner] * sx_, y_ + g->py[corner] * sy_,
                              0.f, g->tu[corner], g->tv[corner]});
        for (int index : {0, 1, 2, 0, 2, 3}) indices.push_back(base + index);
      }
      x_ += (g && g->adv_px > 0.f ? g->adv_px : font_.px_height * .35f) * sx_;
    }
    return !vertices.empty();
  }
 private:
  const SlrrFont& font_;
  std::string_view text_;
  size_t offset_ = 0;
  float x_, y_, sx_, sy_;
};
} // namespace slrr
