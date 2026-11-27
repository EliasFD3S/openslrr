#include "host_input_devices.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_properties.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace slrr {
namespace {
constexpr const char* pad_axes[] = {
    "Left Stick Left", "Left Stick Right", "Left Stick Up", "Left Stick Down",
    "Right Stick Left", "Right Stick Right", "Right Stick Up", "Right Stick Down",
    "LT", "RT", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
    "Start", "Back", "Left Stick Click", "Right Stick Click", "LB", "RB",
    "A", "B", "X", "Y"};

std::string device_name(SDL_Joystick* joystick) {
  char guid[33]{};
  SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid, sizeof(guid));
  const char* serial = SDL_GetJoystickSerial(joystick);
  const char* path = SDL_GetJoystickPath(joystick);
  const std::string identity = std::string(guid) + ":" +
      (serial && *serial ? serial : path ? path : "");
  // Keep the UI label short, but distinguish identical products on different ports.
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : identity) hash = (hash ^ c) * 1099511628211ull;
  char suffix[24]{};
  std::snprintf(suffix, sizeof(suffix), " [%016llx]", static_cast<unsigned long long>(hash));
  const char* name = SDL_GetJoystickName(joystick);
  return std::string(name ? name : "SDL Joystick") + suffix;
}

float normalized(Sint16 value) {
  return value < 0 ? value / 32768.f : value / 32767.f;
}
}  // namespace

HostInputDevices::HostInputDevices() {
  // There is no SDL window: read peripherals in the background and apply the
  // existing Win32 foreground-process gate ourselves, including on focus loss.
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!initialized_) {
    std::fprintf(stderr, "[input] SDL3 initialization failed: %s\n", SDL_GetError());
    return;
  }
  // Force feedback is optional: a failure only leaves every wheel without it.
  haptic_ready_ = SDL_InitSubSystem(SDL_INIT_HAPTIC);
  if (!haptic_ready_) std::fprintf(stderr, "[input] SDL3 haptics unavailable: %s\n", SDL_GetError());
  SDL_SetJoystickEventsEnabled(false);
  SDL_SetGamepadEventsEnabled(false);
  // SDL 3.2's helpers omit the update-complete events; this polling-only host
  // has no SDL event consumer, so those must not accumulate either.
  SDL_SetEventEnabled(SDL_EVENT_JOYSTICK_UPDATE_COMPLETE, false);
  SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_UPDATE_COMPLETE, false);
  poll(false);
}

void HostInputDevices::open_haptic(Device& d) {
  if (!d.joystick || !SDL_IsJoystickHaptic(d.joystick)) return;
  d.haptic = SDL_OpenHapticFromJoystick(d.joystick);
  if (!d.haptic) return;
  const Uint32 features = SDL_GetHapticFeatures(d.haptic);
  if (features & SDL_HAPTIC_CONSTANT) {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_CONSTANT;
    // SDL picks the wheel's steering axis; the level's sign is the direction.
    effect.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.constant.length = SDL_HAPTIC_INFINITY;
    d.constant = SDL_CreateHapticEffect(d.haptic, &effect);
  }
  if (d.constant < 0) {  // rumble-only pads keep SDL_RumbleJoystick
    SDL_CloseHaptic(d.haptic);
    d.haptic = nullptr;
    return;
  }
  if (features & SDL_HAPTIC_DAMPER) {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_DAMPER;
    effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.condition.length = SDL_HAPTIC_INFINITY;
    effect.condition.right_sat[0] = effect.condition.left_sat[0] = 0xFFFF;
    d.damper = SDL_CreateHapticEffect(d.haptic, &effect);
  }
  if (features & SDL_HAPTIC_SINE) {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_SINE;
    effect.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.periodic.length = SDL_HAPTIC_INFINITY;
    effect.periodic.period = 40;
    d.sine = SDL_CreateHapticEffect(d.haptic, &effect);
  }
  if (features & SDL_HAPTIC_GAIN) SDL_SetHapticGain(d.haptic, 100);
  // The game's torque replaces the wheel's own spring.
  if (features & SDL_HAPTIC_AUTOCENTER) SDL_SetHapticAutocenter(d.haptic, 0);
}

void HostInputDevices::stop_haptic(Device& d) {
  if (!d.haptic || !d.playing) return;
  if (d.constant >= 0) SDL_StopHapticEffect(d.haptic, d.constant);
  if (d.damper >= 0) SDL_StopHapticEffect(d.haptic, d.damper);
  if (d.sine >= 0 && d.shaking) SDL_StopHapticEffect(d.haptic, d.sine);
  d.playing = d.shaking = false;
  d.level = d.damping = d.shake = d.shake_hz = 0;
}

void HostInputDevices::close(Device& device) {
  stop_haptic(device);
  if (device.haptic) SDL_CloseHaptic(device.haptic);
  device.haptic = nullptr;
  device.constant = device.damper = device.sine = -1;
  if (device.gamepad) SDL_CloseGamepad(device.gamepad);
  else if (device.joystick) SDL_CloseJoystick(device.joystick);
  device.gamepad = nullptr;
  device.joystick = nullptr;
  device.id = 0;
  std::fill(device.values.begin(), device.values.end(), 0.f);
}

HostInputDevices::~HostInputDevices() {
  for (auto& device : devices_) close(device);
  if (haptic_ready_) SDL_QuitSubSystem(SDL_INIT_HAPTIC);
  if (initialized_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void HostInputDevices::poll(bool focused) {
  focused_ = focused;
  pad_.fill(0.f);
  if (!initialized_) return;
  SDL_UpdateGamepads();
  for (auto& device : devices_)
    if (device.joystick && !SDL_JoystickConnected(device.joystick)) close(device);

  int count = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&count);
  for (int i = 0; i < count; ++i) {
    if (std::any_of(devices_.begin(), devices_.end(), [&](const Device& d) { return d.id == ids[i]; }))
      continue;
    Device incoming;
    incoming.id = ids[i];
    if (SDL_IsGamepad(ids[i])) incoming.gamepad = SDL_OpenGamepad(ids[i]);
    incoming.joystick = incoming.gamepad ? SDL_GetGamepadJoystick(incoming.gamepad) : SDL_OpenJoystick(ids[i]);
    if (!incoming.joystick) continue;
    const auto type = SDL_GetJoystickType(incoming.joystick);
    incoming.pad = incoming.gamepad &&
        (type == SDL_JOYSTICK_TYPE_GAMEPAD || type == SDL_JOYSTICK_TYPE_UNKNOWN);
    incoming.name = device_name(incoming.joystick);
    if (haptic_ready_) open_haptic(incoming);
    // Identical devices without a serial/path can only be distinguished by
    // discovery order. Do not collapse two simultaneously connected peripherals.
    const auto base_name = incoming.name;
    for (int ordinal = 2; std::any_of(devices_.begin(), devices_.end(), [&](const Device& d) {
           return d.id && d.name == incoming.name;
         }); ++ordinal)
      incoming.name = base_name + " #" + std::to_string(ordinal);
    incoming.axes = std::max(0, SDL_GetNumJoystickAxes(incoming.joystick));
    incoming.buttons = std::max(0, SDL_GetNumJoystickButtons(incoming.joystick));
    incoming.hats = std::max(0, SDL_GetNumJoystickHats(incoming.joystick));
    incoming.values.resize(incoming.axes + incoming.buttons + incoming.hats * 4);
    for (int axis = 0; axis < incoming.axes; ++axis)
      incoming.rest.push_back(normalized(SDL_GetJoystickAxis(incoming.joystick, axis)));
    auto slot = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) {
      return !d.id && d.name == incoming.name;
    });
    if (slot == devices_.end()) devices_.push_back(std::move(incoming));
    else *slot = std::move(incoming);
  }
  SDL_free(ids);

  if (!focused) stop_force();
  if (test_slot_ >= 0) {
    // TEST: half force one way, then the other, then off.
    const double left = test_until_ - SDL_GetTicks() / 1000.0;
    Device* d = slot_device(test_slot_);
    if (!d || !d->haptic || left <= 0 || !focused) {
      if (d) stop_haptic(*d);
      test_slot_ = -1;
    } else {
      send_force(*d, left > .3 ? .5f : -.5f, .2f);
    }
  }

  // Keep the legacy alias on the same gamepad until it is unplugged.
  auto primary = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) {
    return d.id == primary_ && d.pad && d.gamepad;
  });
  if (primary == devices_.end())
    primary = std::find_if(devices_.begin(), devices_.end(), [](const Device& d) { return d.pad && d.gamepad; });
  primary_ = primary == devices_.end() ? 0 : primary->id;

  for (auto& device : devices_) {
    std::fill(device.values.begin(), device.values.end(), 0.f);
    if (!device.joystick) continue;
    for (int i = 0; i < device.axes; ++i) {
      Sint16 initial = 0;
      if (SDL_GetJoystickAxisInitialState(device.joystick, i, &initial))
        device.rest[i] = normalized(initial);
    }
    if (!focused) continue;
    int channel = 0;
    for (int i = 0; i < device.axes; ++i) {
      const float raw = normalized(SDL_GetJoystickAxis(device.joystick, i));
      if (device.calibrating) {
        device.seen_lo[i] = std::min(device.seen_lo[i], raw);
        device.seen_hi[i] = std::max(device.seen_hi[i], raw);
      }
      device.values[channel++] = calibrated(device, i, raw);
    }
    for (int i = 0; i < device.buttons; ++i)
      device.values[channel++] = SDL_GetJoystickButton(device.joystick, i) ? 1.f : 0.f;
    for (int i = 0; i < device.hats; ++i) {
      const auto hat = SDL_GetJoystickHat(device.joystick, i);
      for (int direction : {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT})
        device.values[channel++] = (hat & direction) ? 1.f : 0.f;
    }
  }
  if (!focused || primary == devices_.end()) return;
  SDL_Gamepad* gamepad = primary->gamepad;
  auto stick = [](int value, int deadzone) {
    return std::clamp((value - deadzone) / (32767.f - deadzone), 0.f, 1.f);
  };
  const int lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
  const int ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
  const int rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
  const int ry = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
  auto trigger = [&](SDL_GamepadAxis axis) {
    return std::clamp((SDL_GetGamepadAxis(gamepad, axis) / 32767.f - 30.f / 255.f) /
                      (1.f - 30.f / 255.f), 0.f, 1.f);
  };
  // Preserve the old split-positive channels and SDL's inverted Y. The left
  // stick steers: its dead zone is only a noise floor (5 %), and the
  // control set's STEERING DEAD ZONE adds the player's own. The XInput
  // default (7849, 24 %) cut a quarter of the stick (#383).
  constexpr int kLeftStickFloor = 1600;
  pad_ = {stick(-lx, kLeftStickFloor), stick(lx, kLeftStickFloor),
          stick(-ly, kLeftStickFloor), stick(ly, kLeftStickFloor),
          stick(-rx, 8689), stick(rx, 8689), stick(-ry, 8689), stick(ry, 8689),
          trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)};
  constexpr SDL_GamepadButton buttons[] = {
      SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
      SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
      SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
      SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
      SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
  for (size_t i = 0; i < std::size(buttons); ++i)
    pad_[10 + i] = SDL_GetGamepadButton(gamepad, buttons[i]) ? 1.f : 0.f;
}

const HostInputDevices::Device* HostInputDevices::device(int slot) const {
  return slot >= 3 && static_cast<size_t>(slot - 3) < devices_.size() ? &devices_[slot - 3] : nullptr;
}

const char* HostInputDevices::name(int slot) const {
  // This string is the saved ControlSet ABI, not the name of the backend.
  if (slot == 2) return "XInput Gamepad";
  const auto* d = device(slot);
  return d ? d->name.c_str() : nullptr;
}

std::string HostInputDevices::axis_name(int slot, int axis) const {
  if (slot == 2 && axis >= 0 && axis < static_cast<int>(pad_.size()))
    return std::string("Pad ") + pad_axes[axis];
  const auto* d = device(slot);
  if (!d || axis < 0 || axis >= static_cast<int>(d->values.size())) return "Undefined";
  const std::string prefix = d->name + " ";
  if (axis < d->axes) return prefix + "Axis " + std::to_string(axis + 1);
  axis -= d->axes;
  if (axis < d->buttons) return prefix + "Button " + std::to_string(axis + 1);
  axis -= d->buttons;
  constexpr const char* directions[] = {"Up", "Down", "Left", "Right"};
  return prefix + "Hat " + std::to_string(axis / 4 + 1) + " " + directions[axis % 4];
}

float HostInputDevices::axis(int slot, int axis) const {
  if (axis < 0) return 0.f;
  if (slot == 2) return axis < static_cast<int>(pad_.size()) ? pad_[axis] : 0.f;
  const auto* d = device(slot);
  return d && axis < static_cast<int>(d->values.size()) ? d->values[axis] : 0.f;
}

int HostInputDevices::active_axis(int slot) const {
  if (!focused_) return -1;
  if (slot == 2) {
    const auto strongest = std::max_element(pad_.begin(), pad_.end());
    return *strongest > .55f ? static_cast<int>(strongest - pad_.begin()) : -1;
  }
  const auto* d = device(slot);
  // The primary pad is captured through its legacy alias, not twice through raw axes.
  if (!d || !d->joystick || d->id == primary_) return -1;
  for (int i = d->axes; i < static_cast<int>(d->values.size()); ++i)
    if (d->values[i] > .5f) return i;
  for (int i = 0; i < d->axes; ++i)
    // End-stop pedals at rest must not capture a binding before being moved.
    if (available(slot, i) && std::abs(d->values[i] - d->rest[i]) > .55f) return i;
  return -1;
}

bool HostInputDevices::analog_axis(int slot, int axis) const {
  if (slot == 2) return axis >= 0 && axis < 10;
  const auto* d = device(slot);
  return d && axis >= 0 && axis < d->axes;
}

bool HostInputDevices::available(int slot, int axis) const {
  if (!focused_ || axis < 0) return false;
  if (slot == 2) return primary_ != 0 && axis < static_cast<int>(pad_.size());
  const auto* d = device(slot);
  if (!d || !d->joystick || axis >= static_cast<int>(d->values.size())) return false;
  if (axis >= d->axes) return true;
  return SDL_GetJoystickAxisInitialState(d->joystick, axis, nullptr);
}

InputKind HostInputDevices::kind(int slot) const {
  if (slot == 2) return InputKind::Pad;
  const auto* d = device(slot);
  if (!d) return InputKind::None;
  return d->pad ? InputKind::Pad : InputKind::Wheel;
}

int HostInputDevices::channels(int slot) const {
  if (slot == 2) return static_cast<int>(pad_.size());
  const auto* d = device(slot);
  return d ? static_cast<int>(d->values.size()) : 0;
}

bool HostInputDevices::capturable(int slot) const {
  if (!focused_) return false;
  if (slot == 2) return primary_ != 0;
  const auto* d = device(slot);
  return d && d->joystick && d->id != primary_;
}

void HostInputDevices::refresh() {
  if (!initialized_) return;
  SDL_UpdateJoysticks();
  poll(focused_);
}

float HostInputDevices::calibrated(const Device& device, int axis, float raw) const {
  if (device.calibrating) return raw;
  const auto dev = calibration_.find(device.name);
  if (dev == calibration_.end()) return raw;
  const auto range = dev->second.find(axis);
  if (range == dev->second.end() || range->second.hi - range->second.lo < .05f) return raw;
  return std::clamp((raw - range->second.lo) / (range->second.hi - range->second.lo) * 2.f - 1.f,
                    -1.f, 1.f);
}

HostInputDevices::Info HostInputDevices::info(int index) const {
  Info out;
  if (index < 0 || index >= count()) return out;
  const Device& d = devices_[static_cast<size_t>(index)];
  out.name = d.name;
  // The bracketed hash only tells identical products apart.
  if (const auto cut = out.name.find(" ["); cut != std::string::npos) out.name.resize(cut);
  out.axes = d.axes;
  out.buttons = d.buttons;
  out.hats = d.hats;
  out.calibrating = d.calibrating;
  out.calibrated = calibration_.count(d.name) != 0;
  out.connected = d.joystick != nullptr;
  if (!d.joystick) return out;
  switch (SDL_GetJoystickType(d.joystick)) {
    case SDL_JOYSTICK_TYPE_GAMEPAD: out.type = "GAMEPAD"; break;
    case SDL_JOYSTICK_TYPE_WHEEL: out.type = "WHEEL"; break;
    case SDL_JOYSTICK_TYPE_ARCADE_STICK: out.type = "ARCADE STICK"; break;
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK: out.type = "FLIGHT STICK"; break;
    case SDL_JOYSTICK_TYPE_DANCE_PAD: out.type = "DANCE PAD"; break;
    case SDL_JOYSTICK_TYPE_GUITAR: out.type = "GUITAR"; break;
    case SDL_JOYSTICK_TYPE_DRUM_KIT: out.type = "DRUM KIT"; break;
    case SDL_JOYSTICK_TYPE_ARCADE_PAD: out.type = "ARCADE PAD"; break;
    case SDL_JOYSTICK_TYPE_THROTTLE: out.type = "THROTTLE"; break;
    default: out.type = "JOYSTICK"; break;
  }
  if (d.gamepad) {
    switch (SDL_GetGamepadType(d.gamepad)) {
      case SDL_GAMEPAD_TYPE_XBOX360: out.type = "XBOX 360 PAD"; break;
      case SDL_GAMEPAD_TYPE_XBOXONE: out.type = "XBOX PAD"; break;
      case SDL_GAMEPAD_TYPE_PS3: out.type = "PS3 PAD"; break;
      case SDL_GAMEPAD_TYPE_PS4: out.type = "PS4 PAD"; break;
      case SDL_GAMEPAD_TYPE_PS5: out.type = "PS5 PAD"; break;
      case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: out.type = "SWITCH PRO PAD"; break;
      default: break;
    }
  }
  int percent = -1;
  switch (SDL_GetJoystickPowerInfo(d.joystick, &percent)) {
    case SDL_POWERSTATE_ON_BATTERY: out.power = "BATTERY"; break;
    case SDL_POWERSTATE_CHARGING: out.power = "CHARGING"; break;
    case SDL_POWERSTATE_CHARGED: out.power = "CHARGED"; break;
    case SDL_POWERSTATE_NO_BATTERY: out.power = "WIRED"; break;
    default: break;
  }
  out.battery = percent;
  out.rumble = SDL_GetBooleanProperty(SDL_GetJoystickProperties(d.joystick),
                                      SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
  out.force_feedback = d.haptic && d.constant >= 0;
  return out;
}

float HostInputDevices::activity(int index) const {
  if (index < 0 || index >= count()) return 0.f;
  const Device& d = devices_[static_cast<size_t>(index)];
  float strongest = 0.f;
  for (int i = 0; i < static_cast<int>(d.values.size()); ++i) {
    const float v = i < d.axes ? std::abs(d.values[i] - calibrated(d, i, d.rest[i])) * .5f
                               : d.values[i];
    strongest = std::max(strongest, v);
  }
  return std::clamp(strongest, 0.f, 1.f);
}

bool HostInputDevices::rumble(int index, float strength, int milliseconds) {
  if (index < 0 || index >= count() || !devices_[static_cast<size_t>(index)].joystick) return false;
  const auto level = static_cast<Uint16>(std::clamp(strength, 0.f, 1.f) * 65535.f);
  return SDL_RumbleJoystick(devices_[static_cast<size_t>(index)].joystick, level, level,
                            static_cast<Uint32>(std::clamp(milliseconds, 0, 5000)));
}

HostInputDevices::Device* HostInputDevices::slot_device(int slot) {
  return slot >= 3 && static_cast<size_t>(slot - 3) < devices_.size() ? &devices_[slot - 3] : nullptr;
}

bool HostInputDevices::has_force_feedback(int slot) const {
  const auto* d = device(slot);
  return d && d->haptic && d->constant >= 0;
}

void HostInputDevices::send_force(Device& d, float level, float damper, float vibration,
                                  float vibration_hz) {
  level = std::clamp(level, -1.f, 1.f);
  damper = std::clamp(damper, 0.f, 1.f);
  // DirectInput updates are not free: skip changes below one step in 256.
  if (!d.playing || std::abs(level - d.level) >= 1.f / 256) {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_CONSTANT;
    effect.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.constant.length = SDL_HAPTIC_INFINITY;
    effect.constant.level = static_cast<Sint16>(std::lround(level * 32767.f));
    SDL_UpdateHapticEffect(d.haptic, d.constant, &effect);
    d.level = level;
  }
  if (d.damper >= 0 && (!d.playing || std::abs(damper - d.damping) >= .02f)) {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_DAMPER;
    effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.condition.length = SDL_HAPTIC_INFINITY;
    effect.condition.right_sat[0] = effect.condition.left_sat[0] = 0xFFFF;
    const auto coefficient = static_cast<Sint16>(std::lround(damper * 32767.f));
    effect.condition.right_coeff[0] = effect.condition.left_coeff[0] = coefficient;
    SDL_UpdateHapticEffect(d.haptic, d.damper, &effect);
    d.damping = damper;
  }
  if (!d.playing) {
    SDL_RunHapticEffect(d.haptic, d.constant, 1);
    if (d.damper >= 0) SDL_RunHapticEffect(d.haptic, d.damper, 1);
    d.playing = true;
  }
  if (d.sine < 0) return;
  vibration = std::clamp(vibration, 0.f, 1.f);
  vibration_hz = std::clamp(vibration_hz, 5.f, 100.f);
  if (vibration < 1.f / 128) {
    if (d.shaking) SDL_StopHapticEffect(d.haptic, d.sine);
    d.shaking = false;
    d.shake = 0;
    return;
  }
  if (!d.shaking || std::abs(vibration - d.shake) >= 1.f / 128 ||
      std::abs(vibration_hz - d.shake_hz) >= 1.f) {
    SDL_HapticEffect effect{};
    effect.type = SDL_HAPTIC_SINE;
    effect.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
    effect.periodic.length = SDL_HAPTIC_INFINITY;
    effect.periodic.period = static_cast<Uint16>(std::lround(1000.f / vibration_hz));
    effect.periodic.magnitude = static_cast<Sint16>(std::lround(vibration * 32767.f));
    SDL_UpdateHapticEffect(d.haptic, d.sine, &effect);
    d.shake = vibration;
    d.shake_hz = vibration_hz;
  }
  if (!d.shaking) {
    SDL_RunHapticEffect(d.haptic, d.sine, 1);
    d.shaking = true;
  }
}

void HostInputDevices::set_force(int slot, float level, float damper, float vibration,
                                 float vibration_hz) {
  if (force_slot_ != slot) stop_force();
  Device* d = slot_device(slot);
  if (!focused_ || !d || !d->haptic || d->constant < 0 || slot == test_slot_) return;
  force_slot_ = slot;
  send_force(*d, level, damper, vibration, vibration_hz);
}

void HostInputDevices::stop_force() {
  if (Device* d = slot_device(force_slot_)) stop_haptic(*d);
  force_slot_ = -1;
}

bool HostInputDevices::test_force(int index) {
  const int slot = index + 3;
  if (!has_force_feedback(slot) || !focused_) return false;
  if (force_slot_ == slot) stop_force();
  test_slot_ = slot;
  test_until_ = SDL_GetTicks() / 1000.0 + .6;
  return true;
}

void HostInputDevices::set_calibration_file(std::string path) {
  calibration_file_ = std::move(path);
  calibration_.clear();
  std::ifstream in(calibration_file_);
  // One axis per line: device name, tab, axis, minimum, maximum.
  for (std::string line; std::getline(in, line);) {
    const auto tab = line.rfind('\t');
    if (tab == std::string::npos) continue;
    std::istringstream fields(line.substr(tab + 1));
    int axis = -1;
    Range range;
    if (fields >> axis >> range.lo >> range.hi && axis >= 0 && range.hi > range.lo)
      calibration_[line.substr(0, tab)][axis] = range;
  }
}

void HostInputDevices::save_calibration() const {
  if (calibration_file_.empty()) return;
  std::ofstream out(calibration_file_, std::ios::trunc);
  for (const auto& [name, axes] : calibration_)
    for (const auto& [axis, range] : axes)
      out << name << '\t' << axis << ' ' << range.lo << ' ' << range.hi << '\n';
  if (!out) std::fprintf(stderr, "[input] could not save %s\n", calibration_file_.c_str());
}

void HostInputDevices::calibrate_begin(int index) {
  if (index < 0 || index >= count()) return;
  Device& d = devices_[static_cast<size_t>(index)];
  d.calibrating = true;
  d.seen_lo.assign(static_cast<size_t>(d.axes), 1.f);
  d.seen_hi.assign(static_cast<size_t>(d.axes), -1.f);
}

bool HostInputDevices::calibrate_end(int index) {
  if (index < 0 || index >= count()) return false;
  Device& d = devices_[static_cast<size_t>(index)];
  if (!d.calibrating) return false;
  d.calibrating = false;
  std::map<int, Range> axes;
  // An axis that never moved keeps the full range.
  for (int i = 0; i < d.axes; ++i)
    if (d.seen_hi[i] - d.seen_lo[i] >= .2f) axes[i] = Range{d.seen_lo[i], d.seen_hi[i]};
  if (axes.empty()) return false;
  calibration_[d.name] = std::move(axes);
  save_calibration();
  return true;
}

void HostInputDevices::calibrate_reset(int index) {
  if (index < 0 || index >= count()) return;
  Device& d = devices_[static_cast<size_t>(index)];
  d.calibrating = false;
  calibration_.erase(d.name);
  save_calibration();
}

HostInputDevices& host_input_devices() {
  static HostInputDevices devices;
  return devices;
}
}  // namespace slrr
