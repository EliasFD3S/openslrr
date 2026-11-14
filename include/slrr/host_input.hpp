#pragma once

#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>

namespace slrr {

struct Object;
class Jvm;

struct InputBinding {
  int axis, device, physical;
  float from, to, low, high;
};

struct InputSmoothing {
  float center, toward_center, opposite, same, power;
  float value = 0.f;
  float speed = 1.f;
};

struct ControllerInput {
  std::vector<InputBinding> bindings;
  std::unordered_map<int, InputSmoothing> smoothing;
  std::unordered_map<int, float> forced_axes;
};

enum class InputBindingSource : std::int32_t {
  MissingController = 0,
  Unbound = 1,
  Forced = 2,
  Profile = 3,
  XInputFallback = 4,
  ProfileAndXInputFallback = 5,
};
enum class InputFallbackStatus : std::int32_t {
  NotEligible = 0,
  Available = 1,
  SuppressedByXInputProfile = 2,
  Overridden = 3,
};
struct InputAxisBindingDiagnostics {
  std::int32_t axis = 0;
  InputBindingSource source = InputBindingSource::MissingController;
  InputFallbackStatus fallback = InputFallbackStatus::NotEligible;
  std::int32_t profile_bindings = 0, xinput_bindings = 0;
  bool smoothed = false;
};

// Pure binding inspection: it does not read keys, text history, or axis values.
inline InputAxisBindingDiagnostics input_axis_binding_diagnostics(
    const ControllerInput* input, std::int32_t axis) {
  InputAxisBindingDiagnostics out;
  out.axis = axis;
  if (!input) return out;
  out.smoothed = input->smoothing.contains(axis);
  const bool forced = input->forced_axes.contains(axis);
  bool any_xinput = false;
  for (const auto& binding : input->bindings) {
    any_xinput |= binding.device == 2;
    if (binding.from == binding.to) continue;
    if (binding.axis != axis) continue;
    ++out.profile_bindings;
    out.xinput_bindings += binding.device == 2;
  }
  const bool eligible = axis == 1 || axis == 28 || axis == 29 || axis == 48;
  out.fallback = forced ? InputFallbackStatus::Overridden
      : !eligible ? InputFallbackStatus::NotEligible
      : any_xinput ? InputFallbackStatus::SuppressedByXInputProfile
      : InputFallbackStatus::Available;
  out.source = forced ? InputBindingSource::Forced
      : out.profile_bindings == 0
          ? (out.fallback == InputFallbackStatus::Available
                 ? InputBindingSource::XInputFallback : InputBindingSource::Unbound)
          : (out.fallback == InputFallbackStatus::Available
                 ? InputBindingSource::ProfileAndXInputFallback : InputBindingSource::Profile);
  return out;
}

const char* input_binding_source_name(InputBindingSource source);
const char* input_fallback_status_name(InputFallbackStatus status);
InputAxisBindingDiagnostics host_input_controller_axis_diagnostics(
    const Object* controller, std::int32_t axis);

// Shared Win32 → script and camera orbit input.
struct HostInput {
  // Instantaneous look axes (consumed / decayed each tick).
  float look_yaw = 0.f;    // AXIS_LOOK_LEFTRIGHT (7)
  float look_pitch = 0.f;  // AXIS_LOOK_UPDOWN (8)
  // Raw right-drag pixels since the last camera step. +x is right, +y is down.
  float mouse_dx = 0.f;
  float mouse_dy = 0.f;
  float look_zoom = 0.f;   // AXIS_LOOK_FWDBACK (10)
  float cursor_x = 0.f;
  float cursor_y = 0.f;
  bool rmb_down = false;
  // Client pixels. part_click is a left release that missed every OSD widget.
  int pointer_x = 0;
  int pointer_y = 0;
  int view_w = 0;
  int view_h = 0;
  bool pointer_valid = false;
  bool debug_mouse_capture = false;
  bool part_click = false;
  int part_click_x = 0;
  int part_click_y = 0;
  bool keys_left = false;
  bool keys_right = false;
  bool keys_up = false;
  bool keys_down = false;
  bool keys_pgup = false;
  bool keys_pgdn = false;
  float drive_steer = 0, drive_throttle = 0, drive_brake = 0;
  bool drive_steer_analog = false;
  float drive_clutch = 0.f, drive_shift = 0.f;
  float free_camera_x = 0.f, free_camera_y = 0.f, free_camera_z = 0.f;
  bool drive_horn = false;
  bool drive_reverse = false, drive_handbrake = false;
  // --bench-drive: >= 0 replaces throttle, brake and steering.
  float bench_throttle = -1.f;
};

HostInput& host_input();
void host_input_char(int ch);
const std::string& host_input_history();
void host_input_clear_history();

// Poll keyboard held keys into HostInput (call each frame).
void host_input_poll_keys();

float host_input_camera_axis(int axis);
const char* host_input_device_name(int device);
std::string host_input_axis_name(int device, int axis);
float host_input_physical_axis(int device, int axis);
int host_input_active_axis(int device);
void host_input_key(int scan, bool down);
void host_input_clear_keys();
int host_input_last_key();
float host_input_controller_axis(const Object* controller, int axis);
void host_input_update_controller(Object* controller, float dt);
Object* host_input_player_controller(Jvm& jvm);
void host_input_create_hotkey(Jvm& jvm, Object* key, Object* handler,
                              Object* owner, int axis, int flags, int events);
void host_input_delete_hotkey(Object* key);
void host_input_forget(Object* object);
void host_input_check_hotkeys(Jvm& jvm, Object* controller, Object* focus);
// One press in a menu or dialog. The same id is ignored for a short while so
// it cannot run twice back to back. A key release still passes. Driving axes
// never come through here.
bool host_input_allow_ui(int id, bool pressed);
void host_input_flush_hotkeys(Object* controller);

}  // namespace slrr
