#pragma once

#include <cstdint>
#include <string>

namespace slrr {

enum class TriggerShape : uint8_t { None = 0, Sphere = 1, Box = 2 };

// A GameRef.RID_TRIGGER volume as java.game.Trigger creates it:
//   "x,y,z,yaw,pitch,roll,sphere,r" or "x,y,z,yaw,pitch,roll,box,hx,hy,hz".
// Box sizes are read as half extents, as SLRR_rev's trigger contacts do; the
// stock scripts do not settle whether the engine halves them.
struct TriggerVolume {
  TriggerShape shape = TriggerShape::None;
  float pose[6] = {};  // position, then yaw/pitch/roll in radians
  float size[3] = {};  // sphere radius in size[0], or box half extents
};

// False, leaving output unchanged, for any other params string.
bool trigger_volume_parse(const std::string& params, TriggerVolume& output);

// A point in the trigger's own frame (relative to its pose).
bool trigger_volume_contains(TriggerShape shape, const float size[3], const float local[3]);

// Whether one car is inside one trigger, as last reported to scripts.
struct TriggerContact {
  bool known = false;
  bool inside = false;
};
enum class TriggerEdge : int8_t { None = 0, Enter = 1, Leave = -1 };
// Records this frame's containment and returns the event to raise. A car's
// first sighting and a teleport only record the state: a car that appears in
// a volume never "entered" it.
TriggerEdge trigger_contact_update(TriggerContact& contact, bool inside, bool teleported);

}  // namespace slrr
