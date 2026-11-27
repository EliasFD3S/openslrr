#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct MIX_Mixer;
struct MIX_Track;
struct MIX_Audio;
struct MIX_Group;

namespace slrr {

// Host-thread-only service shared by effects, music and stock FMV. Owners retain it
// so static destruction cannot destroy a mixer before its tracks and audio.
// Each group is its own SDL3_mixer mixer (a logical device on the default
// output), so its gain is independent of the other two. SDL_mixer mixes on
// SDL's audio thread; there is no per-frame update to pump.
struct HostAudio {
  MIX_Mixer* effects = nullptr;
  MIX_Mixer* music = nullptr;
  MIX_Mixer* fmv = nullptr;
  // Effects tracks placed in the world (#390); the environment reverb and
  // echo process this group only, so menus and the cockpit stay dry.
  MIX_Group* world = nullptr;
  bool initialized = false;
  ~HostAudio();
};
std::shared_ptr<HostAudio> host_audio_acquire();
// Logs SDL_GetError() for a failed SDL/SDL3_mixer call and returns `ok`.
bool host_audio_check(bool ok, const char* operation);

// Effects and stock FMV PCM are upsampled to the mixer rate with linear
// interpolation, as FMOD's software mixer did. SDL's band-limited resampler
// removes every image above the source Nyquist frequency, which makes the
// stock 11/22 kHz samples sound muffled (#389).
int host_audio_mixer_rate(MIX_Mixer* mixer);
// Frames after upsampling `frames` at `rate` to the mixer rate.
uint64_t host_audio_output_frames(MIX_Mixer* mixer, uint64_t frames, int rate);
// Decodes a whole file to interleaved float samples at its own rate.
// Loop points in the file (a WAV `smpl` chunk) are ignored: the caller
// decides whether a sound loops. Fails once the samples would take more
// than `max_bytes` after upsampling to `mixer`'s rate.
bool host_audio_decode_file(MIX_Mixer* mixer, const char* path, std::vector<float>& samples,
                            int& channels, int& rate, uint64_t max_bytes);
// Loads interleaved float PCM, upsampled first when `rate` is below the
// mixer rate. `wrap` interpolates the last frame towards the first, for
// samples that may loop. `loaded_rate` receives the rate of the audio.
MIX_Audio* host_audio_load_pcm(MIX_Mixer* mixer, const std::vector<float>& samples,
                               int channels, int rate, bool wrap, int* loaded_rate = nullptr);

// Moves an effects track into the world group.
void host_audio_join_world(MIX_Track* track);
// Environment preset of the world reverb: 0 none (dry), 1 garage, 2 city,
// 3 open (EnvironmentReverb::Preset).
void host_audio_set_world_reverb(int preset);
int host_audio_world_reverb();

// Diagnostics and tests: the track behind an effect instance (null when the
// instance is unknown) and the stock Cinepak FMV PCM track (null when none).
MIX_Track* host_sfx_track(int32_t instance);
MIX_Track* cinepak_fmv_audio_track();

} // namespace slrr
