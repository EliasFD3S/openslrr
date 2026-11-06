#pragma once

#include "slrr/jvm.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/section_machine.hpp"

#include <functional>

namespace slrr {

// Registers System / File / FindFile / GameLogic / OSD stubs for career boot.
void register_boot_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections);
void register_gc_natives(Jvm& jvm);

// Object.wait pumps this until notify. Empty pump: wait returns at once
// (boot paths that run before the window loop).
using HostScriptPump = std::function<bool()>;
void host_set_script_pump(HostScriptPump fn);

// LoadingScreen.show paints one frame before its body, so the plate is up
// while the click handler keeps running. Empty until boot installs it.
void host_set_loading_present(std::function<void()> fn);
void host_loading_present();
void install_osd_script_hover(Jvm& jvm);

// Frontend.setFonts + Config.video_x/y — pick SLII11 vs SLII17 from resolution
// (stock Layout). Call after the window size is known, before Garage.enter.
void frontend_set_fonts(Jvm& jvm, int32_t video_x, int32_t video_y);

}  // namespace slrr
