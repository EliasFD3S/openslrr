#include "slrr/host_sfx.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <vector>
#include <unordered_map>
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
struct WaveData { std::vector<uint8_t> format, samples; };
struct CachedWave { std::shared_ptr<const WaveData> data; uint64_t used=0; };
std::unordered_map<std::string,CachedWave> wave_cache;
size_t wave_cache_bytes=0;
uint64_t wave_clock=0,wave_reads=0,wave_hits=0,rejected=0;
constexpr size_t kVoiceLimit=32,kLoopLimit=24,kWaveCacheBytes=16*1024*1024,kWaveCacheEntries=64;
struct Voice {
  int32_t id = 0;
  IXAudio2SourceVoice *source = nullptr;
  std::shared_ptr<const WaveData> data;
  std::string cue;
  float gain = 1.f;
  bool loop = false;
  int menu = 0; // 1 hover, 2 select, 3 back
  std::chrono::steady_clock::time_point last_request;
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
  uint64_t coalesced = 0;
  void rebalance() {
    for (const auto& v : voices) {
      size_t copies = 1;
      if (!v->loop) {
        copies = 0;
        for (const auto& other : voices)
          copies += !other->loop && other->cue == v->cue;
      }
      // Correlated copies can add coherently. 1/N keeps their combined
      // peak within one instance's requested gain, unlike 1/sqrt(N).
      v->source->SetVolume(v->gain / static_cast<float>(copies));
    }
  }
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
    rebalance();
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
std::shared_ptr<const WaveData> wave_data(const std::string& path,const std::string& cue) {
  if (auto it=wave_cache.find(cue);it!=wave_cache.end()) {
    it->second.used=++wave_clock;++wave_hits;return it->second.data;
  }
  auto data=std::make_shared<WaveData>();++wave_reads;
  if (!read_wave(path,data->format,data->samples)) return {};
  const size_t bytes=data->format.size()+data->samples.size();
  if (bytes<=kWaveCacheBytes) {
    while (!wave_cache.empty() && (wave_cache_bytes+bytes>kWaveCacheBytes || wave_cache.size()>=kWaveCacheEntries)) {
      auto oldest=std::min_element(wave_cache.begin(),wave_cache.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
      wave_cache_bytes-=oldest->second.data->format.size()+oldest->second.data->samples.size();
      wave_cache.erase(oldest);
    }
    wave_cache.emplace(cue,CachedWave{data,++wave_clock});wave_cache_bytes+=bytes;
  }
  return data;
}
#endif
} // namespace
void host_sfx_set_game_root(const std::string &path) {
  root = path;
#ifdef _WIN32
  audio.voices.clear();
  audio.coalesced = 0;
  wave_cache.clear();wave_cache_bytes=0;wave_clock=wave_reads=wave_hits=rejected=0;
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
  std::string cue = std::filesystem::path(path).lexically_normal().generic_string();
  std::transform(cue.begin(), cue.end(), cue.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  const auto file = cue.substr(cue.find_last_of('/') + 1);
  const int menu = file == "mnumove.wav" ? 1 : file == "mnuselect.wav" ? 2 : file == "mnuback.wav" ? 3 : 0;
  if (!loop && menu > 1) {
    // A deliberate click takes priority over a lingering hover sound.
    audio.voices.erase(std::remove_if(audio.voices.begin(), audio.voices.end(),
        [](const auto& v) { return !v->loop && v->menu == 1; }), audio.voices.end());
  }
  const auto now = std::chrono::steady_clock::now();
  if (!loop) {
    Voice* latest = nullptr;
    size_t copies = 0;
    size_t menu_copies = 0;
    for (const auto& playing : audio.voices) {
      menu_copies += !playing->loop && playing->menu != 0;
      if (menu == 1 && !playing->loop && playing->menu > 1) {
        ++audio.coalesced;
        return 0;
      }
      if (playing->loop || playing->cue != cue) continue;
      ++copies;
      if (!latest || playing->last_request > latest->last_request) latest = playing.get();
    }
    // Menu clicks/hover may repeat rapidly, but the whole UI has three slots.
    if (menu && menu_copies >= 3) {
      ++audio.coalesced;
      return latest ? latest->id : 0;
    }
    if (!menu && latest && (copies >= 2 || now - latest->last_request < std::chrono::milliseconds(35))) {
      // Coalesce bursts before loading/allocating another WAV/source voice.
      latest->last_request = now;
      ++audio.coalesced;
      return latest->id;
    }
  }
  // Admit requests before disk reads or source allocation. Loops cannot
  // occupy the eight slots reserved for clicks, impacts and other one-shots.
  if (loop && std::count_if(audio.voices.begin(),audio.voices.end(),[](const auto& v){return v->loop;})>=kLoopLimit) {
    ++rejected;return 0;
  }
  int32_t victim_id=0;
  if (audio.voices.size()>=kVoiceLimit) {
    auto victim=std::find_if(audio.voices.begin(),audio.voices.end(),[](const auto& v){return !v->loop;});
    if (victim==audio.voices.end()) {++rejected;return 0;}
    victim_id=(*victim)->id;
  }
  auto v = std::make_unique<Voice>();
  v->cue = cue;
  v->loop = loop;
  v->menu = menu;
  v->last_request = now;
  v->gain = std::isfinite(volume) ? std::clamp(volume, 0.f, 1.f) : 1.f;
  v->data=wave_data(path,cue);
  if (!v->data || !audio.init()) {
    printf("[sfx] cannot load/play %s\n", path.c_str());
    return 0;
  }
  auto hr = audio.engine->CreateSourceVoice(
      &v->source, reinterpret_cast<const WAVEFORMATEX *>(v->data->format.data()), 0,
      XAUDIO2_MAX_FREQ_RATIO);
  if (FAILED(hr)) {
    printf("[sfx] unsupported %s hr=%lx\n", path.c_str(), hr);
    return 0;
  }
  v->source->SetFrequencyRatio(
      std::isfinite(pitch)
          ? std::clamp(pitch, XAUDIO2_MIN_FREQ_RATIO, XAUDIO2_MAX_FREQ_RATIO)
          : 1.f);
  XAUDIO2_BUFFER b{};
  b.Flags = XAUDIO2_END_OF_STREAM;
  b.AudioBytes = static_cast<UINT32>(v->data->samples.size());
  b.pAudioData = v->data->samples.data();
  if (loop)
    b.LoopCount = XAUDIO2_LOOP_INFINITE;
  if (FAILED(v->source->SubmitSourceBuffer(&b)))
    return 0;
  if (victim_id) host_sfx_stop(victim_id); // Retire only once replacement is ready.
  while (host_sfx_is_playing(audio.next))
    audio.next = audio.next == INT32_MAX ? 1 : audio.next + 1;
  v->id = audio.next;
  audio.next = audio.next == INT32_MAX ? 1 : audio.next + 1;
  int32_t id = v->id;
  audio.voices.push_back(std::move(v));
  audio.rebalance();
  if (FAILED(audio.voices.back()->source->Start())) {
    audio.voices.pop_back();
    audio.rebalance();
    return 0;
  }
  return id;
#else
  return 0;
#endif
}
void host_sfx_stop(int32_t id) {
#ifdef _WIN32
  if (id <= 0) return;
  audio.voices.erase(
      std::remove_if(audio.voices.begin(), audio.voices.end(),
                     [id](const auto &v) { return v->id == id; }),
      audio.voices.end());
  audio.rebalance();
#endif
}
void host_sfx_pause_loop(int32_t id, bool paused) {
#ifdef _WIN32
  if (id <= 0) return;
  for (const auto& voice : audio.voices) {
    if (voice->id != id || !voice->loop) continue;
    if (paused) voice->source->Stop();
    else voice->source->Start();
    return;
  }
#endif
}
void host_sfx_update_loop(int32_t id, float pitch, float volume) {
#ifdef _WIN32
  for (const auto &voice : audio.voices) {
    if (voice->id != id || !voice->loop) continue;
    voice->gain = std::isfinite(volume)?std::clamp(volume,0.f,1.f):1.f;
    voice->source->SetVolume(voice->gain);
    voice->source->SetFrequencyRatio(
        std::isfinite(pitch)?std::clamp(pitch,XAUDIO2_MIN_FREQ_RATIO,XAUDIO2_MAX_FREQ_RATIO):1.f);
    break;
  }
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
HostSfxStats host_sfx_stats() {
#ifdef _WIN32
  audio.reap();
  return {static_cast<uint32_t>(audio.voices.size()), audio.coalesced, rejected, wave_reads, wave_hits,
          static_cast<uint32_t>(wave_cache.size()),wave_cache_bytes};
#else
  return {};
#endif
}
} // namespace slrr
