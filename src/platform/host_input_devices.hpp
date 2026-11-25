#pragma once

#include "slrr/host_input.hpp"

#include <SDL3/SDL_gamepad.h>
#include <array>
#include <map>
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
  // Pad for device 2 and SDL gamepads; Wheel for every other peripheral.
  InputKind kind(int device) const;
  // Channels of a slot: signed axes, buttons, then four per hat.
  int channels(int device) const;
  // The rebind listener reads this slot's raw channels (the primary pad is
  // read through device 2 instead).
  bool capturable(int device) const;
  const std::array<float, 24>& pad() const { return pad_; }

  // Options > Controls (#345): what SDL3 knows about each device slot.
  // Index i is the slot ControlSet calls device i + 3; disconnected devices
  // keep their slot.
  struct Info {
    bool connected = false;
    std::string name, type;
    int axes = 0, buttons = 0, hats = 0;
    int battery = -1;           // percent, -1 unknown
    std::string power;          // "WIRED", "CHARGING", "CHARGED", "BATTERY", ""
    bool rumble = false;
    bool calibrated = false, calibrating = false;
  };
  int count() const { return static_cast<int>(devices_.size()); }
  // Asks SDL for the device list again now (Options > Devices > REFRESH).
  void refresh();
  Info info(int index) const;
  // Strongest input on the device: half the axis distance from rest, or a button.
  float activity(int index) const;
  bool rumble(int index, float strength, int milliseconds);
  // Axis range calibration: while calibrating, the raw minimum and maximum
  // of every axis are recorded; ending stores them and maps each axis range
  // onto -1..1. Saved per device name in save/game/openslrr_axis_calibration.
  void set_calibration_file(std::string path);
  void calibrate_begin(int index);
  bool calibrate_end(int index);
  void calibrate_reset(int index);

 private:
  struct Device {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    SDL_Gamepad* gamepad = nullptr;
    // An SDL gamepad mapping on a wheel (some Logitech/Thrustmaster bases have
    // one) does not make it a pad: it keeps its raw channels and filters.
    bool pad = false;
    std::string name;
    int axes = 0, buttons = 0, hats = 0;
    std::vector<float> values, rest;
    bool calibrating = false;
    std::vector<float> seen_lo, seen_hi;
  };
  struct Range { float lo = -1.f, hi = 1.f; };
  std::map<std::string, std::map<int, Range>> calibration_;
  std::string calibration_file_;
  void save_calibration() const;
  float calibrated(const Device& device, int axis, float raw) const;
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
