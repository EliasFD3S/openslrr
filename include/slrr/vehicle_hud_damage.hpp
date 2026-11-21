#pragma once

#include "slrr/part_runtime.hpp"
#include "slrr/scx_mesh.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>

namespace slrr {
struct HudDamageBlip {
  unsigned char glyph = 0;
  VehicleHudPart::Kind kind = VehicleHudPart::Kind::Body;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool present = false;
  float condition = 1;
};

inline bool hud_fender_glyph(unsigned char glyph) {
  return glyph == 'p' || glyph == 'q' || glyph == 'u' || glyph == 'v';
}

// Fender identity comes from the installed script type and its closed/rest
// location, never from which small rectangle happens to contain an accessory.
inline unsigned char hud_fender_glyph(const VehicleHudPart& part) {
  if (part.kind != VehicleHudPart::Kind::Body || !part.quarterpanel ||
      !std::isfinite(part.x) || !std::isfinite(part.z)) return 0;
  return part.z < 0 ? (part.x < 0 ? 'q' : 'p') : (part.x < 0 ? 'u' : 'v');
}

inline void hud_assign_damage(std::span<HudDamageBlip> blips,
                              std::span<const VehicleHudPart> parts,
                              const HudDamageBlip& outline, bool stock_fenders) {
  using Kind = VehicleHudPart::Kind;
  const float centre_x = .5f * (outline.x0 + outline.x1);
  const float centre_y = .5f * (outline.y0 + outline.y1);
  for (auto& blip : blips) {
    blip.present = false;
    blip.condition = 1;
  }
  for (const auto& part : parts) {
    if (!std::isfinite(part.condition) || !std::isfinite(part.x) ||
        !std::isfinite(part.z)) continue;
    const float gx = outline.x0 + (part.x + 1.f) * .5f * (outline.x1 - outline.x0);
    const float gy = outline.y0 + (part.z + 1.f) * .5f * (outline.y1 - outline.y0);
    HudDamageBlip* best = nullptr;
    float best_area = std::numeric_limits<float>::max();
    for (auto& blip : blips) {
      if (blip.kind != part.kind) continue;
      if (part.kind == Kind::Wheel) {
        const bool left = .5f * (blip.x0 + blip.x1) < centre_x;
        const bool front = .5f * (blip.y0 + blip.y1) < centre_y;
        if ((front ? 0 : 2) + (left ? 0 : 1) == part.corner) best = &blip;
        continue;
      }
      if (stock_fenders && part.kind == Kind::Body) {
        if (part.quarterpanel) {
          if (blip.glyph == hud_fender_glyph(part)) best = &blip;
          continue;
        }
        // Lights, mirrors, bumpers, etc. must not claim an absent fender or
        // override an installed quarterpanel's own wear * tear.
        if (hud_fender_glyph(blip.glyph)) continue;
      }
      const float area = (blip.x1 - blip.x0) * (blip.y1 - blip.y0);
      if (gx >= blip.x0 && gx <= blip.x1 && gy >= blip.y0 && gy <= blip.y1 &&
          area > 0 && area < best_area) {
        best = &blip;
        best_area = area;
      }
    }
    if (!best) continue;
    best->present = true;
    best->condition = std::min(best->condition, std::clamp(part.condition, 0.f, 1.f));
  }
}

// The stock q tile has its longitudinal axis reversed instead of being a
// horizontal mirror of p. Reuse p's UVs with left/right corners exchanged.
// Only repair the exact stock UV layout; a mod's authored layout is untouched.
// Call only for the stock cars.rpk damage font, not arbitrary CFG HUD fonts.
inline bool hud_stock_left_fender_uv(const ScxGeometry& mesh, std::array<float, 8>& uv) {
  constexpr size_t p = size_t('p') * 8, q = size_t('q') * 8;
  if (mesh.uvs.size() < q + 8) return false;
  constexpr std::array<float, 8> stock_p{
      509.f/512, 54.f/256, 509.f/512, 1.f/256,
      380.f/512, 54.f/256, 380.f/512, 1.f/256};
  constexpr std::array<float, 8> stock_q{
      380.f/512, 54.f/256, 380.f/512, 1.f/256,
      509.f/512, 54.f/256, 509.f/512, 1.f/256};
  for (size_t i = 0; i < 8; ++i)
    if (std::abs(mesh.uvs[p + i] - stock_p[i]) > .000001f ||
        std::abs(mesh.uvs[q + i] - stock_q[i]) > .000001f ||
        !std::isfinite(mesh.uvs[p + i]) || !std::isfinite(mesh.uvs[q + i])) return false;
  constexpr size_t mirrored[] = {1, 0, 3, 2};
  for (size_t i = 0; i < 4; ++i) {
    uv[i * 2] = mesh.uvs[p + mirrored[i] * 2];
    uv[i * 2 + 1] = mesh.uvs[p + mirrored[i] * 2 + 1];
  }
  return true;
}
} // namespace slrr
