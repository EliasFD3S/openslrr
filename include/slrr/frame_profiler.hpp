#pragma once

// Opt-in frame timing for performance work (--perf-log). Disabled scopes cost
// one branch. Enabled, each second prints one `[perf]` line to stdout and, if
// a path was given, appends the same fields as one CSV row.

#include <array>
#include <cstdint>
#include <string>

namespace slrr::perf {

enum class Zone : uint8_t {
  Scripts,     // script threads, messages, callbacks and GameType timers
  Physics,     // part_physics_begin/end on the host: prepare, wait, publish
  Flush,       // render.flush(), all of it
  Collect,     // scene collection and GPU residency (sync_instances)
  Shadows,     // map and vehicle shadow maps
  Reflections, // reflection cubemap faces
  World,       // sky, world, flare and previews submission
  Osd,         // interface and debug overlay submission
  Present,     // bgfx::frame(): waits for the render thread
  Traffic,     // traffic streaming and notifications, separate from physics
  Vehicles,    // the physics thread's batch (#203), added on the host; not nested
  Pedestrians, // pedestrian publication/stepping, nested inside Physics
  Collision,   // scene collision preparation, nested inside Physics
  Gc,          // end-of-frame VM collection
  // Renderer detail (#243), nested inside the zones above.
  CollectBatches, // instanced prop batches, inside Collect
  SunShadows,     // sun cascades / point shadow, inside Shadows
  CarDepth,       // daytime vehicle depth map, inside Shadows
  FixtureShadows, // garage fixture atlas
  LampSelect,     // street lamp selection (update_local_light)
  LampShadows,    // lamp and car lamp shadow tiles, inside Shadows
  AmbientOcclusion, // AO prepass and passes submission
  WorldList,      // the frame's world list build (#264), inside the first collection
  WorldCopy,      // world list copies in scene_instances_collect, every collection (#264)
  // The rest of sync_instances (#266), inside Collect.
  CollectInstances, // scene_instances_collect of the main view
  CollectSkins,     // walker palettes in view, inside CollectInstances (every collection)
  CollectPreviews,  // script viewport previews
  Residency,        // GPU residency check, uploads and releases
  // Worker encoders (#270), inside Flush.
  WorkerSubmit,     // AO prepass, car tiles and reflection faces, summed over workers
  SubmitWait,       // host thread waiting for those workers after the world
  Interpolation,    // render-pose publication, nested inside Physics
  SunCollect,       // sun cascade outer-frustum collection, inside SunShadows (#359)
  Count
};

// GPU time per renderer pass (#362): bgfx views grouped by what they draw.
// render_debug.hpp:render_gpu_pass maps a view to its pass.
enum class GpuPass {
  MapShadows,      // sun cascades, or the six point-light faces in a garage
  CarDepth,        // vehicle depth map for the sun
  InteriorSun,     // interior sun map and aperture
  World,           // opaque world views and the world view
  AoPrepass,       // AO depth prepass (geometry)
  AmbientOcclusion, // AO geometry, AO and the two filters
  Reflections,     // dynamic reflection cube faces
  LampTiles,       // fixture, street lamp and car lamp shadow tiles
  HeadlightTiles,  // headlight shadow tiles
  Previews,        // script viewport previews (garage part views)
  Osd,             // FMV, OSD and debug overlay
  Other,
  Count
};
const char *gpu_pass_name(GpuPass pass);

// Renderer-side numbers for the frame bgfx finished last.
struct RenderSample {
  double render_thread_ms = 0.; // bgfx render thread (API submission)
  double gpu_ms = 0.;
  double wait_render_ms = 0.; // main thread blocked on the render thread
  uint32_t draws = 0;
  uint32_t instances = 0;       // individually submitted scene instances
  uint32_t batch_instances = 0; // instanced prop placements
  // Per-pass GPU times from the bgfx profiler, on while profiling (#362).
  std::array<double, static_cast<size_t>(GpuPass::Count)> gpu_pass_ms{};
};

struct Snapshot {
  bool ready = false;
  double frame_ms = 0, worst_ms = 0;
  std::array<double, static_cast<size_t>(Zone::Count)> zones{};
};
Snapshot snapshot();
void enable(const std::string &csv_path);
bool enabled();
// Drops the current window. With frames > 0 a window closes after that many
// frames instead of one wall-clock second (one simulated second of a
// fixed-step benchmark), so rows of different builds cover the same street.
void restart(int frames_per_window);
void add(Zone zone, int64_t ticks);
void set_render_sample(const RenderSample &sample);
// Call once per host frame, after the frame was presented.
void end_frame();
int64_t now_ticks();

// True on a thread that submits a view on a worker encoder (#270). Zones
// are host-thread data: scopes on such a thread record nothing, and the
// caller reports the worker time as WorkerSubmit.
bool &worker_thread();

class Scope {
public:
  explicit Scope(Zone zone)
      : zone_(zone), start_(enabled() && !worker_thread() ? now_ticks() : 0) {}
  ~Scope() {
    if (start_)
      add(zone_, now_ticks() - start_);
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;

private:
  Zone zone_;
  int64_t start_;
};

} // namespace slrr::perf
