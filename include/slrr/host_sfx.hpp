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
void host_sfx_update_loop(int32_t instance, float pitch, float volume);
bool host_sfx_is_playing(int32_t instance);
void host_sfx_set_volume(float volume);
struct HostSfxStats {
  uint32_t active = 0;
  uint64_t coalesced = 0;
};
HostSfxStats host_sfx_stats();

}  // namespace slrr
