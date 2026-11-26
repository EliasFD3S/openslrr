#include "host_input_capture.hpp"
#include "host_input_mapping.hpp"
#include "host_input_devices.hpp"
#include "slrr/host_input.hpp"
#include "slrr/host_game_options.hpp"

#include "slrr/jvm.hpp"
#include "slrr/osd_registry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace slrr {
namespace {

HostInput g_input;
std::array<bool, 256> keys{};
std::array<bool, 256> pressed{}, frame_pressed{};
std::deque<int> key_presses;
std::string history;

// Keep this logical device ID available even when unplugged so a saved
// profile can resolve its bindings before the controller is connected.
constexpr int kPad = 2;
std::array<float, 24> pad{};
std::array<float, 6> mouse{};

struct Hotkey {
  uint64_t id;
  Object* key;
  Object* handler;
  Object* owner;
  int axis, flags, events;
  bool down;
};
std::vector<Hotkey> hotkeys;
uint64_t next_hotkey = 0;

InputCapture capture;

// The force feedback wheel: the first steering binding on a device with
// haptics, and the sign from the logical axis to that device's axis.
int ffb_device = -1;
float ffb_sign = 1.f;
// The controller the brake (axis 29) is bound to, for the ABS pulse (#446).
int brake_device = -1;

void choose_force_feedback(const ControllerInput* input) {
  ffb_device = -1;
  brake_device = -1;
  if (!input) return;
  for (const auto& b : input->bindings)
    if (b.axis == 29 && b.device >= 3) {
      brake_device = b.device;
      break;
    }
  for (const auto& b : input->bindings) {
    if (b.axis != 1 || b.device < 3 || !host_input_devices().has_force_feedback(b.device)) continue;
    // Logical steer = lerp(low, high, (physical - from) / (to - from)).
    const float slope = (b.to - b.from) * (b.high - b.low);
    if (slope == 0.f) continue;
    ffb_device = b.device;
    ffb_sign = slope > 0 ? 1.f : -1.f;
    return;
  }
}

double capture_clock() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Every channel the rebind listener may take, on every device at once.
// Device classes the listener may take (1 << InputKind), see
// host_input_capture_begin.
int capture_kinds = -1;

bool capture_kind(int device) {
  const int kind = static_cast<int>(host_input_device_kind(device));
  return kind >= 0 && (capture_kinds & (1 << kind));
}

std::vector<CaptureChannel> capture_channels() {
  std::vector<CaptureChannel> out;
  constexpr int kEscape = 0x01;
  const bool keyboard = capture_kind(0);
  for (int i = 1; i < static_cast<int>(keys.size()); ++i)
    if (keyboard || i == kEscape)
      out.push_back({0, i, host_input_physical_axis(0, i), false, false});
  // Pointer motion is never taken; the left button (the click that opened the
  // listener, or a click elsewhere) cancels it.
  const bool buttons = capture_kind(1);
  for (int i = 3; i < static_cast<int>(mouse.size()); ++i)
    if (buttons || i == 3)
      out.push_back({1, i, mouse[static_cast<size_t>(i)], false, i == 3});
  auto& devices = host_input_devices();
  for (int slot = kPad; slot < 3 + devices.count(); ++slot) {
    if (!devices.capturable(slot) || !capture_kind(slot)) continue;
    for (int i = 0; i < devices.channels(slot); ++i)
      if (devices.available(slot, i))
        out.push_back({slot, i, devices.axis(slot, i), devices.analog_axis(slot, i), false});
  }
  return out;
}

bool captures_binding(Object* focus) {
  if (!focus) return false;
  auto handler = focus->fields.find("globalHandler");
  if (handler == focus->fields.end() || handler->second.tag != ValueTag::Obj ||
      !handler->second.v.o) return false;
  const auto& fields = handler->second.v.o->fields;
  auto read = [&](const char* name) {
    const auto it = fields.find(name);
    return it != fields.end() && it->second.tag == ValueTag::Int ? it->second.v.i : -1;
  };
  // A screen that captures a binding says so (OsdxOptions.capturingInput).
  if (read("capturingInput") > 0) return true;
  const int group = read("actGroup");
  return read("getKey") >= 0 && group >= 0 &&
         (group == read("redefineGroup") || group == read("redefine2Group") ||
          group == read("redefineSuspGroup"));
}

bool physical_available(int device, int axis) {
  return device < 2 || host_input_devices().available(device, axis);
}

float mapped_axis(const ControllerInput& input, int axis) {
  return input_mapping::mapped_axis(input, axis, host_input_physical_axis, pad, physical_available);
}

bool hotkey_down(const Hotkey& hotkey, const Object* controller) {
  return std::abs(hotkey.flags & 1
      ? host_input_controller_axis(controller, hotkey.axis)
      : host_input_physical_axis(0, hotkey.axis)) > .5f;
}

}  // namespace

HostInput& host_input() { return g_input; }

void host_input_char(int ch) {
  if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  if (ch >= 32 && ch < 127) {
    history.push_back(static_cast<char>(ch));
    if (history.size() > 128) history.erase(0, history.size() - 128);
  }
}

const std::string& host_input_history() { return history; }
void host_input_clear_history() { history.clear(); }

void host_input_key(int scan, bool down) {
  if (scan <= 0 || scan >= static_cast<int>(keys.size())) return;
  if (down && !keys[scan]) {
    pressed[scan] = true;
    if (key_presses.size() == 64) key_presses.pop_front();
    key_presses.push_back(scan);
  }
  keys[scan] = down;
}

void host_input_clear_keys() {
  keys.fill(false);
  pressed.fill(false);
  frame_pressed.fill(false);
  key_presses.clear();
}

int host_input_last_key() {
  if (key_presses.empty()) return 0;
  const int scan = key_presses.front();
  key_presses.pop_front();
  return scan;
}

void host_input_poll_keys() {
#ifdef _WIN32
  auto& in = g_input;
  DWORD owner = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &owner);
  const bool focused = owner == GetCurrentProcessId();
  if (!focused) host_input_clear_keys();
  frame_pressed = pressed;
  pressed.fill(false);
  in.keys_pgup = keys[0xc9];
  in.keys_pgdn = keys[0xd1];

  const bool game_mouse = focused && !in.debug_mouse_capture;
  auto held = [game_mouse](int key) {
    return game_mouse && (GetAsyncKeyState(key) & 0x8000);
  };
  mouse = {game_mouse ? in.cursor_x : 0.f, game_mouse ? in.cursor_y : 0.f,
           game_mouse ? in.look_zoom : 0.f, float(held(VK_LBUTTON)),
           float(held(VK_RBUTTON)), float(held(VK_MBUTTON))};
  in.cursor_x = in.cursor_y = 0.f;

  host_input_devices().poll(focused);
  pad = host_input_devices().pad();
#endif
  if (capture.state() != InputCapture::Idle) capture.update(capture_clock(), capture_channels());
}

void host_input_capture_begin(int kinds) {
  capture_kinds = kinds;
  capture.begin(capture_clock());
}
void host_input_capture_cancel() { capture.reset(); }
const InputCapture& host_input_capture() { return capture; }
float host_input_capture_progress() { return capture.settle_progress(capture_clock()); }

namespace {
float ffb_smoothed = 0.f;  // the low-passed force, before the minimum force
}

void host_input_force_feedback(float torque, float damper, float detail, float impact,
                               float vibration, float vibration_hz, float dt) {
  const auto& options = host_game_options();
  if (ffb_device < 0 || !options.ffb || options.ffb_strength <= 0.f) {
    host_input_force_feedback_stop();
    return;
  }
  const auto& tuning = host_input_tuning();
  auto gain = [](int percent) { return std::clamp(percent, 0, 200) / 100.f; };
  const float strength = options.ffb_strength;
  float force = std::clamp((torque + detail * gain(tuning.ffb_detail) +
                            impact * gain(tuning.ffb_impacts)) * strength, -1.f, 1.f);
  // A light low-pass takes the frame-to-frame steps out of the force, which a
  // belt or gear wheel otherwise plays as grain.
  const float tau = std::clamp(tuning.ffb_smoothing, 0, 50) / 1000.f;
  ffb_smoothed = tau > 0.f && dt > 0.f
                     ? ffb_smoothed + (force - ffb_smoothed) * (1.f - std::exp(-dt / tau))
                     : force;
  force = ffb_smoothed;
  // Minimum force: a wheel with a dead band ignores small forces; lift every
  // non-zero force past it, keeping the response proportional above.
  const float floor = std::clamp(tuning.ffb_min_force, 0, 20) / 100.f;
  if (floor > 0.f && std::abs(force) > 1e-3f)
    force = std::copysign(floor + (1.f - floor) * std::abs(force), force);
  const float invert = tuning.ffb_invert ? -1.f : 1.f;
  host_input_devices().set_force(ffb_device, force * ffb_sign * invert,
                                 damper * gain(tuning.ffb_damper) * strength,
                                 vibration * gain(tuning.ffb_vibration) * strength,
                                 vibration_hz);
}

void host_input_brake_pulse(float strength) {
  const auto& options = host_game_options();
  if (brake_device < 3 || !options.ffb || strength <= 0.f) return;
  host_input_devices().rumble(brake_device - 3, std::clamp(strength, 0.f, 1.f) * options.ffb_strength, 90);
}

void host_input_force_feedback_stop() {
  ffb_smoothed = 0.f;
  host_input_devices().stop_force();
}

int host_input_force_feedback_device() { return ffb_device; }

InputKind host_input_device_kind(int device) {
  if (device == 0) return InputKind::Keyboard;
  if (device == 1) return InputKind::Mouse;
  return device >= kPad ? host_input_devices().kind(device) : InputKind::None;
}

const char* host_input_device_name(int device) {
  switch (device) {
    case 0: return "SysKeyboard";
    case 1: return "SysMouse";
    default: return host_input_devices().name(device);
  }
}

std::string host_input_axis_name(int device, int axis) {
  if (device >= kPad) return host_input_devices().axis_name(device, axis);
  constexpr const char* mouse_axes[] = {"Mouse X", "Mouse Y", "Mouse Wheel",
                                       "Mouse Left", "Mouse Right", "Mouse Middle"};
  if (device == 1 && axis >= 0 && axis < static_cast<int>(mouse.size()))
    return mouse_axes[axis];
  if (device == 0 && axis > 0 && axis < static_cast<int>(keys.size())) {
#ifdef _WIN32
    char name[64]{};
    if (GetKeyNameTextA((axis & 0x7f) << 16 | (axis & 0x80) << 17, name,
                        static_cast<int>(sizeof(name))) > 0)
      return name;
#endif
    return "Key " + std::to_string(axis);
  }
  return "Undefined";
}

float host_input_physical_axis(int device, int axis) {
  if (axis < 0) return 0.f;
  const auto i = static_cast<size_t>(axis);
  if (device == 0 && i < keys.size()) return keys[i] || frame_pressed[i] ? 1.f : 0.f;
  if (device == 1 && i < mouse.size()) return mouse[i];
  if (device >= kPad) return host_input_devices().axis(device, axis);
  return 0.f;
}

int host_input_active_axis(int device) {
  if (device == 0) {
    for (int i = 1; i < static_cast<int>(keys.size()); ++i)
      if (keys[i] || frame_pressed[i]) return i;
  } else if (device == 1) {
    // Ignore pointer motion so clicking a row and moving away cannot bind X/Y.
    for (int i = 3; i < static_cast<int>(mouse.size()); ++i)
      if (mouse[i] > .5f) return i;
  } else if (device >= kPad) {
    return host_input_devices().active_axis(device);
  }
  return -1;
}

float host_input_camera_axis(const Object* controller, int axis) {
  const auto& in = g_input;
  switch (axis) {
    // Arrow-key look is ControlSet CAMTURNSET (group 3), read through the
    // player controller, so it stops when a script resets to DRIVERSET
    // (garage engine test). Mouse look bindings are the host drag instead.
    case 7:
    case 8:
      if (!controller || !controller->input) return 0.f;
      return input_mapping::mapped_axis(
          *controller->input, axis,
          [](int device, int physical) {
            return device == 1 ? 0.f : host_input_physical_axis(device, physical);
          },
          pad, physical_available);
    case 10: return in.look_zoom + (float(in.keys_pgup) - float(in.keys_pgdn)) * .010f;
    default: return 0.f;
  }
}

void host_input_cancel_pointer_gestures() {
  auto& in = g_input;
  ++in.pointer_epoch;
#ifdef _WIN32
  in.pointer_cancel_ms = GetTickCount();
#endif
  in.part_click = false;
  in.world_press = false;
  if (in.world_button) {
    in.world_button = false;
    in.world_release = true;
  }
}

float host_input_controller_axis(const Object* controller, int axis) {
  if (!controller || !controller->input) return 0.f;
  const auto& input = *controller->input;
  if (const auto forced = input.forced_axes.find(axis); forced != input.forced_axes.end())
    return forced->second;
  auto it = input.smoothing.find(axis);
  if (it != input.smoothing.end()) return it->second.value;
  return mapped_axis(input, axis);
}

void host_input_update_controller(Object* controller, float dt) {
  if (controller && controller->input) {
    auto& input = *controller->input;
    for (auto& [axis, smooth] : input.smoothing) {
      const float raw = std::clamp(mapped_axis(input, axis), -1.f, 1.f);
      const float target = std::copysign(std::pow(std::abs(raw), smooth.power), raw);
      const float factor = std::abs(target) <= smooth.center ? smooth.toward_center
          : target * smooth.value < 0.f ? smooth.opposite : smooth.same;
      // Stock Config speed values are small rates (throttle defaults to .02).
      const float alpha = 1.f - std::pow(
          1.f - std::clamp(factor, 0.f, 1.f), dt * 1000.f * smooth.speed);
      smooth.value = std::lerp(smooth.value, target, alpha);
    }
  }
  auto& in = g_input;
  auto axis = [&](int id) { return host_input_controller_axis(controller, id); };
  // Stock AXIS_TURN_LEFTRIGHT is positive to the left, like the chassis yaw.
  in.drive_steer = axis(1);
  choose_force_feedback(controller ? controller->input.get() : nullptr);
  if (!controller || !controller->input) {
    in.drive_steer_analog = false;
    in.drive_steer_kind = InputKind::None;
  } else {
    auto analog_axis = [](int device, int axis) {
      return host_input_devices().analog_axis(device, axis);
    };
    // A binding counts as active past 1 % of its travel. Below that, keep the
    // class that steered last: 1 % of a 900-degree wheel is 4.5 degrees, and
    // treating it as a key there applied the keyboard window and help at
    // speed, a dead zone around the centre and a notch when leaving it.
    const auto kind = input_mapping::steering_kind(
        *controller->input, host_input_physical_axis, pad, analog_axis,
        host_input_device_kind, physical_available);
    if (kind != InputKind::None) {
      in.drive_steer_analog = input_mapping::steering_is_analog(
          *controller->input, host_input_physical_axis, pad, analog_axis, physical_available);
      in.drive_steer_kind = kind;
    }
    // Steering skips the stock ControlSet smoothing (#383): its power 1.5
    // curve gave a half-turned wheel or stick 35 % of the lock, and its rate
    // (Config.steerSpeed 0.01) lagged about 400 ms behind the input. An analog
    // wheel or stick now steers directly; a key is shaped by steering_filter.
    // A script-forced steer still wins.
    // The STEERING GAMMA option (the axis' ControlSet power) still shapes it.
    const auto& input = *controller->input;
    if (input.forced_axes.find(1) == input.forced_axes.end()) {
      float steer = std::clamp(mapped_axis(input, 1), -1.f, 1.f);
      if (const auto smooth = input.smoothing.find(1); smooth != input.smoothing.end())
        steer = std::copysign(std::pow(std::abs(steer), smooth->second.power), steer);
      in.drive_steer = steer;
    }
  }
  in.drive_throttle = axis(28);
  in.drive_brake = axis(29);
  in.drive_nitro = std::clamp(axis(31), 0.f, 1.f);
  in.drive_handbrake = std::clamp(axis(48), 0.f, 1.f);
  in.drive_clutch = axis(49);
  in.free_camera_x = -axis(4);
  in.free_camera_y = axis(5);
  in.free_camera_z = axis(6);
  in.drive_shift = axis(50);
  in.drive_horn = axis(54) > .5f;
  const bool pad_y_mapped = controller && controller->input &&
      std::any_of(controller->input->bindings.begin(), controller->input->bindings.end(),
                  [](const InputBinding& b) { return b.device == kPad && b.physical == 23; });
  in.drive_reverse = keys[0x13] || (!pad_y_mapped && pad[23] > .5f);
  if (in.bench_throttle >= 0.f) {
    in.drive_throttle = in.bench_throttle;
    in.drive_brake = 0.f;
    in.drive_steer = 0.f;
    in.drive_steer_analog = false;
    in.drive_steer_kind = InputKind::None;
    in.drive_handbrake = 0.f;
    in.drive_reverse = false;
  }
}

void host_input_create_hotkey(Jvm& jvm, Object* key, Object* handler,
                              Object* owner, int axis, int flags, int events) {
  if (!key) return;
  host_input_delete_hotkey(key);
  auto& hotkey = hotkeys.emplace_back(Hotkey{++next_hotkey, key, handler, owner,
                                             axis, flags, events, false});
  hotkey.down = hotkey_down(hotkey, host_input_player_controller(jvm));
}

void host_input_delete_hotkey(Object* key) {
  std::erase_if(hotkeys, [key](const Hotkey& hk) { return hk.key == key; });
}

void host_input_forget(Object* object) {
  std::erase_if(hotkeys, [object](const Hotkey& hk) {
    return hk.key == object || hk.owner == object || hk.handler == object;
  });
}

bool menu_repeat_command(int command) {
  const unsigned code = static_cast<unsigned>(command) & 0x7fffffffu;
  return code >= 0x00010000u && code <= 0x00070000u && (code & 0xffffu) == 0;
}

bool host_input_allow_ui(int id, bool pressed) {
  if (!pressed) return true;
  using Clock = std::chrono::steady_clock;
  static std::unordered_map<int, Clock::time_point> until;
  const auto now = Clock::now();
  auto& slot = until[id];
  if (now < slot) return false;
  slot = now + std::chrono::milliseconds(250);
  return true;
}

void host_input_check_hotkeys(Jvm& jvm, Object* controller, Object* focus) {
  const bool capturing = captures_binding(focus);
  // Record every transition before invoking Java. A callback can display a
  // modal dialog and pump another input frame before it returns.
  std::vector<uint64_t> changed;
  for (auto& hk : hotkeys) {
    const bool down = hotkey_down(hk, controller);
    if (hk.down == down) continue;
    hk.down = down;
    if (!capturing && (!hk.owner || hk.owner == focus) &&
        (hk.events & (down ? 1 : 2))) changed.push_back(hk.id);
  }
  for (auto id : changed) {
    const auto it = std::find_if(hotkeys.begin(), hotkeys.end(),
                                [id](const Hotkey& hk) { return hk.id == id; });
    if (it == hotkeys.end()) continue;
    const Hotkey hk = *it;
    hk.key->fields["state"] = Value::make_int(hk.down ? 1 : 0);
    // MENU and CANCEL can be the same physical Escape/Start press across
    // two different dialogs. Debounce the action, not its per-dialog command.
    const bool virtual_axis = (hk.flags & 1) != 0;
    const bool cancel = virtual_axis ? (hk.axis == 35 || hk.axis == 36)
                                     : hk.axis == 0x01;
    const bool select = virtual_axis ? hk.axis == 34
                                     : (hk.axis == 0x1c || hk.axis == 0x9c);
    if ((cancel || select) &&
        !host_input_allow_ui(cancel ? -0x10000 : -0x10001, hk.down))
      continue;
    const auto video_command = hk.key->fields.find("command");
    if (video_command != hk.key->fields.end() &&
        video_command->second.tag == ValueTag::Int &&
        menu_repeat_command(video_command->second.v.i) &&
        !host_input_allow_ui(video_command->second.v.i, hk.down))
      continue;
    if (hk.down && video_command != hk.key->fields.end() &&
        video_command->second.tag == ValueTag::Int &&
        host_commit_video_dialog(jvm, hk.handler, video_command->second.v.i))
      continue;
    if (hk.owner) {
      // The owner's own class: java.render.osdx.Osd has its own handleEvent.
      jvm.invoke(hk.owner->class_name, "handleEvent", "(Ljava.io.Hotkey;)V",
                 {Value::make_obj(hk.owner), Value::make_obj(hk.key)}, false);
    } else if (hk.handler) {
      auto command = hk.key->fields.find("command");
      if (command != hk.key->fields.end())
        jvm.invoke_by_name(hk.handler->class_name, "osdCommand",
                           {Value::make_obj(hk.handler), command->second}, false);
    }
  }
}

void host_input_flush_hotkeys(Object* controller) {
  for (auto& hk : hotkeys) hk.down = hotkey_down(hk, controller);
}

Object* host_input_player_controller(Jvm& jvm) {
  const Value player = jvm.get_static("java.game.GameLogic", "player");
  if (player.tag != ValueTag::Obj || !player.v.o) return nullptr;
  const auto it = player.v.o->fields.find("controller");
  return it != player.v.o->fields.end() && it->second.tag == ValueTag::Obj
      ? it->second.v.o : nullptr;
}

}  // namespace slrr
