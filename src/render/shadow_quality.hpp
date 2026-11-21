#pragma once
#include "render_debug.hpp"
#include "slrr/host_game_options.hpp"
#include <algorithm>
#include <cstdint>
namespace slrr {
inline uint16_t local_shadow_resolution() {
  if (const int tile = render_debug().local_shadow_tile; tile > 0)
    return uint16_t(std::clamp(tile, 128, 1024));
  return uint16_t(
      256u << std::clamp(host_enhanced_graphics().shadow_quality, 0, 2));
}
inline uint16_t sun_shadow_resolution() {
  return uint16_t(
      512u << std::clamp(host_enhanced_graphics().shadow_quality, 0, 2));
}
inline uint16_t scenery_shadow_resolution() {
  if (const int size = render_debug().sun_shadow_size; size > 0)
    return uint16_t(std::clamp(size, 512, 4096));
  return host_enhanced_graphics().shadow_quality == 0 ? 1024 : 2048;
}
} // namespace slrr
