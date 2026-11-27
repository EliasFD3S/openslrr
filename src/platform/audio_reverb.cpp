#include "audio_reverb.hpp"

#include <algorithm>
#include <cmath>

namespace slrr {
namespace {
// Freeverb tunings in samples at 44.1 kHz; the right side is spread by 23.
constexpr std::array<int, 8> kCombTuning = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr std::array<int, 4> kAllpassTuning = {556, 441, 341, 225};
constexpr int kStereoSpread = 23;
constexpr float kInputGain = .015f, kAllpassFeedback = .5f;
}  // namespace

EnvironmentReverb::Settings EnvironmentReverb::settings(int preset) {
  switch (preset) {
    // A concrete workshop: a dense, short room with an early reflection.
    case Garage: return {.62f, .42f, .20f, 38.f, .18f, .10f, .5f};
    // Facades: a light room and a damped slapback.
    case City: return {.38f, .55f, .12f, 115.f, .28f, .17f, .6f};
    // Open track or countryside: almost dry, a distant echo.
    case Open: return {.22f, .65f, .05f, 260.f, .12f, .08f, .7f};
    default: return {};
  }
}

void EnvironmentReverb::configure(int preset, int rate) {
  active_preset_ = preset;
  rate_ = rate;
  current_ = settings(preset);
  const double scale = rate / 44100.0;
  for (int side = 0; side < 2; ++side) {
    for (size_t i = 0; i < combs_[side].size(); ++i) {
      auto& comb = combs_[side][i];
      comb.buffer.assign(static_cast<size_t>((kCombTuning[i] + side * kStereoSpread) * scale) + 1, 0.f);
      comb.index = 0;
      comb.store = 0;
    }
    for (size_t i = 0; i < allpasses_[side].size(); ++i) {
      auto& allpass = allpasses_[side][i];
      allpass.buffer.assign(static_cast<size_t>((kAllpassTuning[i] + side * kStereoSpread) * scale) + 1, 0.f);
      allpass.index = 0;
    }
    const size_t echo = current_.echo_ms > 0
        ? static_cast<size_t>(current_.echo_ms * rate / 1000.f) + 1 : 0;
    echo_[side].assign(echo, 0.f);
    echo_store_[side] = 0;
  }
  echo_index_ = 0;
  ringing_ = false;
  silent_frames_ = 0;
}

void EnvironmentReverb::process(const float* input, float* output, int frames, int channels, int rate) {
  if (frames <= 0 || channels <= 0 || rate <= 0) return;
  const int preset = preset_.load(std::memory_order_relaxed);
  // A change of room restarts from silence rather than carrying a tail
  // from the garage into the street.
  if (preset != active_preset_ || rate != rate_) configure(preset, rate);
  if (preset == None) return;
  const float feedback = current_.room * .28f + .7f, damp = current_.damp * .4f;
  const bool stereo = channels >= 2;
  float peak = 0;
  for (int f = 0; f < frames; ++f) {
    const float* in = input ? input + static_cast<size_t>(f) * channels : nullptr;
    const float left = in ? in[0] : 0.f, right = in ? (stereo ? in[1] : in[0]) : 0.f;
    const float mono = (left + right) * kInputGain;
    float wet[2]{};
    for (int side = 0; side < 2; ++side) {
      float sum = 0;
      for (auto& comb : combs_[side]) {
        const float out = comb.buffer[comb.index];
        comb.store = out * (1.f - damp) + comb.store * damp;
        comb.buffer[comb.index] = mono + comb.store * feedback;
        if (++comb.index == comb.buffer.size()) comb.index = 0;
        sum += out;
      }
      for (auto& allpass : allpasses_[side]) {
        const float delayed = allpass.buffer[allpass.index];
        const float out = delayed - sum;
        allpass.buffer[allpass.index] = sum + delayed * kAllpassFeedback;
        if (++allpass.index == allpass.buffer.size()) allpass.index = 0;
        sum = out;
      }
      wet[side] = sum * current_.wet * 3.f;  // Freeverb's wet scale
    }
    if (!echo_[0].empty()) {
      const float source[2] = {left, right};
      for (int side = 0; side < 2; ++side) {
        auto& line = echo_[side];
        const float delayed = line[echo_index_];
        // One-pole low-pass in the loop: each repeat is duller, like a
        // reflection off distant walls.
        echo_store_[side] = delayed * (1.f - current_.echo_damp) + echo_store_[side] * current_.echo_damp;
        line[echo_index_] = source[side] + echo_store_[side] * current_.echo_feedback;
        wet[side] += delayed * current_.echo_level;
      }
      if (++echo_index_ == echo_[0].size()) echo_index_ = 0;
    }
    float* out = output + static_cast<size_t>(f) * channels;
    if (stereo) {
      out[0] += wet[0];
      out[1] += wet[1];
    } else {
      out[0] += .5f * (wet[0] + wet[1]);
    }
    peak = std::max(peak, std::max(std::abs(wet[0]), std::abs(wet[1])));
  }
  // About a second below -90 dB means the tail is gone.
  silent_frames_ = peak < 3e-5f ? silent_frames_ + frames : 0;
  ringing_ = silent_frames_ < rate;
}

}  // namespace slrr
