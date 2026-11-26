#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slrr {

// A ground material (#452). In the original an SCM material is a physics
// shape record: friction at +0x50, rollres at +0x54, collcoeff (the shape's
// bounce) at +0x58, then the skid effects.
struct GroundSurface {
  // Multiplies the tyre's friction (times Config.ground_friction).
  float friction = 1.f;
  // Rolling-resistance coefficient added to the tyre's own (the drag of a
  // soft surface such as grass or sand).
  float rolling = 0.f;
  // SCM `collcoeff`: the bounce a body contact averages with the body's
  // (the original's default 0.2).
  float bounce = .2f;
  // SCM `skid_SMcolor` (ARGB) of the tyre marks; 0 is the road mark.
  uint32_t mark_color = 0;
  // SCM `skid_SFX`: the full resource id of the slide loop (0: the stock
  // surface/slide) and its volume scale (the original's default 0.6).
  uint32_t slide_sound = 0;
  float slide_volume = .6f;
};

// One map SCM material: its name and the values its block sets.
struct GroundMaterialEntry {
  std::string name;
  bool has_friction = false, has_rolling = false, has_bounce = false, has_mark_color = false;
  float friction = 1.f;   // SCM `friction`
  float rollres = 0.f;    // SCM `rollres`
  float collcoeff = .2f;  // SCM `collcoeff`
  uint32_t mark_color = 0;   // SCM `skid_SMcolor`
  uint32_t slide_sound = 0;  // SCM `skid_SFX` id as written (ground pack slots)
  float slide_volume = 1.f;  // its scale, 1 when omitted
};

// The SCM a ground native names: the text form (`materials N`, `material i
// name`, `params name`, then `friction`, `rollres`, `collcoeff`,
// `skid_SMcolor`, `skid_SFX`) or the compiled one (a count, a record size,
// then per material a texture id and a 32-byte name). Empty for anything
// else.
std::vector<GroundMaterialEntry> parse_ground_materials(const std::vector<uint8_t>& bytes);

// Values for a material the SCM leaves unset, from its material name or its
// texture's file name: grass, sand, gravel or dirt (with a mark colour);
// anything else is road.
GroundSurface ground_surface_for_name(std::string_view name);

// Each material's surface: the SCM's values where it sets them (`rollres`
// scaled to a rolling coefficient), else the name fallback from the material
// name, then from textures[i] (may be empty or shorter). Every friction is
// multiplied by `ground_friction` (Config.ground_friction, 1 when not
// positive), as the original does. `slide_sound` keeps the id as written;
// the caller resolves it against the ground's pack.
std::vector<GroundSurface> ground_surfaces(const std::vector<GroundMaterialEntry>& materials,
                                           const std::vector<std::string>& textures,
                                           float ground_friction = 1.f);

// SCM `rollres` 1 as a rolling-resistance coefficient: Top_Gear_Track's
// grass (2) becomes 0.10 and Dead_Island's sand (3) 0.15.
constexpr float kRollresCoefficient = .05f;

}  // namespace slrr
