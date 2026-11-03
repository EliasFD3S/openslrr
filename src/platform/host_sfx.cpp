#include "slrr/host_sfx.hpp"

#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace slrr {
namespace {

std::string g_root;

const char* file_for(HostSfx id) {
  switch (id) {
    case HostSfx::MenuMove:
      return "frontend\\sounds\\mnumove.wav";
    case HostSfx::MenuSelect:
      return "frontend\\sounds\\mnuselect.wav";
    case HostSfx::MenuBack:
      return "frontend\\sounds\\mnuback.wav";
  }
  return nullptr;
}

}  // namespace

void host_sfx_set_game_root(const std::string& game_root) { g_root = game_root; }

void host_sfx_play(HostSfx id) {
  const char* rel = file_for(id);
  if (!rel || g_root.empty()) return;
  const std::string path = g_root + "\\" + rel;
#ifdef _WIN32
  // SND_FILENAME | SND_ASYNC | SND_NODEFAULT — overlap OK for menu ticks.
  if (!PlaySoundA(path.c_str(), nullptr,
                  SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
    std::printf("[sfx] PlaySound fail %s\n", path.c_str());
  }
#else
  (void)path;
#endif
}

}  // namespace slrr
