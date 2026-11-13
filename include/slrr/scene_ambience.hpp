#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slrr {

class RpkIndex;
struct GroundMapData;

// Ambient emitters of map placements. A placement's object CFG may carry
//   sfx_loop   <sound> <radius> [from_hour=0] [to_hour=24]
//   sfx_random <sound> <radius> [min_s=1] [max_s=1] [from_hour=0] [to_hour=24]
// (StreetLegal_Redline.exe 0x465340 / 0x465410). An object keeps one loop,
// the last line wins, and at most 16 random entries.
struct AmbienceConfig {
  struct Loop {
    uint32_t sound = 0;  // CFG id, still relative to the owner pack
    float radius = 0.f;
    int32_t from = 0, to = 24 * 3600;  // seconds of the day, [from, to)
  };
  struct Random {
    uint32_t sound = 0;
    float radius = 0.f;
    float min_delay = 1.f, delay_range = 0.f;
    int32_t from = 0, to = 24 * 3600;
  };
  std::optional<Loop> loop;
  std::vector<Random> randoms;
  bool empty() const { return !loop && randoms.empty(); }
};
AmbienceConfig scene_ambience_parse(std::string_view cfg);

// [from, to) with midnight wrap, seconds of the day (0x4670a0).
bool scene_ambience_in_window(int32_t from, int32_t to, int32_t now);

// The emitter cannot start beyond 250 m outside its radius (0x550560).
bool scene_ambience_audible(float distance_squared, float radius);

// DirectSound 3D rolloff with the sound resource's mindist/maxdist, which
// the original passes to SetMinDistance/SetMaxDistance (0x55b6a0).
float scene_ambience_gain(float volume, float min_distance, float max_distance,
                          float distance);

// Registers the sfx placements of one ground shell. Positions are relative
// to the shell, which must stay in the active world for them to sound.
void scene_ambience_attach(void* shell, const RpkIndex& rpk, const GroundMapData& map);
// Once per frame with simulated seconds; uses the world camera as listener.
void scene_ambience_tick(double seconds);
void scene_ambience_pause(bool paused);
void scene_ambience_reset();

}  // namespace slrr
