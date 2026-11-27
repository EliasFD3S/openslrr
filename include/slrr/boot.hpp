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
  // --auto-cmd demo: from the main menu, run its DEMO command (an AI race in
  // Valo City). With --bench-drive it is measured like the benchmark: fixed
  // 1/60 s step, --bench-time hour, then quit (#358).
  bool auto_demo = false;
  // --seed N: fixed seed for the script RNG (Math.randomize), -1 = clock.
  int64_t random_seed = -1;
  int32_t max_frames = 0;  // 0 = until quit
  // --perf-log: per-second frame timing on stdout, plus CSV rows when a path
  // other than "-" is given.
  bool perf_log = false;
  std::string perf_log_path;
  // --bench-drive: once the player drives in an outdoor world, hold full
  // throttle with the wheel straight for this many simulated seconds, then
  // quit. The run advances 1/60 s per frame, so every build covers the same
  // path frame by frame however fast it renders.
  float bench_drive_seconds = 0.f;
  bool bench_realtime=false; // Preserve wall-clock stalls/catch-up while profiling.
  bool physics_batch=false;
  std::string physics_trace;  // --physics-trace FILE (#380)  // --physics-batch: frame-locked physics instead of the free-running thread (#380)
  // --bench-map: from the garage, start the GameLogic.MapBox track whose
  // script class name contains this text (ValoCity_Track for the city).
  std::string bench_map;
  // --bench-traffic: java.util.Config.trafficDensity for the bench map
  // (0..1; -1 keeps the saved option). Measures physics with traffic (#380).
  float bench_traffic = -1.f;
  std::string bench_career; // Relative save directory, for reproducing career performance.
  // --bench-time: game hour set when the benchmark drive starts (-1 keeps it).
  float bench_hour = -1.f;
  // --bench-shots: directory for window captures (TGA) at the start of the
  // benchmark drive and every 10 seconds after it.
  std::string bench_shots;
  // --record-path: write the player car's pose every frame of normal play
  // (slrr/drive_path.hpp). --replay-path: during the benchmark drive, move
  // the car along such a recording instead of driving it; the drive lasts
  // the recording unless --bench-drive is given.
  std::string record_path;
  std::string replay_path;
  // --wind SPEED[,HEADING]: mean wind in m/s at 10 m height (a gentle breeze
  // by default, 0 for calm air), blowing towards HEADING degrees from +Z
  // towards +X.
  float wind_speed = 4.f, wind_heading = 60.f;
  bool vsync = true;          // --no-vsync clears it
  bool debug_overlay = false; // --debug-overlay opens the Ctrl+F12 panel
  RenderBackendKind render = RenderBackendKind::Bgfx;
};

// Probe only: validate game root + classpath bootstrap, no window loop.
int boot_probe(Jvm& jvm, const BootOptions& opt);

// Interactive / smoke path (Splash → MainMenu → CMD_NEW → Garage).
// Returns 0 on success. Full section machine lands incrementally.
int boot_game(Jvm& jvm, IRenderBackend& render, const BootOptions& opt);

// Runs stock Init; initializes mods only if it completed (no abort, no unbound
// native). Returns whether mods were initialized.
bool boot_stock_init_and_mods(Jvm& jvm);

}  // namespace slrr
