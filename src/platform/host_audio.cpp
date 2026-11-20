#include "slrr/host_audio.hpp"
#include <fmod_errors.h>
#include <cstdio>

namespace slrr {
namespace {
std::weak_ptr<HostAudio>& shared_audio() {
  static std::weak_ptr<HostAudio> audio;
  return audio;
}
}

bool host_audio_check(FMOD_RESULT result, const char* operation) {
  if (result == FMOD_OK) return true;
  std::fprintf(stderr, "[audio] FMOD %s: %s (%d)\n", operation, FMOD_ErrorString(result), result);
  return false;
}

HostAudio::~HostAudio() {
  if (effects) effects->release();
  if (music) music->release();
  if (system) system->release();
}

std::shared_ptr<HostAudio> host_audio_acquire() {
  if (auto audio = shared_audio().lock()) return audio;
  auto audio = std::make_shared<HostAudio>();
  if (!host_audio_check(FMOD::System_Create(&audio->system), "create") ||
      // 32 admitted effects plus music; leave room to prepare a replacement
      // before retiring its victim, without FMOD stealing an existing loop.
      !host_audio_check(audio->system->init(64, FMOD_INIT_NORMAL, nullptr), "init") ||
      !host_audio_check(audio->system->createChannelGroup("effects", &audio->effects), "effects group") ||
      !host_audio_check(audio->system->createChannelGroup("music", &audio->music), "music group")) return {};
  shared_audio() = audio;
  std::fprintf(stderr, "[audio] FMOD Core %08x initialized\n", FMOD_VERSION);
  return audio;
}

void host_audio_update() {
  if (auto audio = shared_audio().lock()) host_audio_check(audio->system->update(), "update");
}
} // namespace slrr
