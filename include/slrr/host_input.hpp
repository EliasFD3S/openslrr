#pragma once

#include <cstdint>
#include <string>

namespace slrr {

// Shared Win32 → script axis bridge (Input.getAxis / camera orbit).
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
  bool part_click = false;
  int part_click_x = 0;
  int part_click_y = 0;
  bool keys_left = false;
  bool keys_right = false;
  bool keys_up = false;
  bool keys_down = false;
  bool keys_pgup = false;
  bool keys_pgdn = false;
};

HostInput& host_input();
void host_input_char(int ch);
const std::string& host_input_history();
void host_input_clear_history();

// Poll keyboard held keys into HostInput (call each frame).
void host_input_poll_keys();

// Map HostInput → stock Input axis id. device ignored for now.
float host_input_get_axis(int32_t axis, int32_t device);
// Keyboard steering/throttle/brake regression without changing live input.
bool host_input_self_check();

}  // namespace slrr
