#pragma once

// Opt-in frame timing for performance work (--perf-log). Disabled scopes cost
// one branch. Enabled, each second prints one `[perf]` line to stdout and, if
// a path was given, appends the same fields as one CSV row.

#include <cstdint>
#include <string>

namespace slrr::perf {

enum class Zone : uint8_t {
  Scripts,      // script threads, messages, callbacks and GameType timers
  Physics,      // part_physics_advance
  Flush,        // render.flush(), all of it
  Collect,      // scene collection and GPU residency (sync_instances)
  Shadows,      // map and vehicle shadow maps
  Reflections,  // reflection cubemap faces
  World,        // sky, world, flare and previews submission
  Osd,          // interface and debug overlay submission
  Present,      // bgfx::frame(): waits for the render thread
  Count
};

// Renderer-side numbers for the frame bgfx finished last.
struct RenderSample {
  double render_thread_ms = 0.;  // bgfx render thread (API submission)
  double gpu_ms = 0.;
  double wait_render_ms = 0.;    // main thread blocked on the render thread
  uint32_t draws = 0;
  uint32_t instances = 0;        // individually submitted scene instances
  uint32_t batch_instances = 0;  // instanced prop placements
};

void enable(const std::string& csv_path);
bool enabled();
// Drops the current window. With frames > 0 a window closes after that many
// frames instead of one wall-clock second (one simulated second of a
// fixed-step benchmark), so rows of different builds cover the same street.
void restart(int frames_per_window);
void add(Zone zone, int64_t ticks);
void set_render_sample(const RenderSample& sample);
// Call once per host frame, after the frame was presented.
void end_frame();
int64_t now_ticks();

class Scope {
 public:
  explicit Scope(Zone zone) : zone_(zone), start_(enabled() ? now_ticks() : 0) {}
  ~Scope() {
    if (start_) add(zone_, now_ticks() - start_);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Zone zone_;
  int64_t start_;
};

}  // namespace slrr::perf
