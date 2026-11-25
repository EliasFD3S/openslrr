#include "slrr/host_audio.hpp"
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace slrr {
namespace {
std::weak_ptr<HostAudio>& shared_audio() {
  static std::weak_ptr<HostAudio> audio;
  return audio;
}
MIX_Mixer* open_group(const char* name) {
  // Each call opens a new logical device on the default output. SDL follows
  // the default device when the hardware changes.
  MIX_Mixer* mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
  host_audio_check(mixer != nullptr, name);
  return mixer;
}
}

bool host_audio_check(bool ok, const char* operation) {
  if (!ok) std::fprintf(stderr, "[audio] SDL_mixer %s: %s\n", operation, SDL_GetError());
  return ok;
}

int host_audio_mixer_rate(MIX_Mixer* mixer) {
  SDL_AudioSpec spec{};
  return mixer && MIX_GetMixerFormat(mixer, &spec) && spec.freq > 0 ? spec.freq : 48000;
}

uint64_t host_audio_output_frames(MIX_Mixer* mixer, uint64_t frames, int rate) {
  const int target = host_audio_mixer_rate(mixer);
  if (rate <= 0 || rate >= target) return frames;
  return (frames * static_cast<uint64_t>(target) + static_cast<uint64_t>(rate) - 1) / static_cast<uint64_t>(rate);
}

bool host_audio_decode_file(MIX_Mixer* mixer, const char* path, std::vector<float>& samples,
                            int& channels, int& rate, uint64_t max_bytes) {
  samples.clear();
  const SDL_PropertiesID props = SDL_CreateProperties();
  if (!host_audio_check(props != 0, "decoder properties")) return false;
  // FMOD ignored WAV loop points unless a sound was played looping;
  // SDL_mixer would otherwise report an infinite duration (Slip.wav).
  SDL_SetBooleanProperty(props, MIX_PROP_AUDIO_LOAD_IGNORE_LOOPS_BOOLEAN, true);
  MIX_AudioDecoder* decoder = MIX_CreateAudioDecoder(path, props);
  SDL_DestroyProperties(props);
  if (!host_audio_check(decoder != nullptr, "open decoder")) return false;
  SDL_AudioSpec spec{};
  bool ok = host_audio_check(MIX_GetAudioDecoderFormat(decoder, &spec), "decoder format") &&
            spec.channels > 0 && spec.freq > 0;
  if (ok) {
    channels = spec.channels;
    rate = spec.freq;
    const SDL_AudioSpec out{SDL_AUDIO_F32, spec.channels, spec.freq};
    std::vector<float> chunk(4096 * static_cast<size_t>(spec.channels));
    for (;;) {
      const int got = MIX_DecodeAudio(decoder, chunk.data(), static_cast<int>(chunk.size() * sizeof(float)), &out);
      if (got < 0) { ok = host_audio_check(false, "decode"); break; }
      if (got == 0) break;
      samples.insert(samples.end(), chunk.begin(), chunk.begin() + got / static_cast<int>(sizeof(float)));
      // Bound the decoded size as it grows: a small compressed file must
      // not expand without limit in memory.
      const uint64_t frames = samples.size() / static_cast<size_t>(channels);
      if (host_audio_output_frames(mixer, frames, rate) * static_cast<uint64_t>(channels) * sizeof(float) > max_bytes) {
        ok = false;
        break;
      }
    }
    samples.resize(samples.size() / static_cast<size_t>(channels) * static_cast<size_t>(channels));
  }
  MIX_DestroyAudioDecoder(decoder);
  return ok && !samples.empty();
}

MIX_Audio* host_audio_load_pcm(MIX_Mixer* mixer, const std::vector<float>& samples,
                               int channels, int rate, bool wrap, int* loaded_rate) {
  if (!mixer || channels <= 0 || rate <= 0 || samples.size() < static_cast<size_t>(channels)) return nullptr;
  const size_t frames = samples.size() / static_cast<size_t>(channels);
  const int target = host_audio_mixer_rate(mixer);
  const std::vector<float>* pcm = &samples;
  std::vector<float> upsampled;
  int out_rate = rate;
  if (rate < target) {
    // Linear interpolation keeps the images above the source Nyquist
    // frequency, like FMOD's default software mixer and DirectSound.
    const size_t out_frames = static_cast<size_t>(host_audio_output_frames(mixer, frames, rate));
    upsampled.resize(out_frames * static_cast<size_t>(channels));
    const double step = static_cast<double>(rate) / target;
    for (size_t i = 0; i < out_frames; ++i) {
      const double position = static_cast<double>(i) * step;
      const size_t a = std::min(static_cast<size_t>(position), frames - 1);
      const size_t b = a + 1 < frames ? a + 1 : wrap ? 0 : a;
      const float t = static_cast<float>(position - static_cast<double>(a));
      for (int c = 0; c < channels; ++c) {
        const float x = samples[a * channels + c], y = samples[b * channels + c];
        upsampled[i * channels + c] = x + (y - x) * t;
      }
    }
    pcm = &upsampled;
    out_rate = target;
  }
  if (channels == 1) {
    // SDL copies mono to both front speakers at full gain. FMOD centred it
    // at -3 dB per side; keep that level relative to stereo sounds.
    if (pcm != &upsampled) { upsampled = samples; pcm = &upsampled; }
    for (float& sample : upsampled) sample *= 0.70710678f;
  }
  const SDL_AudioSpec spec{SDL_AUDIO_F32, channels, out_rate};
  // Raw audio is copied and never decoded again.
  MIX_Audio* audio = MIX_LoadRawAudio(mixer, pcm->data(), pcm->size() * sizeof(float), &spec);
  if (!host_audio_check(audio != nullptr, "load PCM")) return nullptr;
  if (loaded_rate) *loaded_rate = out_rate;
  return audio;
}

HostAudio::~HostAudio() {
  if (effects) MIX_DestroyMixer(effects);
  if (music) MIX_DestroyMixer(music);
  if (fmv) MIX_DestroyMixer(fmv);
  if (initialized) MIX_Quit();
}

std::shared_ptr<HostAudio> host_audio_acquire() {
  if (auto audio = shared_audio().lock()) return audio;
  auto audio = std::make_shared<HostAudio>();
  audio->initialized = host_audio_check(MIX_Init(), "init");
  if (!audio->initialized || !(audio->effects = open_group("effects mixer")) ||
      !(audio->music = open_group("music mixer")) || !(audio->fmv = open_group("FMV mixer")))
    return {};
  shared_audio() = audio;
  const int version = MIX_Version();
  std::fprintf(stderr, "[audio] SDL_mixer %d.%d.%d initialized (%s)\n",
               SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version),
               SDL_VERSIONNUM_MICRO(version), SDL_GetCurrentAudioDriver());
  return audio;
}
} // namespace slrr
