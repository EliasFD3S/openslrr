#include "host_input_devices.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <iterator>
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
  SDL_SetJoystickEventsEnabled(false);
  SDL_SetGamepadEventsEnabled(false);
  // SDL 3.2's helpers omit the update-complete events; this polling-only host
  // has no SDL event consumer, so those must not accumulate either.
  SDL_SetEventEnabled(SDL_EVENT_JOYSTICK_UPDATE_COMPLETE, false);
  SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_UPDATE_COMPLETE, false);
  poll(false);
}

void HostInputDevices::close(Device& device) {
  if (device.gamepad) SDL_CloseGamepad(device.gamepad);
  else if (device.joystick) SDL_CloseJoystick(device.joystick);
  device.gamepad = nullptr;
  device.joystick = nullptr;
  device.id = 0;
  std::fill(device.values.begin(), device.values.end(), 0.f);
}

HostInputDevices::~HostInputDevices() {
  for (auto& device : devices_) close(device);
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
    incoming.name = device_name(incoming.joystick);
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

  // Keep the legacy alias on the same gamepad until it is unplugged.
  auto primary = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) {
    return d.id == primary_ && d.gamepad;
  });
  if (primary == devices_.end())
    primary = std::find_if(devices_.begin(), devices_.end(), [](const Device& d) { return d.gamepad != nullptr; });
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
    for (int i = 0; i < device.axes; ++i)
      device.values[channel++] = normalized(SDL_GetJoystickAxis(device.joystick, i));
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
  // Preserve the old split-positive channels, deadzones and SDL's inverted Y.
  pad_ = {stick(-lx, 7849), stick(lx, 7849), stick(-ly, 7849), stick(ly, 7849),
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

HostInputDevices& host_input_devices() {
  static HostInputDevices devices;
  return devices;
}
}  // namespace slrr
