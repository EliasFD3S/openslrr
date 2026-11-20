#pragma once

#include <SDL3/SDL_gamepad.h>
#include <array>
#include <string>
#include <vector>

namespace slrr {

// SDL owns peripheral discovery only; Win32 remains the window/keyboard/mouse owner.
// Slots never move during a session, including while a device is disconnected.
class HostInputDevices {
 public:
  HostInputDevices();
  ~HostInputDevices();
  HostInputDevices(const HostInputDevices&) = delete;
  HostInputDevices& operator=(const HostInputDevices&) = delete;
  void poll(bool focused);
  const char* name(int device) const;
  std::string axis_name(int device, int axis) const;
  float axis(int device, int axis) const;
  int active_axis(int device) const;
  bool analog_axis(int device, int axis) const;
  bool available(int device, int axis) const;
  const std::array<float, 24>& pad() const { return pad_; }

 private:
  struct Device {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    SDL_Gamepad* gamepad = nullptr;
    std::string name;
    int axes = 0, buttons = 0, hats = 0;
    std::vector<float> values, rest;
  };
  bool initialized_ = false;
  bool focused_ = false;
  std::vector<Device> devices_;
  std::array<float, 24> pad_{};
  SDL_JoystickID primary_ = 0;
  const Device* device(int slot) const;
  static void close(Device& device);
};

HostInputDevices& host_input_devices();

}  // namespace slrr
