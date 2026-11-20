#include "save_io_fixture.hpp"
#include "../src/platform/host_input_devices.hpp"
#include "../src/platform/host_input_mapping.hpp"
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_events.h>
#include <cmath>

using namespace slrr;
namespace {
struct Peripheral {
  SDL_JoystickID id = 0;
  SDL_Joystick* joystick = nullptr;
  Peripheral(const char* name, int product, int axes, int buttons, int hats = 0, bool pad = false) {
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = pad ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_WHEEL;
    desc.vendor_id = 0x1234;
    desc.product_id = product;
    desc.naxes = axes; desc.nbuttons = buttons; desc.nhats = hats;
    desc.name = name;
    if (pad) {
      desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
      desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    }
    id = SDL_AttachVirtualJoystick(&desc);
    SLRR_REQUIRE(id != 0);
    joystick = SDL_OpenJoystick(id);
    SLRR_REQUIRE(joystick != nullptr);
    for (int i = 0; i < axes; ++i)
      axis(i, pad && i >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? -32768 : 0);
  }
  void detach() {
    if (joystick) SDL_CloseJoystick(joystick);
    joystick = nullptr;
    if (id) SLRR_REQUIRE(SDL_DetachVirtualJoystick(id));
    id = 0;
  }
  ~Peripheral() { detach(); }
  void axis(int index, Sint16 value) { SLRR_REQUIRE(SDL_SetJoystickVirtualAxis(joystick, index, value)); }
  void button(int index, bool value) { SLRR_REQUIRE(SDL_SetJoystickVirtualButton(joystick, index, value)); }
};
int find_device(const std::string& prefix) {
  for (int i = 3; const char* name = host_input_device_name(i); ++i)
    if (std::string(name).starts_with(prefix)) return i;
  throw std::runtime_error("device not found: " + prefix);
}
bool close_to(float a, float b) { return std::abs(a - b) < .002f; }

void multi_peripheral_input() {
  // Isolate the fixture from whatever physical controllers the operator owns.
  for (const char* hint : {SDL_HINT_JOYSTICK_DIRECTINPUT, SDL_HINT_JOYSTICK_RAWINPUT,
       SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_GAMEINPUT, SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_XINPUT_ENABLED})
    SDL_SetHint(hint, "0");
  auto& devices = host_input_devices();
  SLRR_REQUIRE(std::string(host_input_device_name(0)) == "SysKeyboard");
  SLRR_REQUIRE(std::string(host_input_device_name(1)) == "SysMouse");
  SLRR_REQUIRE(std::string(host_input_device_name(2)) == "XInput Gamepad");
  SLRR_REQUIRE(devices.active_axis(2) == -1);
  Peripheral wheel("Fixture wheel", 1, 1, 2, 1);
  Peripheral pedals("Fixture pedals", 2, 3, 0);
  Peripheral shifter("Fixture shifter", 3, 0, 8);
  Peripheral first_pad("Fixture pad", 4, SDL_GAMEPAD_AXIS_COUNT, SDL_GAMEPAD_BUTTON_COUNT, 0, true);
  Peripheral second_pad("Fixture pad", 4, SDL_GAMEPAD_AXIS_COUNT, SDL_GAMEPAD_BUTTON_COUNT, 0, true);
  for (int i = 0; i < 3; ++i) pedals.axis(i, -32768);
  devices.poll(true);
  const int w = find_device("Fixture wheel"), p = find_device("Fixture pedals"), s = find_device("Fixture shifter");
  const int first = find_device("Fixture pad");
  SLRR_REQUIRE(host_input_device_name(first + 1) != nullptr);
  SLRR_REQUIRE(std::string(host_input_device_name(first)) != host_input_device_name(first + 1));
  SLRR_REQUIRE(host_input_device_name(first + 2) == nullptr);
  SLRR_REQUIRE(devices.active_axis(p) == -1); // Three end-stop pedals cannot block all rebinding.
  SLRR_REQUIRE(devices.analog_axis(w, 0) && !devices.analog_axis(w, 1));
  SLRR_REQUIRE(devices.axis_name(w, 0).find("Fixture wheel") != std::string::npos);
  SLRR_REQUIRE(devices.axis_name(w, 3).ends_with("Hat 1 Up"));
  SLRR_REQUIRE(devices.axis_name(w, 99) == "Undefined");
  SLRR_REQUIRE(devices.axis(-1, 0) == 0 && devices.axis(w, -1) == 0);

  wheel.axis(0, -24576); pedals.axis(0, 16384); shifter.button(4, true);
  first_pad.axis(SDL_GAMEPAD_AXIS_LEFTX, -32768);
  first_pad.axis(SDL_GAMEPAD_AXIS_LEFTY, -32768);
  first_pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
  first_pad.button(SDL_GAMEPAD_BUTTON_NORTH, true);
  second_pad.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
  devices.poll(true);
  SLRR_REQUIRE(close_to(host_input_physical_axis(w, 0), -.75f));
  SLRR_REQUIRE(close_to(host_input_physical_axis(p, 0), .5f));
  SLRR_REQUIRE(host_input_physical_axis(s, 4) == 1);
  SLRR_REQUIRE(devices.active_axis(w) == 0);
  SLRR_REQUIRE(devices.active_axis(p) == 0);
  SLRR_REQUIRE(devices.active_axis(s) == 4);
  SLRR_REQUIRE(close_to(devices.pad()[0], 1) && close_to(devices.pad()[2], 1));
  SLRR_REQUIRE(close_to(devices.pad()[9], 1) && devices.pad()[23] == 1);
  SLRR_REQUIRE(devices.active_axis(first) == -1); // No duplicate primary-pad capture.
  SLRR_REQUIRE(devices.active_axis(first + 1) >= 0);
  SLRR_REQUIRE(devices.axis(first + 1, SDL_GAMEPAD_AXIS_COUNT + SDL_GAMEPAD_BUTTON_SOUTH) == 1);

  Fixture f;
  Object controller;
  auto add = [&](int logical, int device, int physical, float from, float to, float low, float high) {
    auto I = [](int n) { return Value::make_int(n); };
    auto F = [](float n) { return Value::make_float(n); };
    SLRR_REQUIRE(f.registry.invoke("java.io.Controller", "user_Add", "(IIIFFFF)I", &controller,
                 {I(logical), I(device), I(physical), F(from), F(to), F(low), F(high)}).v.i == 1);
  };
  add(1, w, 0, -1, 1, 1, -1); // Positive logical steering is left.
  add(28, p, 0, -1, 1, 0, 1);
  add(50, s, 4, 0, 1, 0, 1);
  SLRR_REQUIRE(controller.input->bindings[0].from == -1);
  auto map = [&](int logical) {
    return input_mapping::mapped_axis(*controller.input, logical, host_input_physical_axis, devices.pad(),
        [&](int device, int axis) { return device < 2 || devices.available(device, axis); });
  };
  SLRR_REQUIRE(close_to(map(1), .75f) && close_to(map(28), .75f)); // Explicit rig bindings beat full pad defaults.
  add(29, p, 1, 1, -1, 0, 1); // Reversed pedal calibration keeps its range.
  SLRR_REQUIRE(host_input_controller_axis(&controller, 29) == 1);
  SLRR_REQUIRE(close_to(host_input_controller_axis(&controller, 1), .75f));
  SLRR_REQUIRE(close_to(host_input_controller_axis(&controller, 28), .75f));
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(host_input().drive_steer_analog && host_input().drive_shift == 1);
  // Keyboard override stays digital while the wheel's physical channel is analog.
  host_input_key(30, true);
  add(1, 0, 30, 0, 1, 0, 1);
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(!host_input().drive_steer_analog);
  host_input_clear_keys();
  add(54, 2, 20, -.8f, 1, 0, 1);
  SLRR_REQUIRE(close_to(controller.input->bindings.back().from, .1f)); // Legacy range conversion only.

  // Full-range raw pedals must not map an unfocused/disconnected zero to half throttle.
  devices.poll(false);
  SLRR_REQUIRE(devices.active_axis(p) == -1 && !devices.available(p, 0));
  SLRR_REQUIRE(devices.axis(p, 0) == 0 && devices.pad()[9] == 0);
  SLRR_REQUIRE(host_input_controller_axis(&controller, 28) == 0);
  devices.poll(true);
  SLRR_REQUIRE(close_to(host_input_controller_axis(&controller, 28), .75f));
  const std::string pedal_name = host_input_device_name(p);
  pedals.detach();
  devices.poll(true);
  SLRR_REQUIRE(std::string(host_input_device_name(p)) == pedal_name);
  SLRR_REQUIRE(host_input_controller_axis(&controller, 28) == 0);
  SLRR_REQUIRE(devices.axis(s, 4) == 1); // Removing a preceding device cannot retarget a shifter.
  Peripheral replacement("Fixture pedals", 2, 3, 0);
  for (int i = 0; i < 3; ++i) replacement.axis(i, -32768);
  devices.poll(true);
  SLRR_REQUIRE(find_device("Fixture pedals") == p && devices.available(p, 0));
  SLRR_REQUIRE(devices.active_axis(p) == -1 && host_input_controller_axis(&controller, 28) == 0);

  wheel.axis(0, 0); shifter.button(4, false);
  SLRR_REQUIRE(SDL_SetJoystickVirtualHat(wheel.joystick, 0, SDL_HAT_RIGHTUP));
  devices.poll(true);
  SLRR_REQUIRE(devices.active_axis(w) == 3 && devices.axis(w, 3) == 1 && devices.axis(w, 6) == 1);
  SLRR_REQUIRE(SDL_SetJoystickVirtualHat(wheel.joystick, 0, SDL_HAT_CENTERED));
  devices.poll(true);
  SLRR_REQUIRE(devices.active_axis(w) == -1);
  SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
  for (int i = 0; i < 128; ++i) {
    wheel.axis(0, i % 2 ? -24000 : 24000);
    first_pad.axis(SDL_GAMEPAD_AXIS_LEFTX, i % 2 ? -24000 : 24000);
    devices.poll(true);
  }
  SLRR_REQUIRE(!SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST)); // Polling cannot fill SDL's unconsumed queue.
  first_pad.detach();
  devices.poll(true);
  SLRR_REQUIRE(devices.pad()[20] == 1 && devices.pad()[23] == 0); // Alias can fail over; raw slots do not move.
  second_pad.detach();
  devices.poll(true);
  SLRR_REQUIRE(!devices.available(2, 0) && devices.active_axis(2) == -1);
}
}  // namespace
int main() {
  const standalone::Test tests[] = {multi_peripheral_input};
  return standalone::run("sdl_input", tests);
}
