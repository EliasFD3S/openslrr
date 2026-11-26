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
  // SCM `skid_PS` and `skid_SM` (#454): the full ids of the RPK entries
  // naming the particle cloud (`particle <cfg>`) and the skid mark
  // (`skidmark <cfg>`), each with its texture. 0 is the original's default,
  // particles:2 (asphalt smoke) and particles:0xA (the asphalt mark).
  // Before the ground pack resolves them, a high word of 0xffff is a local
  // id of particles.rpk (the name fallbacks use it).
  uint32_t skid_particles = 0;
  uint32_t skid_mark = 0;
};

// particles.rpk entries of the stock clouds and marks (`0xffff0000 | id`).
constexpr uint32_t kParticlesLocal = 0xffff0000u;
constexpr uint32_t kCloudAsphalt = 0x2, kCloudGrass = 0x102, kCloudPebbles = 0x103,
                   kCloudSand = 0x104;
constexpr uint32_t kSkidAsphalt = 0xA, kSkidGrass = 0x202;

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
  uint32_t skid_particles = 0;  // SCM `skid_PS` id as written (ground pack slots)
  uint32_t skid_mark = 0;       // SCM `skid_SM` id as written
};

// The SCM a ground native names: the text form (`materials N`, `material i
// name`, `params name`, then `friction`, `rollres`, `collcoeff`, `skid_PS`,
// `skid_SM`, `skid_SMcolor`, `skid_SFX`) or the compiled one (a count, a record size,
// then per material a texture id and a 32-byte name). Empty for anything
// else.
std::vector<GroundMaterialEntry> parse_ground_materials(const std::vector<uint8_t>& bytes);

// Values for a material the SCM leaves unset, from its material name or its
// texture's file name: grass, sand, gravel or dirt (each with a stock cloud
// and the stock grass mark, #454); anything else is road.
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
