#include "slrr/host_input.hpp"

#include "slrr/jvm.hpp"
#include "slrr/osd_registry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
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
constexpr const char* pad_axes[] = {
    "Left Stick Left", "Left Stick Right", "Left Stick Up", "Left Stick Down",
    "Right Stick Left", "Right Stick Right", "Right Stick Up", "Right Stick Down",
    "LT", "RT", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
    "Start", "Back", "Left Stick Click", "Right Stick Click", "LB", "RB",
    "A", "B", "X", "Y"};
std::array<float, std::size(pad_axes)> pad{};
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
  const int group = read("actGroup");
  return read("getKey") >= 0 && group >= 0 &&
         (group == read("redefineGroup") || group == read("redefine2Group") ||
          group == read("redefineSuspGroup"));
}

#ifdef _WIN32
XINPUT_GAMEPAD poll_gamepad() {
  static DWORD slot = XUSER_MAX_COUNT;
  static ULONGLONG next_scan = 0;
  XINPUT_STATE state{};
  if (slot < XUSER_MAX_COUNT && XInputGetState(slot, &state) == ERROR_SUCCESS)
    return state.Gamepad;
  slot = XUSER_MAX_COUNT;
  const auto now = GetTickCount64();
  if (now < next_scan) return {};
  next_scan = now + 1000;
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    if (XInputGetState(i, &state) != ERROR_SUCCESS) continue;
    slot = i;
    return state.Gamepad;
  }
  return {};
}
#endif

float mapped_axis(const ControllerInput& input, int axis) {
  float positive = 0.f, negative = 0.f;
  for (const auto& binding : input.bindings) {
    if (binding.axis != axis || binding.from == binding.to) continue;
    const float value = host_input_physical_axis(binding.device, binding.physical);
    const float t = std::clamp((value - binding.from) / (binding.to - binding.from), 0.f, 1.f);
    const float mapped = std::lerp(binding.low, binding.high, t);
    positive = std::max(positive, mapped);
    negative = std::min(negative, mapped);
  }
  const bool default_pad_axis = axis == 1 || axis == 28 || axis == 29 || axis == 48;
  if (default_pad_axis && std::none_of(input.bindings.begin(), input.bindings.end(),
          [](const InputBinding& b) { return b.device == kPad; })) {
    switch (axis) {
      case 1:
        if (positive + negative == 0.f) return pad[0] - pad[1];
        break;
      case 28: return std::max(positive, pad[9]);
      case 29: return std::max(positive, pad[8]);
      case 48: return std::max(positive, pad[21]);
    }
  }
  return positive + negative;
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
  in.keys_left = keys[0xcb];
  in.keys_right = keys[0xcd];
  in.keys_up = keys[0xc8];
  in.keys_down = keys[0xd0];
  in.keys_pgup = keys[0xc9];
  in.keys_pgdn = keys[0xd1];

  auto held = [focused](int key) {
    return focused && (GetAsyncKeyState(key) & 0x8000);
  };
  mouse = {focused ? in.cursor_x : 0.f, focused ? in.cursor_y : 0.f,
           focused ? in.look_zoom : 0.f, float(held(VK_LBUTTON)),
           float(held(VK_RBUTTON)), float(held(VK_MBUTTON))};
  in.cursor_x = in.cursor_y = 0.f;

  const auto state = focused ? poll_gamepad() : XINPUT_GAMEPAD{};
  const auto stick = [](int value, int deadzone) {
    return std::clamp((value - deadzone) / (32767.f - deadzone), 0.f, 1.f);
  };
  const int lx = state.sThumbLX, ly = state.sThumbLY;
  const int rx = state.sThumbRX, ry = state.sThumbRY;
  const auto trigger = [](int value) {
    return std::max(0.f, (value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) /
                            (255.f - XINPUT_GAMEPAD_TRIGGER_THRESHOLD));
  };
  constexpr int left = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
  constexpr int right = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
  pad = {stick(-lx, left), stick(lx, left), stick(ly, left), stick(-ly, left),
         stick(-rx, right), stick(rx, right), stick(ry, right), stick(-ry, right),
         trigger(state.bLeftTrigger), trigger(state.bRightTrigger)};
  constexpr WORD buttons[] = {
      XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
      XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
      XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
      XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
      XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
      XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y};
  for (size_t i = 0; i < std::size(buttons); ++i)
    pad[10 + i] = (state.wButtons & buttons[i]) ? 1.f : 0.f;
#endif
}

const char* host_input_device_name(int device) {
  switch (device) {
    case 0: return "SysKeyboard";
    case 1: return "SysMouse";
    case kPad: return "XInput Gamepad";
    default: return nullptr;
  }
}

std::string host_input_axis_name(int device, int axis) {
  if (device == kPad && axis >= 0 && axis < static_cast<int>(pad.size()))
    return std::string("Pad ") + pad_axes[axis];
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
  if (device == kPad && i < pad.size()) return pad[i];
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
  } else if (device == kPad) {
    auto strongest = std::max_element(pad.begin(), pad.end());
    if (*strongest > .55f) return static_cast<int>(strongest - pad.begin());
  }
  return -1;
}

float host_input_camera_axis(int axis) {
  const auto& in = g_input;
  switch (axis) {
    case 7: return in.look_yaw + (float(in.keys_left) - float(in.keys_right)) * .005f;
    case 8: return in.look_pitch + (float(in.keys_up) - float(in.keys_down)) * .003f;
    case 10: return in.look_zoom + (float(in.keys_pgup) - float(in.keys_pgdn)) * .010f;
    default: return 0.f;
  }
}

float host_input_controller_axis(const Object* controller, int axis) {
  if (!controller || !controller->input) return 0.f;
  const auto& input = *controller->input;
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
      const float alpha = 1.f - std::pow(1.f - std::clamp(factor, 0.f, 1.f), dt * 1000.f);
      smooth.value = std::lerp(smooth.value, target, alpha);
    }
  }
  auto& in = g_input;
  auto axis = [&](int id) { return host_input_controller_axis(controller, id); };
  // Stock AXIS_TURN_LEFTRIGHT is positive to the left, like the chassis yaw.
  in.drive_steer = axis(1);
  in.drive_throttle = axis(28);
  in.drive_brake = axis(29);
  in.drive_handbrake = axis(48) > .5f;
  in.drive_clutch = axis(49);
  in.drive_shift = axis(50);
  in.drive_horn = axis(54) > .5f;
  const bool pad_y_mapped = controller && controller->input &&
      std::any_of(controller->input->bindings.begin(), controller->input->bindings.end(),
                  [](const InputBinding& b) { return b.device == kPad && b.physical == 23; });
  in.drive_reverse = keys[0x13] || (!pad_y_mapped && pad[23] > .5f);
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
    if (hk.owner) {
      jvm.invoke("java.render.Osd", "handleEvent", "(Ljava.io.Hotkey;)V",
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
