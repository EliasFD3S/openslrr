#include "slrr/host_sfx.hpp"
#include "slrr/host_audio.hpp"
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>
#include <unordered_map>

namespace slrr {
namespace {
std::string root;
#ifdef _WIN32
struct WaveData {
  std::shared_ptr<HostAudio> audio;
  MIX_Audio* sound = nullptr;
  size_t bytes = 0;
  // Tracks hold their own reference, so a playing sound outlives eviction.
  ~WaveData() { if (sound) MIX_DestroyAudio(sound); }
};
struct CachedWave { std::shared_ptr<const WaveData> data; uint64_t used=0; };
std::unordered_map<std::string,CachedWave> wave_cache;
size_t wave_cache_bytes=0;
uint64_t wave_clock=0,wave_reads=0,wave_hits=0,rejected=0;
constexpr size_t kVoiceLimit=32,kLoopLimit=24,kWaveCacheBytes=16*1024*1024,kWaveCacheEntries=64;
struct Voice {
  int32_t id = 0;
  MIX_Track* source = nullptr;
  std::shared_ptr<const WaveData> data;
  std::string cue;
  float gain = 1.f;
  bool loop = false;
  int menu = 0; // 1 hover, 2 select, 3 back
  std::chrono::steady_clock::time_point last_request;
  ~Voice() {
    if (source)
      MIX_DestroyTrack(source);
  }
};
struct Audio {
  std::shared_ptr<HostAudio> engine;
  std::vector<std::unique_ptr<Voice>> voices;
  int32_t next = 1;
  float volume = 1;
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
      MIX_SetTrackGain(v->source, v->gain / static_cast<float>(copies));
    }
  }
  bool init() {
    if (!engine) engine = host_audio_acquire();
    return engine && host_audio_check(MIX_SetMixerGain(engine->effects, volume), "effects volume");
  }
  void reap() {
    voices.erase(std::remove_if(voices.begin(), voices.end(),
                                [](const auto &v) {
                                  return !MIX_TrackPlaying(v->source) && !MIX_TrackPaused(v->source);
                                }),
                 voices.end());
    rebalance();
  }
} audio;
float finite_pitch(float pitch) {
  // SDL_mixer accepts frequency ratios from 0.01 to 100.
  return std::isfinite(pitch) ? std::clamp(pitch, .01f, 100.f) : 1.f;
}
std::shared_ptr<const WaveData> wave_data(const std::string& path,const std::string& cue) {
  if (auto it=wave_cache.find(cue);it!=wave_cache.end()) {
    it->second.used=++wave_clock;++wave_hits;return it->second.data;
  }
  if (!audio.init()) return {};
  auto data=std::make_shared<WaveData>();++wave_reads;
  data->audio = audio.engine;
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size < 12 || size > 64 * 1024 * 1024) return {};
  const auto utf8 = std::filesystem::path(path).u8string();
  const char* file = reinterpret_cast<const char*>(utf8.c_str());
  // One decoded copy is shared by every one-shot and loop of this cue. It
  // may loop, so the upsampling wraps across its end. Decoding stops at the
  // 64 MiB per-effect bound (32-bit float samples at the mixer rate).
  std::vector<float> samples;
  int channels = 0, rate = 0;
  if (!host_audio_decode_file(audio.engine->effects, file, samples, channels, rate, 64 * 1024 * 1024))
    return {};
  data->sound = host_audio_load_pcm(audio.engine->effects, samples, channels, rate, true);
  if (!data->sound) return {};
  data->bytes = static_cast<size_t>(host_audio_output_frames(audio.engine->effects,
      samples.size() / static_cast<size_t>(channels), rate)) * static_cast<size_t>(channels) * sizeof(float);
  const size_t bytes=data->bytes;
  if (bytes<=kWaveCacheBytes) {
    while (!wave_cache.empty() && (wave_cache_bytes+bytes>kWaveCacheBytes || wave_cache.size()>=kWaveCacheEntries)) {
      auto oldest=std::min_element(wave_cache.begin(),wave_cache.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
      wave_cache_bytes-=oldest->second.data->bytes;
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
  v->source = MIX_CreateTrack(audio.engine->effects);
  if (!host_audio_check(v->source != nullptr, "effect track") ||
      !host_audio_check(MIX_SetTrackAudio(v->source, v->data->sound), "effect audio") ||
      !host_audio_check(MIX_SetTrackFrequencyRatio(v->source, finite_pitch(pitch)), "effect pitch")) return 0;
  if (victim_id) host_sfx_stop(victim_id); // Retire only once replacement is ready.
  while (host_sfx_is_playing(audio.next))
    audio.next = audio.next == INT32_MAX ? 1 : audio.next + 1;
  v->id = audio.next;
  audio.next = audio.next == INT32_MAX ? 1 : audio.next + 1;
  int32_t id = v->id;
  audio.voices.push_back(std::move(v));
  audio.rebalance();
  const SDL_PropertiesID options = SDL_CreateProperties();
  if (options) SDL_SetNumberProperty(options, MIX_PROP_PLAY_LOOPS_NUMBER, loop ? -1 : 0);
  const bool started = options && MIX_PlayTrack(audio.voices.back()->source, options);
  if (options) SDL_DestroyProperties(options);
  if (!host_audio_check(started, "start effect")) {
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
    if (paused) MIX_PauseTrack(voice->source);
    else MIX_ResumeTrack(voice->source);
    return;
  }
#endif
}
void host_sfx_update_loop(int32_t id, float pitch, float volume) {
#ifdef _WIN32
  for (const auto &voice : audio.voices) {
    if (voice->id != id || !voice->loop) continue;
    voice->gain = std::isfinite(volume)?std::clamp(volume,0.f,1.f):1.f;
    MIX_SetTrackGain(voice->source, voice->gain);
    MIX_SetTrackFrequencyRatio(voice->source, finite_pitch(pitch));
    break;
  }
#endif
}
void host_sfx_set_loop_pan(int32_t id, float pan) {
#ifdef _WIN32
  for (const auto &voice : audio.voices) {
    if (voice->id != id || !voice->loop) continue;
    // Balance: the far side fades out, the near side stays at full gain.
    const float p = std::isfinite(pan) ? std::clamp(pan, -1.f, 1.f) : 0.f;
    const MIX_StereoGains gains{p > 0 ? 1.f - p : 1.f, p < 0 ? 1.f + p : 1.f};
    MIX_SetTrackStereo(voice->source, &gains);
    break;
  }
#endif
}
bool host_sfx_is_playing(int32_t id) {
#ifdef _WIN32
  for (const auto &v : audio.voices)
    if (v->id == id)
      return MIX_TrackPlaying(v->source) || MIX_TrackPaused(v->source);
#endif
  return false;
}
void host_sfx_set_volume(float volume) {
#ifdef _WIN32
  audio.volume = std::isfinite(volume) ? std::clamp(volume, 0.f, 1.f) : 1.f;
  if (audio.engine)
    MIX_SetMixerGain(audio.engine->effects, audio.volume);
#endif
}
MIX_Track* host_sfx_track(int32_t id) {
#ifdef _WIN32
  for (const auto &v : audio.voices)
    if (v->id == id) return v->source;
#endif
  return nullptr;
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
