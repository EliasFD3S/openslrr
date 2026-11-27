#pragma once
#include "slrr/host_input.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

namespace slrr::input_mapping {
constexpr int kPadDevice = 2;

inline std::optional<float> pad_fallback(const ControllerInput& input, int axis,
                                         std::span<const float> pad) {
  if (axis != 1 && axis != 28 && axis != 29 && axis != 48) return std::nullopt;
  for (const auto& binding : input.bindings)
    if (binding.device >= kPadDevice && binding.axis == axis) return std::nullopt;
  auto available = [&](int physical) {
    return std::none_of(input.bindings.begin(), input.bindings.end(), [&](const InputBinding& b) {
      return b.device == kPadDevice && b.physical == physical;
    });
  };
  auto value = [&](int physical) {
    return physical < static_cast<int>(pad.size()) && available(physical) ? pad[physical] : 0.f;
  };
  switch (axis) {
    case 1: return value(0) - value(1);
    case 28: return value(9);
    case 29: return value(8);
    case 48: return value(21);
  }
  return std::nullopt;
}

struct AllAvailable {
  bool operator()(int, int) const { return true; }
};
struct LegacyAnalog {
  bool operator()(int device, int axis) const { return device == kPadDevice && axis >= 0 && axis < 10; }
};

inline float binding_value(const InputBinding& binding, float value) {
  if (binding.from == binding.to) return 0.f;
  const float t = std::clamp((value - binding.from) / (binding.to - binding.from), 0.f, 1.f);
  return std::lerp(binding.low, binding.high, t);
}

template<class Physical, class Available = AllAvailable>
float mapped_axis(const ControllerInput& input, int axis, Physical physical,
                  std::span<const float> pad, Available available = {}) {
  float positive = 0.f, negative = 0.f;
  for (const auto& binding : input.bindings) {
    if (binding.axis != axis || !available(binding.device, binding.physical)) continue;
    const float mapped = binding_value(binding, physical(binding.device, binding.physical));
    positive = std::max(positive, mapped);
    negative = std::min(negative, mapped);
  }
  if (const auto fallback = pad_fallback(input, axis, pad)) {
    if (axis == 1) {
      if (positive + negative == 0.f) return *fallback;
    } else return std::max(positive, *fallback);
  }
  return positive + negative;
}

template<class Physical, class Analog = LegacyAnalog, class Available = AllAvailable>
bool steering_is_analog(const ControllerInput& input, Physical physical,
                         std::span<const float> pad, Analog analog_axis = {}, Available available = {}) {
  bool analog = false, digital = false;
  for (const auto& binding : input.bindings) {
    if (binding.axis != 1 || !available(binding.device, binding.physical)) continue;
    const bool analog_binding = analog_axis(binding.device, binding.physical);
    const bool active = std::abs(binding_value(binding, physical(binding.device, binding.physical))) > .01f;
    analog |= analog_binding && active;
    digital |= !analog_binding && active;
  }
  const auto fallback = pad_fallback(input, 1, pad);
  analog |= fallback && std::abs(*fallback) > .01f;
  return analog && !digital;
}
// Class of the device steering now, for the per-class filters. An active key
// or button wins, as in steering_is_analog; then a wheel axis, then a pad stick.
template<class Physical, class Analog, class Kind, class Available = AllAvailable>
InputKind steering_kind(const ControllerInput& input, Physical physical, std::span<const float> pad,
                        Analog analog_axis, Kind kind, Available available = {}) {
  InputKind digital = InputKind::None;
  bool wheel = false, stick = false;
  for (const auto& binding : input.bindings) {
    if (binding.axis != 1 || !available(binding.device, binding.physical)) continue;
    if (std::abs(binding_value(binding, physical(binding.device, binding.physical))) <= .01f) continue;
    const InputKind k = kind(binding.device);
    if (!analog_axis(binding.device, binding.physical)) {
      if (digital == InputKind::None) digital = k;
    } else if (k == InputKind::Wheel) wheel = true;
    else stick = true;
  }
  if (digital != InputKind::None) return digital;
  if (wheel) return InputKind::Wheel;
  const auto fallback = pad_fallback(input, 1, pad);
  if (stick || (fallback && std::abs(*fallback) > .01f)) return InputKind::Pad;
  return InputKind::None;
}
}  // namespace slrr::input_mapping
