#pragma once

#include "slrr/jvm.hpp"
#include "slrr/render_backend.hpp"

#include <string>

namespace slrr {

struct BootOptions {
  std::string game_root;
  std::string player_name = "Player";
  std::string spawn_car; // development garage: owning RPK path:local type ID
  std::string vehicle_osd; // optional standalone gauge CFG for preview
  bool window = false;
  bool auto_new = false;
  // After Garage: fire Garage.osdCommand (101 MAINMENU / 109 HITTHESTREET / 0=skip).
  int32_t auto_cmd = 0;
  int32_t max_frames = 0;  // 0 = until quit
  RenderBackendKind render = RenderBackendKind::Bgfx;
};

// Probe only: validate game root + classpath bootstrap, no window loop.
int boot_probe(Jvm& jvm, const BootOptions& opt);

// Interactive / smoke path (Splash → MainMenu → CMD_NEW → Garage).
// Returns 0 on success. Full section machine lands incrementally.
int boot_game(Jvm& jvm, IRenderBackend& render, const BootOptions& opt);

}  // namespace slrr
