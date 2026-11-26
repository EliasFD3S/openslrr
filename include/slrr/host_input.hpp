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

// Device class of a ControlSet device id. Each class gets its own filters:
// keys are digital and ramped, pad sticks have a dead zone, a wheel and its
// pedals, shifter and button boxes ("Wheel": every SDL peripheral that is not a
// gamepad) are read as they are.
enum class InputKind { None = -1, Keyboard = 0, Mouse = 1, Pad = 2, Wheel = 3 };

struct ControllerInput {
  std::vector<InputBinding> bindings;
  std::unordered_map<int, InputSmoothing> smoothing;
  std::unordered_map<int, float> forced_axes;
};

// Shared Win32 keyboard/mouse and SDL3 peripherals → script and camera orbit input.
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
  // Left button pressed on the 3D view (no OSD widget, modal or movie under
  // it) and still held; press/release are edges for the next world pointer
  // dispatch (EC_LDOWN / EC_LUP).
  bool world_button = false;
  bool world_press = false;
  bool world_release = false;
  // Bumped when a loading plate or scene transition starts or ends. A
  // button held across it is cancelled: its release is not a click.
  uint32_t pointer_epoch = 0;
  // Win32 tick of the last cancel. A synchronous load queues the clicks made
  // during it; button messages stamped before this are dropped.
  uint32_t pointer_cancel_ms = 0;
  bool keys_pgup = false;
  bool keys_pgdn = false;
  float drive_steer = 0, drive_throttle = 0, drive_brake = 0;
  bool drive_steer_analog = false;
  // Class of the device steering now (None when nothing steers): the steering
  // filters differ for keys, pad sticks and wheels.
  InputKind drive_steer_kind = InputKind::None;
  float drive_clutch = 0.f, drive_shift = 0.f;
  float free_camera_x = 0.f, free_camera_y = 0.f, free_camera_z = 0.f;
  bool drive_horn = false;
  float drive_nitro = 0.f;
  bool drive_reverse = false;
  // Handbrake pull, 0..1: a key or button gives 0 or 1, an analog lever or
  // axis anything between.
  float drive_handbrake = 0.f;
  // --bench-drive: >= 0 replaces throttle, brake and steering.
  float bench_throttle = -1.f;
};

HostInput& host_input();
void host_input_char(int ch);
const std::string& host_input_history();
void host_input_clear_history();

// Poll keyboard held keys into HostInput (call each frame).
void host_input_poll_keys();

// Camera look axes. 7/8 are the controller's look bindings without mouse
// ones (the drag is look_* / mouse_d*); 10 is the zoom.
float host_input_camera_axis(const Object* controller, int axis);
// Drop latched clicks and world press edges, release a held world button and
// cancel every gesture still held (see pointer_epoch).
void host_input_cancel_pointer_gestures();
const char* host_input_device_name(int device);
std::string host_input_axis_name(int device, int axis);
float host_input_physical_axis(int device, int axis);
int host_input_active_axis(int device);
InputKind host_input_device_kind(int device);
// Force feedback on the wheel that steers the player's car. torque is the
// steering torque in logical units (-1..1, positive turns left), damper 0..1;
// Options' strength, the invert switch and the binding's direction apply.
// Stop when no car is driven (menus, loading, a script-driven car).
// detail (bump kicks), impact (crash knocks), the damper and the vibration
// each have their own Options gain (#405); the sum is low-passed over dt
// (FFB SMOOTHING) and lifted past the wheel's dead band (FFB MIN FORCE).
void host_input_force_feedback(float torque, float damper, float detail = 0, float impact = 0,
                               float vibration = 0, float vibration_hz = 0, float dt = 0);
void host_input_force_feedback_stop();
// ControlSet device id that receives force feedback now, -1 none.
int host_input_force_feedback_device();
// Rebind listener (host_input_capture.hpp), advanced by host_input_poll_keys.
class InputCapture;
// kinds: a mask of 1 << InputKind, the device classes that may be taken
// (-1 every class). The Esc key (clears the binding) and the left mouse
// button (cancels) always listen.
void host_input_capture_begin(int kinds = -1);
void host_input_capture_cancel();
const InputCapture& host_input_capture();
float host_input_capture_progress();
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
