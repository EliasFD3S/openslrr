#pragma once

#include <string>
#include <cstdint>

namespace slrr {

// Menu SFX from Frontend.java (frontend.rpk → frontend/sounds/*.wav).
enum class HostSfx {
  MenuMove,    // 0x0079 mnumove.wav
  MenuSelect,  // 0x007A mnuselect.wav
  MenuBack,    // 0x0078 mnuback.wav
};

void host_sfx_set_game_root(const std::string& game_root);
void host_sfx_play(HostSfx id);
int32_t host_sfx_play_file(const std::string& path, float pitch=1.f, float volume=1.f, bool loop=false, int32_t instance=0);
void host_sfx_stop(int32_t instance);
void host_sfx_pause_loop(int32_t instance, bool paused);
void host_sfx_update_loop(int32_t instance, float pitch, float volume);
// Stereo placement of a loop: -1 left, 0 centre, 1 right. A non-finite value is centre.
void host_sfx_set_loop_pan(int32_t instance, float pan);
bool host_sfx_is_playing(int32_t instance);
void host_sfx_set_volume(float volume);

// Spatial audio (#390). The listener is the world camera; a placed voice is
// panned by its direction in listener space, attenuated by its distance
// (inverse model, as SLRR_rev's Web Audio panners) and pitched by the
// Doppler factor of the relative velocity. Placed voices form the "world"
// group that the environment reverb processes; unplaced voices (menus,
// the cockpit) stay dry and centred.
struct HostSfxPlace {
  float position[3]{};
  float velocity[3]{};     // m/s, script space; zero when unknown
  float reference = 2.5f;  // full gain up to this distance (m)
  float maximum = 60.f;    // silent beyond this distance (m)
  float rolloff = 1.f;
  bool attenuate = true;   // false: the caller already applies a distance gain
};
int32_t host_sfx_play_at(const std::string& path, const HostSfxPlace& place, float pitch = 1.f,
                         float volume = 1.f, bool loop = false, int32_t instance = 0);
// Moves a placed voice (or places a playing one).
void host_sfx_set_place(int32_t instance, const HostSfxPlace& place);
// Called once per frame. `valid` is false outside a 3D scene, which centres
// placed voices at full gain. `dt` is the frame time, for the listener speed.
void host_sfx_set_listener(bool valid, const float eye[3], const float forward[3],
                           const float up[3], double dt);
// Script-space right vector of a camera looking along `forward` (the
// renderer flips Z and uses a left-handed look-at, so it is forward x up).
void host_sfx_listener_right(const float forward[3], const float up[3], float right[3]);

enum class HostSfxEnvironment { None, Garage, City, Open };
struct HostSfxSpatialOptions {
  bool positioning = true;
  bool doppler = true;
  bool reverb = true;
};
HostSfxSpatialOptions& host_sfx_spatial_options();
// Re-applies the options to the playing voices after a change.
void host_sfx_apply_spatial_options();
void host_sfx_set_environment(HostSfxEnvironment environment);
HostSfxEnvironment host_sfx_environment();
// Diagnostics and tests: the spatial state last applied to an instance.
struct HostSfxSpatialState {
  bool found = false, placed = false;
  float gain = 1.f, pan = 0.f, doppler = 1.f;
};
HostSfxSpatialState host_sfx_spatial_state(int32_t instance);
struct HostSfxStats {
  uint32_t active = 0;
  uint64_t coalesced = 0;
  uint64_t rejected = 0, wave_reads = 0, cache_hits = 0;
  uint32_t cached_waves = 0;
  uint64_t cached_bytes = 0;
};
HostSfxStats host_sfx_stats();

}  // namespace slrr
