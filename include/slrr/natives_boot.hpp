#pragma once

#include "slrr/jvm.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/section_machine.hpp"

#include <functional>

namespace slrr {

// Registers System / File / FindFile / GameLogic / OSD stubs for career boot.
void register_boot_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections);

// Object.wait pumps this until notify. Empty pump: wait returns at once
// (boot paths that run before the window loop).
using HostScriptPump = std::function<bool()>;
void host_set_script_pump(HostScriptPump fn);
void install_osd_script_hover(Jvm& jvm);

// Frontend.setFonts + Config.video_x/y — pick SLII11 vs SLII17 from resolution
// (stock Layout). Call after the window size is known, before Garage.enter.
void frontend_set_fonts(Jvm& jvm, int32_t video_x, int32_t video_y);

}  // namespace slrr
