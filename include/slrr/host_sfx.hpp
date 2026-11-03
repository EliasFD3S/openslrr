#pragma once

#include <string>

namespace slrr {

// Menu SFX from Frontend.java (frontend.rpk → frontend/sounds/*.wav).
enum class HostSfx {
  MenuMove,    // 0x0079 mnumove.wav
  MenuSelect,  // 0x007A mnuselect.wav
  MenuBack,    // 0x0078 mnuback.wav
};

void host_sfx_set_game_root(const std::string& game_root);
void host_sfx_play(HostSfx id);

}  // namespace slrr
