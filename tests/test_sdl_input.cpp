#include "save_io_fixture.hpp"
#include "../src/platform/host_input_capture.hpp"
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
  // mapped_wheel: a wheel SDL also maps as a gamepad (pad masks, wheel type).
  Peripheral(const char* name, int product, int axes, int buttons, int hats = 0, bool pad = false,
             bool mapped_wheel = false) {
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = pad && !mapped_wheel ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_WHEEL;
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
  // Found before the pads: it must not become the device 2 alias.
  Peripheral mapped_wheel("Fixture mapped wheel", 5, SDL_GAMEPAD_AXIS_COUNT, SDL_GAMEPAD_BUTTON_COUNT, 0, true, true);
  Peripheral first_pad("Fixture pad", 4, SDL_GAMEPAD_AXIS_COUNT, SDL_GAMEPAD_BUTTON_COUNT, 0, true);
  Peripheral second_pad("Fixture pad", 4, SDL_GAMEPAD_AXIS_COUNT, SDL_GAMEPAD_BUTTON_COUNT, 0, true);
  for (int i = 0; i < 3; ++i) pedals.axis(i, -32768);
  devices.poll(true);
  const int w = find_device("Fixture wheel"), p = find_device("Fixture pedals"), s = find_device("Fixture shifter");
  const int first = find_device("Fixture pad");
  const int mw = find_device("Fixture mapped wheel");
  SLRR_REQUIRE(devices.kind(2) == InputKind::Pad && devices.kind(first) == InputKind::Pad);
  SLRR_REQUIRE(devices.kind(w) == InputKind::Wheel && devices.kind(p) == InputKind::Wheel);
  SLRR_REQUIRE(devices.kind(mw) == InputKind::Wheel && devices.capturable(mw));
  SLRR_REQUIRE(!devices.capturable(first) && devices.capturable(first + 1));
  SLRR_REQUIRE(host_input_device_kind(0) == InputKind::Keyboard && host_input_device_kind(1) == InputKind::Mouse);
  SLRR_REQUIRE(devices.channels(w) == 1 + 2 + 4);
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
  // Stock AXIS_NITRO (31) uses N (scan 0x31), with no implicit pad binding.
  host_input_clear_keys();
  add(31, 0, 0x31, 0, 1, 0, 1);
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(!host_input().drive_nitro); // Pad North is held but not mapped.
  host_input_key(0x31, true);
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(host_input().drive_nitro);
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(host_input().drive_nitro); // Held, not a press edge.
  host_input_key(0x31, false);
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(!host_input().drive_nitro);
  add(31, 2, 23, 0, 1, 0, 1); // Explicit pad North mapping.
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(host_input().drive_nitro);
  first_pad.button(SDL_GAMEPAD_BUTTON_NORTH, false);
  devices.poll(true);
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(!host_input().drive_nitro);
  first_pad.button(SDL_GAMEPAD_BUTTON_NORTH, true);
  devices.poll(true);
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(host_input().drive_nitro);

  Object analog_nitro;
  analog_nitro.input = std::make_unique<ControllerInput>();
  analog_nitro.input->bindings.push_back({31, p, 0, 0, 1, 0, .5f});
  host_input_update_controller(&analog_nitro, 1.f / 60);
  SLRR_REQUIRE(close_to(host_input().drive_nitro, .25f)); // Preserve the axis, not a bool threshold.
  host_input_update_controller(&controller, 1.f / 60);

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
  SLRR_REQUIRE(host_input().drive_steer_kind == InputKind::Wheel);
  // Keyboard override stays digital while the wheel's physical channel is analog.
  host_input_key(30, true);
  add(1, 0, 30, 0, 1, 0, 1);
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(!host_input().drive_steer_analog);
  SLRR_REQUIRE(host_input().drive_steer_kind == InputKind::Keyboard);
  host_input_clear_keys();
  add(54, 2, 20, -.8f, 1, 0, 1);
  SLRR_REQUIRE(close_to(controller.input->bindings.back().from, .1f)); // Legacy range conversion only.

  // Full-range raw pedals must not map an unfocused/disconnected zero to half throttle.
  devices.poll(false);
  SLRR_REQUIRE(devices.active_axis(p) == -1 && !devices.available(p, 0));
  SLRR_REQUIRE(devices.axis(p, 0) == 0 && devices.pad()[9] == 0);
  SLRR_REQUIRE(host_input_controller_axis(&controller, 28) == 0);
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(!host_input().drive_nitro);
  devices.poll(true);
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(host_input().drive_nitro);
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
  host_input_update_controller(&controller, 1.f / 60);
  SLRR_REQUIRE(!host_input().drive_nitro);
}

// The rebind listener on a rig: a shifter in gear, a toggle, end-stop pedals,
// a centred wheel and a noisy axis on four devices at once.
void capture_on_a_rig() {
  using C = CaptureChannel;
  InputCapture c;
  double t = 0;
  std::vector<C> rig = {
      {0, 0x1e, 0, false, false},     // key A
      {1, 3, 1, false, true},         // left mouse still down from the click
      {3, 0, 0, true, false},         // wheel, centred
      {4, 0, -1, true, false},        // throttle pedal at its end stop
      {4, 1, 1, true, false},         // reversed clutch pedal
      {5, 2, 1, false, false},        // H-shifter in 2nd gear: never released
      {6, 7, 0, false, false},        // button box button
  };
  auto frames = [&](double seconds) {
    for (double end = t + seconds; t < end; t += 1.0 / 60) c.update(t, rig);
  };
  c.begin(t);
  frames(.1);
  SLRR_REQUIRE(c.state() == InputCapture::Releasing);
  rig[1].value = 0;  // the click ends
  frames(.2);
  SLRR_REQUIRE(c.state() == InputCapture::Releasing && c.settle_progress(t) < 1);
  frames(.2);
  SLRR_REQUIRE(c.state() == InputCapture::Listening && c.held() == 1);  // the shifter
  frames(.5);
  SLRR_REQUIRE(c.state() == InputCapture::Listening);  // a held gear does not bind
  // The last device on the list wins when it is the one touched.
  rig[6].value = 1;
  frames(1.0 / 60);
  SLRR_REQUIRE(c.state() == InputCapture::Holding && c.device() == 6 && c.axis() == 7 && !c.analog());
  frames(.5);
  SLRR_REQUIRE(c.state() == InputCapture::Holding);
  rig[6].value = 0;
  frames(1.0 / 60);
  SLRR_REQUIRE(c.state() == InputCapture::Done && c.device() == 6);
  frames(1);
  SLRR_REQUIRE(c.state() == InputCapture::Done);  // kept until read

  // End-stop pedal: its rest and the far end, also when pressed in steps.
  c.begin(t);
  frames(.4);
  SLRR_REQUIRE(c.state() == InputCapture::Listening);
  rig[3].value = -.7f;  // below the threshold from -1
  frames(.1);
  SLRR_REQUIRE(c.state() == InputCapture::Listening);
  rig[3].value = .2f;
  frames(.1);
  rig[3].value = 1;
  frames(.1);
  SLRR_REQUIRE(c.state() == InputCapture::Holding && c.device() == 4 && c.axis() == 0 && c.analog());
  rig[3].value = -1;
  frames(.1);
  SLRR_REQUIRE(c.state() == InputCapture::Done && c.rest() == -1 && c.peak() == 1);

  // Centred wheel turned left; the reversed clutch keeps its +1 rest.
  c.begin(t);
  frames(.4);
  rig[2].value = -.8f;
  frames(.1);
  rig[2].value = 0;
  frames(.1);
  SLRR_REQUIRE(c.state() == InputCapture::Done && c.device() == 3 && c.rest() == 0 && c.peak() < -.7f);
  c.begin(t);
  frames(.4);
  rig[5].value = 0;  // shifter to neutral: not a press
  frames(.2);
  SLRR_REQUIRE(c.state() == InputCapture::Listening);
  rig[5].value = 1;  // back into gear after it rested at 0: now a press
  frames(.1);
  SLRR_REQUIRE(c.state() == InputCapture::Holding && c.device() == 5);
  // A toggle that stays on is accepted after kHoldMax.
  frames(InputCapture::kHoldMax + .1);
  SLRR_REQUIRE(c.state() == InputCapture::Done && c.device() == 5);

  // A noisy axis cannot keep the listener releasing forever.
  rig.push_back({7, 0, 0, true, false});
  c.begin(t);
  for (double end = t + InputCapture::kSettleMax + .1; t < end; t += 1.0 / 60) {
    rig.back().value = rig.back().value > 0 ? -.06f : .06f;
    c.update(t, rig);
  }
  SLRR_REQUIRE(c.state() == InputCapture::Listening);
  frames(.2);
  SLRR_REQUIRE(c.state() == InputCapture::Listening);

  // The left mouse button cancels; a device plugged in while listening
  // starts at its own rest instead of binding at once.
  rig.push_back({8, 0, -1, true, false});
  frames(.1);
  SLRR_REQUIRE(c.state() == InputCapture::Listening);
  rig[1].value = 1;
  frames(1.0 / 60);
  SLRR_REQUIRE(c.state() == InputCapture::Cancelled);
  c.reset();
  SLRR_REQUIRE(c.state() == InputCapture::Idle);
}
}  // namespace
int main() {
  const standalone::Test tests[] = {multi_peripheral_input, capture_on_a_rig};
  return standalone::run("sdl_input", tests);
}
