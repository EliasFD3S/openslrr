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
  float pitch = 1.f;       // requested, before the Doppler factor
  float pan = 0.f;         // host_sfx_set_loop_pan, for an unplaced loop
  bool placed = false;
  HostSfxPlace place;
  // Last applied spatial state; the Doppler factor is smoothed per frame.
  float spatial_gain = 1.f, spatial_pan = 0.f, doppler = 1.f;
  bool stereo = false;     // MIX_SetTrackStereo is active
  bool loop = false;
  int menu = 0; // 1 hover, 2 select, 3 back
  std::chrono::steady_clock::time_point last_request;
  ~Voice() {
    if (source)
      MIX_DestroyTrack(source);
  }
};
// The world camera. Its velocity is a smoothed finite difference of the eye.
struct Listener {
  bool valid = false;
  float eye[3]{}, right[3]{1, 0, 0}, velocity[3]{};
} listener;
HostSfxSpatialOptions spatial_options;
HostSfxEnvironment environment = HostSfxEnvironment::None;
constexpr float kSpeedOfSound = 343.f;

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Gain, pan and target Doppler factor of a placed voice for this listener.
void spatialize(const Voice& v, float& gain, float& pan, float& doppler) {
  gain = 1.f; pan = 0.f; doppler = 1.f;
  if (!v.placed || !listener.valid || !spatial_options.positioning) return;
  const auto& p = v.place;
  const float d[3] = {p.position[0] - listener.eye[0], p.position[1] - listener.eye[1],
                      p.position[2] - listener.eye[2]};
  const float distance = std::sqrt(dot3(d, d));
  if (!std::isfinite(distance)) { gain = 0.f; return; }
  const float reference = std::max(p.reference, .01f);
  if (p.attenuate)
    gain = distance > std::max(p.maximum, reference) ? 0.f
         : reference / (reference + std::max(p.rolloff, 0.f) * std::max(0.f, distance - reference));
  // Inside the reference distance a source drifts back to the centre, so a
  // sound at the listener does not jump between the speakers.
  pan = std::clamp(dot3(d, listener.right) / std::max(distance, reference), -1.f, 1.f);
  if (spatial_options.doppler && distance > .05f) {
    const float u[3] = {d[0] / distance, d[1] / distance, d[2] / distance};
    // Positive: the source moves away, the listener moves towards it.
    const float away = std::clamp(dot3(p.velocity, u), -.5f * kSpeedOfSound, .5f * kSpeedOfSound);
    const float towards = std::clamp(dot3(listener.velocity, u), -.5f * kSpeedOfSound, .5f * kSpeedOfSound);
    doppler = std::clamp((kSpeedOfSound + towards) / (kSpeedOfSound + away), .5f, 2.f);
  }
}

struct Audio {
  std::shared_ptr<HostAudio> engine;
  std::vector<std::unique_ptr<Voice>> voices;
  int32_t next = 1;
  float volume = 1;
  uint64_t coalesced = 0;
  // Applies the requested gain/pitch with the overlap and spatial factors.
  // `smooth` blends the Doppler factor towards its target (per frame) so
  // camera jitter does not warble; a new voice starts at its target.
  void apply(Voice& v, bool smooth = true) {
    size_t copies = 1;
    if (!v.loop) {
      copies = 0;
      for (const auto& other : voices)
        copies += !other->loop && other->cue == v.cue;
      copies = std::max<size_t>(copies, 1);
    }
    float gain, pan, doppler;
    spatialize(v, gain, pan, doppler);
    v.spatial_gain = gain;
    v.spatial_pan = v.placed ? pan : v.pan;
    v.doppler = smooth ? v.doppler + (doppler - v.doppler) * .35f : doppler;
    // Correlated copies can add coherently. 1/N keeps their combined
    // peak within one instance's requested gain, unlike 1/sqrt(N).
    MIX_SetTrackGain(v.source, v.gain * gain / static_cast<float>(copies));
    MIX_SetTrackFrequencyRatio(v.source, std::clamp(v.pitch * v.doppler, .01f, 100.f));
    if (v.spatial_pan != 0.f || v.stereo) {
      // Constant power, scaled so the centre keeps both sides at unit gain.
      const float angle = (v.spatial_pan + 1.f) * .78539816f;
      const MIX_StereoGains gains{1.41421356f * std::cos(angle), 1.41421356f * std::sin(angle)};
      MIX_SetTrackStereo(v.source, &gains);
      v.stereo = true;
    }
  }
  void rebalance() {
    for (const auto& v : voices) apply(*v);
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
namespace {
int32_t play(const std::string &path, const HostSfxPlace* place, float pitch, float volume,
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
  v->pitch = finite_pitch(pitch);
  if (place) { v->placed = true; v->place = *place; }
  v->data=wave_data(path,cue);
  if (!v->data || !audio.init()) {
    printf("[sfx] cannot load/play %s\n", path.c_str());
    return 0;
  }
  v->source = MIX_CreateTrack(audio.engine->effects);
  if (!host_audio_check(v->source != nullptr, "effect track") ||
      !host_audio_check(MIX_SetTrackAudio(v->source, v->data->sound), "effect audio") ||
      !host_audio_check(MIX_SetTrackFrequencyRatio(v->source, v->pitch), "effect pitch")) return 0;
  // Placed voices are the world group the environment reverb processes.
  if (v->placed) host_audio_join_world(v->source);
  if (victim_id) host_sfx_stop(victim_id); // Retire only once replacement is ready.
  while (host_sfx_is_playing(audio.next))
    audio.next = audio.next == INT32_MAX ? 1 : audio.next + 1;
  v->id = audio.next;
  audio.next = audio.next == INT32_MAX ? 1 : audio.next + 1;
  int32_t id = v->id;
  audio.voices.push_back(std::move(v));
  audio.apply(*audio.voices.back(), false);
  for (const auto& other : audio.voices)
    if (other.get() != audio.voices.back().get() && other->cue == audio.voices.back()->cue) audio.apply(*other);
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
} // namespace
int32_t host_sfx_play_file(const std::string &path, float pitch, float volume,
                           bool loop, int32_t instance) {
  return play(path, nullptr, pitch, volume, loop, instance);
}
int32_t host_sfx_play_at(const std::string &path, const HostSfxPlace& place, float pitch,
                         float volume, bool loop, int32_t instance) {
  return play(path, &place, pitch, volume, loop, instance);
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
    voice->pitch = finite_pitch(pitch);
    audio.apply(*voice);
    break;
  }
#endif
}
void host_sfx_set_loop_pan(int32_t id, float pan) {
#ifdef _WIN32
  for (const auto &voice : audio.voices) {
    if (voice->id != id || !voice->loop) continue;
    voice->pan = std::isfinite(pan) ? std::clamp(pan, -1.f, 1.f) : 0.f;
    audio.apply(*voice);
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
void host_sfx_set_place(int32_t id, const HostSfxPlace& place) {
#ifdef _WIN32
  for (const auto &voice : audio.voices) {
    if (voice->id != id) continue;
    const bool fresh = !voice->placed;
    voice->placed = true;
    voice->place = place;
    if (fresh) host_audio_join_world(voice->source);
    audio.apply(*voice, !fresh);
    break;
  }
#endif
}
void host_sfx_listener_right(const float f[3], const float up[3], float right[3]) {
  right[0] = f[1] * up[2] - f[2] * up[1];
  right[1] = f[2] * up[0] - f[0] * up[2];
  right[2] = f[0] * up[1] - f[1] * up[0];
  const float length = std::sqrt(dot3(right, right));
  if (length > 1e-6f) for (int i = 0; i < 3; ++i) right[i] /= length;
}
void host_sfx_set_listener(bool valid, const float eye[3], const float forward[3],
                           const float up[3], double dt) {
#ifdef _WIN32
  float right[3]{};
  if (valid) host_sfx_listener_right(forward, up, right);
  valid = valid && std::isfinite(dot3(eye, eye)) && dot3(right, right) > .5f;
  if (valid && listener.valid && dt > 1e-4) {
    const float moved[3] = {eye[0] - listener.eye[0], eye[1] - listener.eye[1], eye[2] - listener.eye[2]};
    const float step = std::sqrt(dot3(moved, moved));
    // A camera cut (switch, teleport) is not motion: no Doppler sweep.
    const bool cut = step > 120.f * dt || step > 25.f;
    const float blend = static_cast<float>(std::clamp(dt / .12, 0.0, 1.0));
    for (int i = 0; i < 3; ++i) {
      const float target = cut ? 0.f : moved[i] / static_cast<float>(dt);
      listener.velocity[i] = cut ? 0.f : listener.velocity[i] + (target - listener.velocity[i]) * blend;
    }
  } else {
    for (float& c : listener.velocity) c = 0.f;
  }
  listener.valid = valid;
  if (valid)
    for (int i = 0; i < 3; ++i) { listener.eye[i] = eye[i]; listener.right[i] = right[i]; }
  for (const auto &voice : audio.voices)
    if (voice->placed) audio.apply(*voice);
#endif
}
HostSfxSpatialOptions& host_sfx_spatial_options() { return spatial_options; }
void host_sfx_apply_spatial_options() {
#ifdef _WIN32
  for (const auto &voice : audio.voices) audio.apply(*voice, false);
  host_audio_set_world_reverb(spatial_options.reverb ? static_cast<int>(environment) : 0);
#endif
}
void host_sfx_set_environment(HostSfxEnvironment value) {
  if (value == environment) return;
  environment = value;
  static const char* names[] = {"none", "garage", "city", "open"};
  std::fprintf(stderr, "[audio] environment %s\n", names[static_cast<int>(value)]);
#ifdef _WIN32
  host_audio_set_world_reverb(spatial_options.reverb ? static_cast<int>(environment) : 0);
#endif
}
HostSfxEnvironment host_sfx_environment() { return environment; }
HostSfxSpatialState host_sfx_spatial_state(int32_t id) {
#ifdef _WIN32
  for (const auto &v : audio.voices)
    if (v->id == id) return {true, v->placed, v->spatial_gain, v->spatial_pan, v->doppler};
#endif
  return {};
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
