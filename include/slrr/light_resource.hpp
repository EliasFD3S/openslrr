#pragma once

#include "slrr/scene_sun.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace slrr {
class RpkIndex;

// Stock light resources: RPK type 13 (RESTYPE_RENDER_LIGHT), text payloads
// such as objects.rpk 0x1C7 "street" (#237). Survey in KNOWLEDGE_BASE
// "Stock light resources and placement (#235)".
enum class LightResourceType : uint8_t {
  None,  // base class without a `type` line
  Directional,
  Point,
  Spot,
  FakePoint,  // `fakepoint` or `fake point`
  FakeSpot,
  Unknown,
};

struct LightResource {
  LightResourceType type = LightResourceType::None;
  // `diffuse a r g b` and friends keep r g b; the leading alpha is dropped.
  std::array<float, 3> diffuse{1.f, 1.f, 1.f}, specular{}, ambient{};
  std::array<float, 3> position{}, direction{};
  float range = 0.f;
  // D3D attenuation: 1 / (c + l*d + q*d*d).
  std::array<float, 3> attenuation{1.f, 0.f, 0.f};
  float theta = 0.f, phi = 0.f;  // spot cone angles, radians (full angles)
  bool lit_dynamic = false;
  bool has_flare = false;  // any lf_* keyword
  // Shared stock flare atlas/color/glow sizes; parse keeps an embedded texture
  // id, load_light_resource resolves it in the light's owning RPK.
  SceneFlare flare;
  // Optional filename on lf_texture. Stock particles:1F names an empty
  // type-14 id, so retain its explicit authored artwork as fallback. load
  // resolves this game-relative path against the resource root.
  std::string flare_texture_path;
  float glow_offset = 0.f;  // authored centimetres toward the camera
  uint32_t flare_texture_mode = 0;  // 2 = atlas, upper-left quadrant is glow
};

// Point and spot light the scene. fake* types are glow/flare only and never
// add scene lighting (not confirmed against the original engine).
inline bool light_resource_lights_scene(LightResourceType type) {
  return type == LightResourceType::Point || type == LightResourceType::Spot;
}

// Parses a type-13 payload. Inline `//` and `;` comments are ignored, as are
// unknown keywords and malformed values (the previous value stays). Returns
// nullopt only when no light keyword is recognised at all.
std::optional<LightResource> parse_light_resource(std::string_view text);

// Reads `full_id` and parses it when the RPK entry's typeof is 13.
std::optional<LightResource> load_light_resource(const RpkIndex &rpk,
                                                 uint32_t full_id);

// True when the RPK entry for `full_id` is a light resource (typeof 13).
bool is_light_resource(const RpkIndex &rpk, uint32_t full_id);
} // namespace slrr
