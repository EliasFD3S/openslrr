#pragma once

#include <fmod.hpp>
#include <memory>

namespace slrr {

// Host-thread-only service shared by effects, music and stock FMV. Owners retain it
// so static destruction cannot release the mixer before its sounds/channels.
struct HostAudio {
  FMOD::System* system = nullptr;
  FMOD::ChannelGroup* effects = nullptr;
  FMOD::ChannelGroup* music = nullptr;
  FMOD::ChannelGroup* fmv = nullptr;
  ~HostAudio();
};
std::shared_ptr<HostAudio> host_audio_acquire();
void host_audio_update();
bool host_audio_check(FMOD_RESULT result, const char* operation);

} // namespace slrr
