#pragma once
// Rebind listener (Options > Controls): which physical input the player means.
//
// The old listener waited until every channel of every device was at rest,
// then took the first active one in device order. One channel that never
// rests (an H-shifter in gear, a toggle on a button box, a rotary encoder
// position, a pedal parked off its end stop) blocked it forever, and the first
// device in the list won over the one the player touched.
//
// This one snapshots every channel once the controls settle, ignores what is
// still held, and takes the channel that moves farthest from its snapshot:
//   Releasing  waits until no channel changes for kSettle (at most kSettleMax),
//   Listening  waits for a button pressed or an axis moved from its rest,
//   Holding    tracks the peak and waits for the release (at most kHoldMax),
//   Done       holds the result until begin() or cancel().
// Pure state machine: the host feeds it the channels every frame.
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace slrr {

struct CaptureChannel {
  int device = -1, axis = -1;
  float value = 0.f;
  bool analog = false;   // a signed or split axis; false for keys and buttons
  bool cancels = false;  // a press cancels the capture (the left mouse button)
};

class InputCapture {
 public:
  enum State { Idle, Releasing, Listening, Holding, Done, Cancelled };
  static constexpr double kSettle = .30, kSettleMax = 2.0, kHoldMax = 2.0;
  static constexpr float kStill = .05f;     // change that counts as movement
  static constexpr float kButton = .5f;     // press threshold above its rest
  static constexpr float kAxis = .5f;       // a quarter of a -1..1 axis
  static constexpr float kReleased = .25f;  // back near the rest

  void begin(double now) {
    *this = {};
    state_ = Releasing;
    start_ = last_change_ = now;
  }
  void cancel() { if (state_ != Idle) state_ = Cancelled; }
  void reset() { *this = {}; }

  void update(double now, const std::vector<CaptureChannel>& channels) {
    switch (state_) {
      case Releasing: settle(now, channels); break;
      case Listening: listen(now, channels); break;
      case Holding: hold(now, channels); break;
      default: break;
    }
  }

  State state() const { return state_; }
  int device() const { return device_; }
  int axis() const { return axis_; }
  float rest() const { return rest_; }
  float peak() const { return peak_; }
  bool analog() const { return analog_; }
  // Channels ignored because they did not return to rest before listening.
  int held() const { return held_; }
  // 0..1 while Releasing, for a "release every control" meter.
  float settle_progress(double now) const {
    if (state_ != Releasing) return state_ == Idle ? 0.f : 1.f;
    return static_cast<float>(std::clamp((now - last_change_) / kSettle, 0.0, 1.0));
  }

 private:
  using Key = std::pair<int, int>;
  State state_ = Idle;
  double start_ = 0, last_change_ = 0, hold_start_ = 0;
  std::map<Key, float> last_, rest_of_;
  int device_ = -1, axis_ = -1, held_ = 0;
  float rest_ = 0.f, peak_ = 0.f;
  bool analog_ = false;

  void settle(double now, const std::vector<CaptureChannel>& channels) {
    for (const auto& c : channels) {
      const Key key{c.device, c.axis};
      const auto it = last_.find(key);
      if (it != last_.end() && std::abs(c.value - it->second) > kStill) last_change_ = now;
      last_[key] = c.value;
    }
    if (now - last_change_ < kSettle && now - start_ < kSettleMax) return;
    rest_of_ = last_;
    held_ = 0;
    for (const auto& c : channels)
      if (!c.analog && c.value > kButton) ++held_;
    state_ = Listening;
  }

  void listen(double now, const std::vector<CaptureChannel>& channels) {
    const CaptureChannel* best = nullptr;
    float best_score = 0.f, best_rest = 0.f;
    for (const auto& c : channels) {
      const auto [it, fresh] = rest_of_.try_emplace({c.device, c.axis}, c.value);
      if (fresh) continue;  // a device connected meanwhile: this is its rest
      // A held button rests at 0 once let go, so a later press counts.
      if (!c.analog) it->second = std::min(it->second, c.value);
      const float delta = c.value - it->second;
      // Above 1 past the threshold; the pad alias' split axes span only 0..1.
      const float score = c.analog ? std::abs(delta) / kAxis : delta / kButton;
      if (score > 1.f && score > best_score) {
        best = &c;
        best_score = score;
        best_rest = it->second;
      }
    }
    if (!best) return;
    if (best->cancels) {
      state_ = Cancelled;
      return;
    }
    device_ = best->device;
    axis_ = best->axis;
    analog_ = best->analog;
    rest_ = best_rest;
    peak_ = best->value;
    hold_start_ = now;
    state_ = Holding;
  }

  void hold(double now, const std::vector<CaptureChannel>& channels) {
    const auto it = std::find_if(channels.begin(), channels.end(), [&](const CaptureChannel& c) {
      return c.device == device_ && c.axis == axis_;
    });
    if (it == channels.end()) {  // unplugged or focus lost: keep what was seen
      state_ = Done;
      return;
    }
    if (std::abs(it->value - rest_) > std::abs(peak_ - rest_)) peak_ = it->value;
    const bool released = analog_ ? std::abs(it->value - rest_) < kReleased
                                  : it->value - rest_ < kButton;
    // A toggle switch never comes back: accept it after a while.
    if (released || now - hold_start_ >= kHoldMax) state_ = Done;
  }
};

}  // namespace slrr
