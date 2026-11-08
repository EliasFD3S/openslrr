#include "slrr/host_input.hpp"

#include <algorithm>
#include <cmath>

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

}  // namespace

HostInput& host_input() { return g_input; }
namespace { std::string history; }
void host_input_char(int ch) {
  if(ch>='A'&&ch<='Z')ch+='a'-'A';
  if(ch>=32&&ch<127){history.push_back(static_cast<char>(ch));if(history.size()>128)history.erase(0,history.size()-128);}
}
const std::string& host_input_history(){return history;}
void host_input_clear_history(){history.clear();}

void host_input_poll_keys() {
#ifdef _WIN32
  auto& in = g_input;
  in.keys_left = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
  in.keys_right = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
  in.keys_up = (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
  in.keys_down = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
  in.keys_pgup = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
  in.keys_pgdn = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
  auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
  const HWND foreground = GetForegroundWindow();
  DWORD owner = 0;
  GetWindowThreadProcessId(foreground, &owner);
  const bool focused = owner == GetCurrentProcessId();
  in.drive_steer = focused ? float(down('D')) - float(down('A')) : 0.f;
  in.drive_throttle = focused && down('W') ? 1.f : 0.f;
  in.drive_brake = focused && down('S') ? 1.f : 0.f;
  in.drive_reverse = focused && down('R');
  in.drive_handbrake = focused && down(VK_SPACE);
  if (focused) {
    const auto pad = poll_gamepad();
    const float x = static_cast<float>(pad.sThumbLX);
    const float steer = std::copysign(std::clamp(
        (std::abs(x) - XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) /
            (32767.f - XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE), 0.f, 1.f), x);
    const auto trigger = [](BYTE value) {
      return std::max(0.f, (value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) /
                              (255.f - XINPUT_GAMEPAD_TRIGGER_THRESHOLD));
    };
    if (in.drive_steer == 0.f) in.drive_steer = steer;
    in.drive_throttle = std::max(in.drive_throttle, trigger(pad.bRightTrigger));
    in.drive_brake = std::max(in.drive_brake, trigger(pad.bLeftTrigger));
    in.drive_reverse |= (pad.wButtons & XINPUT_GAMEPAD_Y) != 0;
    in.drive_handbrake |= (pad.wButtons & XINPUT_GAMEPAD_B) != 0;
  }
#endif
}

float host_input_get_axis(int32_t axis, int32_t /*device*/) {
  auto& in = g_input;
  // CAMTURNSET keyboard rates (ControlSet.java ~0.003–0.005).
  constexpr float kYaw = 0.005f;
  constexpr float kPitch = 0.003f;
  switch (axis) {
    case 7: {  // AXIS_LOOK_LEFTRIGHT
      float v = in.look_yaw;
      if (in.keys_left) v += kYaw;
      if (in.keys_right) v -= kYaw;
      return v;
    }
    case 8: {  // AXIS_LOOK_UPDOWN
      float v = in.look_pitch;
      if (in.keys_up) v += kPitch;
      if (in.keys_down) v -= kPitch;
      return v;
    }
    case 10: {  // AXIS_LOOK_FWDBACK
      float v = in.look_zoom;
      // ControlSet: PgUp -0.010, PgDn +0.010 on AXIS_LOOK_FWDBACK.
      // Positive here zooms in; the orbit applies 1 - axis * 100 * dt.
      if (in.keys_pgup) v += 0.010f;
      if (in.keys_pgdn) v -= 0.010f;
      return v;
    }
    case 1:  // AXIS_TURN_LEFTRIGHT — mouse X when not orbiting
      return in.cursor_x;
    case 2:
      return in.cursor_y;
    case 28: return in.drive_throttle;
    case 29: return in.drive_brake;
    case 48: return in.drive_handbrake ? 1.f : 0.f;
    default:
      return 0.f;
  }
}

}  // namespace slrr
