#include "slrr/host_input.hpp"

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace slrr {
namespace {

HostInput g_input;

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
    case 1: {  // AXIS_TURN_LEFTRIGHT — clamped keyboard steering
      const float value = (in.keys_left ? 1.f : 0.f) -
                          (in.keys_right ? 1.f : 0.f);
      return std::clamp(value, -1.f, 1.f);
    }
    case 28:  // AXIS_THROTTLE
      return in.keys_up ? 1.f : 0.f;
    case 29:  // AXIS_BRAKE
      return in.keys_down ? 1.f : 0.f;
    case 2:
      return in.cursor_y;
    default:
      return 0.f;
  }
}

bool host_input_self_check() {
  auto& in = host_input();
  const HostInput saved = in;
  in.cursor_x = 1000.f;
  in.keys_left = true;
  in.keys_right = false;
  in.keys_up = true;
  in.keys_down = false;
  const float left = host_input_get_axis(1, 0);
  const float throttle = host_input_get_axis(28, 0);
  in.keys_left = false;
  in.keys_right = true;
  in.keys_up = false;
  in.keys_down = true;
  const float right = host_input_get_axis(1, 0);
  const float brake = host_input_get_axis(29, 0);
  in = saved;
  return left == 1.f && right == -1.f && throttle == 1.f && brake == 1.f;
}

}  // namespace slrr
