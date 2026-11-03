#include "slrr/host_input.hpp"

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
      if (in.keys_pgup) v += 0.02f;
      if (in.keys_pgdn) v -= 0.02f;
      return v;
    }
    case 1:  // AXIS_TURN_LEFTRIGHT — mouse X when not orbiting
      return in.cursor_x;
    case 2:
      return in.cursor_y;
    default:
      return 0.f;
  }
}

}  // namespace slrr
