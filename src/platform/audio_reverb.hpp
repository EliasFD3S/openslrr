#pragma once

#include <array>
#include <atomic>
#include <vector>

namespace slrr {

// Environment reverb and echo for the world group of the effects mixer
// (#390). A Freeverb-style network (eight damped combs and four allpasses per
// side, Jezar's public-domain tunings) gives the room; a damped feedback
// delay gives the slapback echo of streets and open spaces. Runs on SDL's
// audio thread: the preset is the only state shared with the host thread.
class EnvironmentReverb {
 public:
  enum Preset { None = 0, Garage = 1, City = 2, Open = 3 };
  struct Settings {
    float room = 0, damp = 0, wet = 0;          // reverb
    float echo_ms = 0, echo_feedback = 0, echo_level = 0, echo_damp = 0;
  };
  static Settings settings(int preset);

  void set_preset(int preset) { preset_.store(preset, std::memory_order_relaxed); }
  int preset() const { return preset_.load(std::memory_order_relaxed); }
  // Adds the wet signal computed from `input` (interleaved, `channels` per
  // frame; null for silence) to `output`. Only the front pair is processed.
  void process(const float* input, float* output, int frames, int channels, int rate);
  // True while a tail from earlier input is still audible.
  bool ringing() const { return ringing_; }

 private:
  struct Comb {
    std::vector<float> buffer;
    size_t index = 0;
    float store = 0;
  };
  struct Allpass {
    std::vector<float> buffer;
    size_t index = 0;
  };
  void configure(int preset, int rate);

  std::atomic<int> preset_{None};
  int active_preset_ = -1, rate_ = 0;
  Settings current_;
  std::array<std::array<Comb, 8>, 2> combs_;
  std::array<std::array<Allpass, 4>, 2> allpasses_;
  std::array<std::vector<float>, 2> echo_;
  size_t echo_index_ = 0;
  std::array<float, 2> echo_store_{};
  float silent_frames_ = 0;
  bool ringing_ = false;
};

}  // namespace slrr
