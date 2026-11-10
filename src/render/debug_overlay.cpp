#include "debug_overlay.hpp"

#include "slrr/host_game_options.hpp"
#include "slrr/host_input.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/gameref.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/scene_presentation.hpp"
#include "slrr/section_machine.hpp"
#include "slrr/tire_model.hpp"

#include <imgui/imgui.h>

#include <algorithm>
#include <cmath>

namespace slrr {
namespace {
constexpr bgfx::ViewId kDebugView = 71;
constexpr double kDegrees = 57.29577951308232;

void draw_render(uint16_t width, uint16_t height, DebugOverlay::RenderOptions& options) {
  ImGui::Text("%s | %u x %u", bgfx::getRendererName(bgfx::getRendererType()), width, height);
  if (const auto* stats = bgfx::getStats()) {
    const auto ms = [](int64_t ticks, int64_t frequency) {
      return frequency > 0 ? ticks * 1000. / frequency : 0.;
    };
    const auto frame = ms(stats->cpuTimeFrame, stats->cpuTimerFreq);
    ImGui::Text("%.1f FPS | %.2f ms/frame", frame > 0 ? 1000. / frame : 0., frame);
    ImGui::Text("CPU render %.2f ms | GPU %.2f ms",
                ms(stats->cpuTimeEnd - stats->cpuTimeBegin, stats->cpuTimerFreq),
                ms(stats->gpuTimeEnd - stats->gpuTimeBegin, stats->gpuTimerFreq));
    ImGui::Text("%u draws | %u textures | %u views", stats->numDraw,
                stats->numTextures, stats->numViews);
  }
  ImGui::Text("Collision mesh rebuilds: %llu",
              static_cast<unsigned long long>(scene_instances_floor_builds()));
  const auto audio = host_sfx_stats();
  ImGui::Text("SFX voices: %u | Coalesced: %llu", audio.active,
              static_cast<unsigned long long>(audio.coalesced));
  ImGui::Separator();
  ImGui::Checkbox("VSync", &options.vsync);
  ImGui::Checkbox("Vehicle shadows", &options.shadows);
  ImGui::Checkbox("Map shadows", &options.map_shadows);
  ImGui::Checkbox("Sun flare", &options.sun_flare);
  ImGui::Checkbox("Game OSD", &options.game_osd);
  if (ImGui::Button("Reset render options")) options = {};
}

void draw_tuning(Jvm* vm) {
  if (vm) {
    ImGui::SeparatorText("Driving");
    float help = host_game_options().driving.steering_help * 100.f;
    if (ImGui::SliderFloat("Steering help", &help, 0.f, 100.f, "%.0f%%",
                           ImGuiSliderFlags_NoInput))
      host_game_options_set_steering_help(*vm, help / 100.f);
    ImGui::SetItemTooltip("Saved in Game Options. Slows keyboard steering at speed "
                          "and helps countersteer during a slide.");
    ImGui::SeparatorText("Session");
    ImGui::Text("Simulation time: %.2f s", vm->sim_time());
    float warp = vm->time_warp(-1.f);
    bool changed = ImGui::SliderFloat("Time scale", &warp, 0.f, 4.f, "%.2fx",
                                      ImGuiSliderFlags_NoInput);
    if (ImGui::Button("Normal speed")) { warp = 1.f; changed = true; }
    if (changed)
      vm->natives().invoke("java.lang.System", "timeWarp", "(F)F", nullptr,
                           {Value::make_float(warp)});
  }
  ImGui::SeparatorText("Tyre model");
  auto& tuning = physics::tire_tuning();
  const auto slider = [](const char* label, float& value, float minimum, float maximum) {
    ImGui::SliderFloat(label, &value, minimum, maximum, "%.2fx", ImGuiSliderFlags_NoInput);
  };
  slider("Lateral grip", tuning.lateral_grip, .25f, 2.f);
  slider("Longitudinal grip", tuning.longitudinal_grip, .25f, 2.f);
  slider("Lateral response", tuning.lateral_response, .25f, 2.f);
  slider("Load sensitivity", tuning.load_sensitivity, 0.f, 2.5f);
  if (ImGui::Button("Reset tyre settings")) tuning = {};
}

void draw_section(Jvm* vm, const SectionMachine* sections, bool& edit_fields) {
  if (!vm || !sections) {
    ImGui::TextUnformatted("No script VM attached.");
    return;
  }
  auto* section = sections->actual();
  ImGui::Text("Classes %d | VM heap %zu objects | Last GC freed %zu",
              vm->classes_loaded(), vm->heap_size(), vm->gc_last_freed());
  if (ImGui::Button("Request garbage collection") && vm->request_gc() >= 0)
    vm->request_gc();
  if (!vm->unbound_native().empty())
    ImGui::TextWrapped("Unbound native: %s", vm->unbound_native().c_str());
  if (!section) return;
  ImGui::Checkbox("Edit numeric section fields", &edit_fields);
  if (!ImGui::BeginTable("section-fields", 2, ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) return;
  ImGui::TableSetupColumn("Script field");
  ImGui::TableSetupColumn("Value");
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();
  std::vector<std::string_view> names;
  for (const auto& [name, value] : section->fields)
    if (!name.empty() && name.front() != '_') names.push_back(name);
  std::sort(names.begin(), names.end());
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(names.size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const auto name = names[row];
      auto& value = section->fields.at(std::string(name));
      ImGui::PushID(name.data());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(name.data(), name.data() + name.size());
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1.f);
      switch (value.tag) {
        case ValueTag::Float:
          if (edit_fields && !scene_transition_active()) {
            float edited = value.v.f;
            if (ImGui::DragFloat("##value", &edited, .05f, 0, 0, "%.4g", ImGuiSliderFlags_NoInput) &&
                std::isfinite(edited))
              value = Value::make_float(edited);
          } else ImGui::Text("%.6g", value.v.f);
          break;
        case ValueTag::Int:
          if (edit_fields && !scene_transition_active())
            ImGui::DragScalar("##value", ImGuiDataType_S32, &value.v.i, .2f,
                              nullptr, nullptr, nullptr, ImGuiSliderFlags_NoInput);
          else ImGui::Text("%d", value.v.i);
          break;
        case ValueTag::Obj:
          ImGui::TextUnformatted(value.v.o ? value.v.o->class_name.c_str() : "null");
          break;
        default: ImGui::TextUnformatted("-"); break;
      }
      ImGui::PopID();
    }
  }
  ImGui::EndTable();
}

void draw_osd_inspector() {
  ImGui::Text("Loading %s | Modal %s | Hover index %d",
              osd_loading_dialog() ? "yes" : "no", osd_has_modal() ? "yes" : "no",
              osd_hover_index());
  const auto& widgets = osd_widgets();
  std::vector<size_t> visible;
  for (size_t i = 0; i < widgets.size(); ++i)
    if (!widgets[i].hidden && !widgets[i].dead) visible.push_back(i);
  ImGui::Text("%zu visible widgets / %zu registered", visible.size(), widgets.size());
  if (!ImGui::BeginTable("osd-widgets", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) return;
  for (const char* name : {"Index", "Command", "Group / layer", "Label"})
    ImGui::TableSetupColumn(name);
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(visible.size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const size_t i = visible[row];
      const auto& widget = widgets[i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%zu", i);
      ImGui::TableNextColumn(); ImGui::Text("%d%s", widget.cmd, widget.disabled ? " (off)" : "");
      ImGui::TableNextColumn(); ImGui::Text("%d / %d", widget.group, widget.layer);
      ImGui::TableNextColumn();
      const auto length = widget.tip.find_first_of("\r\n");
      ImGui::TextUnformatted(widget.tip.data(),
          widget.tip.data() + std::min(length, widget.tip.size()));
    }
  }
  ImGui::EndTable();
}

int vehicle_id(Object* vehicle) {
  const auto* node = gameref_get(vehicle);
  return node ? node->live_id : 0;
}

std::string vehicle_label(Object* vehicle) {
  const auto* node = gameref_get(vehicle);
  const std::string& name = node && !node->alias.empty() ? node->alias : vehicle->class_name;
  return name + " #" + std::to_string(vehicle_id(vehicle));
}

void draw_vehicle(const PartPhysicsState& state) {
  ImGui::Text("%.1f km/h | %.0f rpm | Gear %d", state.speed_kph, state.rpm, state.gear);
  ImGui::Text("Physics %s | Mass %.0f kg | Yaw %.1f deg/s",
              state.active ? "active" : "inactive", state.mass, state.yaw_rate * kDegrees);
  ImGui::Text("Steering %.0f%% help | %+.1f deg requested | %+.1f deg applied | %+.1f deg assist",
              state.steering_help * 100., state.steering_request * kDegrees,
              state.steering_angle * kDegrees, state.steering_correction * kDegrees);
  ImGui::Text("Ticks %llu | Backlog %llu",
              static_cast<unsigned long long>(state.ticks),
              static_cast<unsigned long long>(state.backlog));
  if (!ImGui::BeginTable("wheels", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    return;
  for (const char* name : {"Wheel", "Load / steer", "Slip X / Y", "Force X / Y"})
    ImGui::TableSetupColumn(name);
  ImGui::TableHeadersRow();
  const char* names[] = {"FL", "FR", "RL", "RR"};
  for (size_t i = 0; i < state.tires.size(); ++i) {
    const auto& tire = state.tires[i];
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("%s %s", names[i], state.contact[i] ? "ground" : "air");
    ImGui::TableNextColumn();
    ImGui::Text("%.0f N / %+.1f deg", state.force[i], tire.steer * kDegrees);
    ImGui::TableNextColumn();
    ImGui::Text("%+.2f / %+.2f m/s", tire.slip_speed, tire.lateral_speed);
    ImGui::TableNextColumn();
    ImGui::Text("%+.0f / %+.0f N", tire.longitudinal_force, tire.lateral_force);
  }
  ImGui::EndTable();
}

void draw_input() {
  const auto& input = host_input();
  ImGui::Text("Steering %+.3f (%s)", input.drive_steer,
              input.drive_steer_analog ? "analog" : "digital");
  ImGui::Text("Throttle %.3f | Brake %.3f | Clutch %.3f",
              input.drive_throttle, input.drive_brake, input.drive_clutch);
  ImGui::Text("Handbrake %s | Reverse %s | Shift %+.2f",
              input.drive_handbrake ? "on" : "off",
              input.drive_reverse ? "on" : "off", input.drive_shift);
}
}  // namespace

void DebugOverlay::set_context(Jvm& jvm, const SectionMachine& sections) {
  jvm_ = &jvm;
  sections_ = &sections;
}

void DebugOverlay::draw_vehicle_tab(std::span<Object* const> vehicles) {
  Object* player_chassis = nullptr;
  if (vehicles.empty() && jvm_) {
    const auto player = jvm_->get_static("java.game.GameLogic", "player");
    if (player.tag == ValueTag::Obj && player.v.o) {
      const auto field = player.v.o->fields.find("car");
      if (field != player.v.o->fields.end() && field->second.tag == ValueTag::Obj)
        player_chassis = part_debug_chassis(field->second.v.o);
    }
    if (player_chassis) vehicles = std::span<Object* const>(&player_chassis, 1);
  }
  auto selected = std::find_if(vehicles.begin(), vehicles.end(),
      [&](Object* object) { return vehicle_id(object) == selected_vehicle_; });
  if (selected == vehicles.end() && !vehicles.empty()) {
    selected = vehicles.begin();
    selected_vehicle_ = vehicle_id(*selected);
    freeze_vehicle_ = false;
  }
  if (selected != vehicles.end() &&
      ImGui::BeginCombo("Vehicle", vehicle_label(*selected).c_str())) {
    for (auto candidate = vehicles.begin(); candidate != vehicles.end(); ++candidate) {
      if (ImGui::Selectable(vehicle_label(*candidate).c_str(), candidate == selected)) {
        selected = candidate;
        selected_vehicle_ = vehicle_id(*selected);
        freeze_vehicle_ = false;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::Checkbox("Freeze telemetry", &freeze_vehicle_);
  if (!freeze_vehicle_)
    have_vehicle_ = selected != vehicles.end() && part_physics_state(*selected, vehicle_);
  if (have_vehicle_) draw_vehicle(vehicle_);
  else ImGui::TextUnformatted("No vehicle in this section.");
}

void DebugOverlay::draw(HWND hwnd, uint16_t width, uint16_t height,
                        std::span<Object* const> vehicles) {
  if (!open_) return;
  if (!initialized_) {
    imguiCreate();
    ImGui::GetIO().IniFilename = nullptr;
    initialized_ = true;
  }
  POINT point{};
  if (!GetCursorPos(&point) || !ScreenToClient(hwnd, &point))
    point = {-1, -1};
  imguiBeginFrame(point.x, point.y, mouse_buttons_, 0, width, height, -1, kDebugView);
  bgfx::setViewName(kDebugView, "OpenSLRR debug overlay");
  ImGui::SetNextWindowSize(ImVec2(std::min(640.f, width * .9f),
                                 std::min(380.f, height * .7f)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(20, 100), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("OpenSLRR Debug", &open_)) {
    ImGui::Text("Section: %s", sections_ ? sections_->actual_name().c_str() : "(none)");
    if (ImGui::BeginTabBar("debug-tabs")) {
      if (ImGui::BeginTabItem("Vehicle")) {
        draw_vehicle_tab(vehicles);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Tuning")) {
        draw_tuning(jvm_);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Section")) {
        ImGui::PushID(sections_ ? sections_->actual() : nullptr);
        draw_section(jvm_, sections_, edit_fields_);
        ImGui::PopID();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("OSD")) {
        draw_osd_inspector();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Renderer")) {
        draw_render(width, height, render_options_);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Input")) {
        draw_input();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
  imguiEndFrame();
}

bool DebugOverlay::captures_mouse() const {
  return mouse_buttons_ || (initialized_ && open_ && ImGui::GetIO().WantCaptureMouse);
}

bool DebugOverlay::handle_message(HWND hwnd, UINT message, WPARAM key,
                                  LPARAM detail, bool game_dragging) {
  if (key == VK_F12) {
    if (message == WM_KEYDOWN && (shortcut_down_ || GetKeyState(VK_CONTROL) < 0)) {
      if (!(detail & (1L << 30))) open_ = !open_;
      shortcut_down_ = true;
      return true;
    }
    if (message == WM_KEYUP && shortcut_down_) {
      shortcut_down_ = false;
      return true;
    }
  }
  if (message == WM_KILLFOCUS) shortcut_down_ = false;
  if (message == WM_KILLFOCUS || message == WM_CANCELMODE || message == WM_CAPTURECHANGED) {
    if (initialized_ && mouse_buttons_) {
      for (int button = 0; button < 3; ++button)
        ImGui::GetIO().AddMouseButtonEvent(button, false);
    }
    mouse_buttons_ = 0;
    return false;
  }
  if (!initialized_ || game_dragging) return false;
  const bool capture = captures_mouse();
  if (message == WM_SETCURSOR && capture && LOWORD(detail) == HTCLIENT) {
    SetCursor(LoadCursorA(nullptr, IDC_ARROW));
    return true;
  }
  if (message == WM_MOUSEMOVE) return capture;
  if (message == WM_MOUSEWHEEL && capture) {
    ImGui::GetIO().AddMouseWheelEvent(0.f, GET_WHEEL_DELTA_WPARAM(key) / float(WHEEL_DELTA));
    return true;
  }
  int button;
  switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: button = 0; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: button = 1; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: button = 2; break;
    default: return false;
  }
  const auto mask = static_cast<uint8_t>(1u << button);
  const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                    message == WM_MBUTTONDOWN;
  if (down ? !capture : !(mouse_buttons_ & mask)) return false;
  ImGui::GetIO().AddMousePosEvent(static_cast<short>(LOWORD(detail)),
                                 static_cast<short>(HIWORD(detail)));
  ImGui::GetIO().AddMouseButtonEvent(button, down);
  if (down) {
    if (!mouse_buttons_) SetCapture(hwnd);
    mouse_buttons_ |= mask;
  } else {
    mouse_buttons_ &= ~mask;
    if (!mouse_buttons_ && GetCapture() == hwnd) ReleaseCapture();
  }
  return true;
}

void DebugOverlay::shutdown() {
  if (initialized_) imguiDestroy();
  initialized_ = open_ = shortcut_down_ = false;
  freeze_vehicle_ = have_vehicle_ = false;
  edit_fields_ = false;
  selected_vehicle_ = 0;
  sections_ = nullptr;
  jvm_ = nullptr;
  render_options_ = {};
  physics::tire_tuning() = {};
  mouse_buttons_ = 0;
}

}  // namespace slrr
