#include "slrr/host_sfx.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#endif
namespace slrr {
namespace {
std::string root;
#ifdef _WIN32
struct Voice {
  int32_t id = 0;
  IXAudio2SourceVoice *source = nullptr;
  std::vector<uint8_t> data;
  ~Voice() {
    if (source)
      source->DestroyVoice();
  }
};
struct Audio {
  IXAudio2 *engine = nullptr;
  IXAudio2MasteringVoice *master = nullptr;
  std::vector<std::unique_ptr<Voice>> voices;
  int32_t next = 1;
  float volume = 1;
  bool com = false;
  ~Audio() {
    voices.clear();
    if (master)
      master->DestroyVoice();
    if (engine)
      engine->Release();
    if (com)
      CoUninitialize();
  }
  bool init() {
    if (master)
      return true;
    if (!engine) {
      auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      com = SUCCEEDED(hr);
      if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return false;
      if (FAILED(XAudio2Create(&engine)))
        return false;
    }
    if (FAILED(engine->CreateMasteringVoice(&master)))
      return false;
    master->SetVolume(volume);
    return true;
  }
  void reap() {
    voices.erase(std::remove_if(voices.begin(), voices.end(),
                                [](const auto &v) {
                                  XAUDIO2_VOICE_STATE s{};
                                  v->source->GetState(
                                      &s, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                                  return !s.BuffersQueued;
                                }),
                 voices.end());
  }
} audio;
bool read_wave(const std::string &path, std::vector<uint8_t> &format,
               std::vector<uint8_t> &data) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    return false;
  auto size = in.tellg();
  if (size < 12 || size > 64 * 1024 * 1024)
    return false;
  std::vector<uint8_t> b(static_cast<size_t>(size));
  in.seekg(0);
  if (!in.read(reinterpret_cast<char *>(b.data()), b.size()))
    return false;
  if (memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4))
    return false;
  for (size_t p = 12; p + 8 <= b.size();) {
    uint32_t n;
    memcpy(&n, b.data() + p + 4, 4);
    size_t start = p + 8;
    if (n > b.size() - start)
      return false;
    if (!memcmp(b.data() + p, "fmt ", 4)) {
      if (n < 16)
        return false;
      format.assign(std::max<size_t>(n, sizeof(WAVEFORMATEX)), 0);
      memcpy(format.data(), b.data() + start, n);
    } else if (!memcmp(b.data() + p, "data", 4))
      data.assign(b.begin() + start, b.begin() + start + n);
    p = start + n + (n & 1);
  }
  return !format.empty() && !data.empty() &&
         sizeof(WAVEFORMATEX) +
                 reinterpret_cast<const WAVEFORMATEX *>(format.data())
                     ->cbSize <=
             format.size();
}
#endif
} // namespace
void host_sfx_set_game_root(const std::string &path) {
  root = path;
#ifdef _WIN32
  audio.voices.clear();
#endif
}
void host_sfx_play(HostSfx id) {
  const char *file = id == HostSfx::MenuMove     ? "mnumove.wav"
                     : id == HostSfx::MenuSelect ? "mnuselect.wav"
                                                 : "mnuback.wav";
  if (!root.empty())
    host_sfx_play_file(root + "/frontend/sounds/" + file);
}
int32_t host_sfx_play_file(const std::string &path, float pitch, float volume,
                           bool loop, int32_t instance) {
#ifdef _WIN32
  audio.reap();
  if (instance > 0 && host_sfx_is_playing(instance))
    return instance;
  auto v = std::make_unique<Voice>();
  std::vector<uint8_t> fmt;
  if (!read_wave(path, fmt, v->data) || !audio.init()) {
    printf("[sfx] cannot load/play %s\n", path.c_str());
    return 0;
  }
  auto hr = audio.engine->CreateSourceVoice(
      &v->source, reinterpret_cast<const WAVEFORMATEX *>(fmt.data()), 0,
      XAUDIO2_MAX_FREQ_RATIO);
  if (FAILED(hr)) {
    printf("[sfx] unsupported %s hr=%lx\n", path.c_str(), hr);
    return 0;
  }
  v->source->SetFrequencyRatio(
      std::isfinite(pitch)
          ? std::clamp(pitch, XAUDIO2_MIN_FREQ_RATIO, XAUDIO2_MAX_FREQ_RATIO)
          : 1.f);
  v->source->SetVolume(std::isfinite(volume) ? std::clamp(volume, 0.f, 1.f)
                                             : 1.f);
  XAUDIO2_BUFFER b{};
  b.Flags = XAUDIO2_END_OF_STREAM;
  b.AudioBytes = static_cast<UINT32>(v->data.size());
  b.pAudioData = v->data.data();
  if (loop)
    b.LoopCount = XAUDIO2_LOOP_INFINITE;
  if (FAILED(v->source->SubmitSourceBuffer(&b)) || FAILED(v->source->Start()))
    return 0;
  if (audio.voices.size() >= 32)
    audio.voices.erase(audio.voices.begin());
  while (host_sfx_is_playing(audio.next))
    audio.next = audio.next == INT32_MAX ? 1 : audio.next + 1;
  v->id = audio.next;
  audio.next = audio.next == INT32_MAX ? 1 : audio.next + 1;
  int32_t id = v->id;
  audio.voices.push_back(std::move(v));
  return id;
#else
  return 0;
#endif
}
void host_sfx_stop(int32_t id) {
#ifdef _WIN32
  audio.voices.erase(
      std::remove_if(audio.voices.begin(), audio.voices.end(),
                     [id](const auto &v) { return v->id == id; }),
      audio.voices.end());
#endif
}
bool host_sfx_is_playing(int32_t id) {
#ifdef _WIN32
  for (const auto &v : audio.voices)
    if (v->id == id) {
      XAUDIO2_VOICE_STATE s{};
      v->source->GetState(&s, XAUDIO2_VOICE_NOSAMPLESPLAYED);
      return s.BuffersQueued != 0;
    }
#endif
  return false;
}
void host_sfx_set_volume(float volume) {
#ifdef _WIN32
  audio.volume = std::isfinite(volume) ? std::clamp(volume, 0.f, 1.f) : 1.f;
  if (audio.master)
    audio.master->SetVolume(audio.volume);
#endif
}
} // namespace slrr
