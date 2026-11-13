#pragma once

#include "slrr/part_runtime.hpp"

#include <span>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace slrr {
class SectionMachine;

class DebugOverlay {
 public:
  struct RenderOptions {
    bool map_shadows = true;
    int reflection_faces = 1;
    bool vsync = true;
    bool shadows = true;
    bool sun_flare = true;
    bool game_osd = true;
  };
  void set_context(Jvm& jvm, const SectionMachine& sections);
  bool visible() const { return open_; }
  void set_visible(bool open) { open_ = open; }
  const RenderOptions& render_options() const { return render_options_; }
  RenderOptions& render_options() { return render_options_; }
  void draw(HWND hwnd, uint16_t width, uint16_t height, const char* reflection_status,
            std::span<Object* const> vehicles);
  void shutdown();
  bool captures_mouse() const;
  bool handle_message(HWND hwnd, UINT message, WPARAM key, LPARAM detail,
                      bool game_dragging);

 private:
  void draw_vehicle_tab(std::span<Object* const> vehicles);
  bool open_ = false;
  bool initialized_ = false;
  bool shortcut_down_ = false;
  bool freeze_vehicle_ = false;
  bool have_vehicle_ = false;
  bool edit_fields_ = false;
  int32_t selected_vehicle_ = 0;
  uint8_t mouse_buttons_ = 0;
  Jvm* jvm_ = nullptr;
  const SectionMachine* sections_ = nullptr;
  RenderOptions render_options_;
  PartPhysicsState vehicle_;
};

}  // namespace slrr
