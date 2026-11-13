#include "slrr/frame_profiler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

namespace slrr::perf {
namespace {

constexpr const char* kZoneNames[] = {"scripts", "physics", "flush",   "collect", "shadows",
                                      "refl",    "world",   "osd",     "present"};
static_assert(std::size(kZoneNames) == static_cast<size_t>(Zone::Count));

struct State {
  bool enabled = false;
  std::FILE* csv = nullptr;
  int64_t window_start = 0;
  int64_t last_frame = 0;
  std::array<int64_t, static_cast<size_t>(Zone::Count)> frame_zones{};
  std::array<double, static_cast<size_t>(Zone::Count)> window_zones{};
  std::vector<double> frame_ms;
  double render_thread = 0., gpu = 0., wait_render = 0.;
  double draws = 0., instances = 0., batch_instances = 0.;
  uint32_t render_samples = 0;
  uint32_t second = 0;
  int frames_per_window = 0;
};

State& state() {
  static State s;
  return s;
}

double to_ms(int64_t ticks) { return static_cast<double>(ticks) * 1e-6; }

}  // namespace

int64_t now_ticks() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void enable(const std::string& csv_path) {
  auto& s = state();
  s.enabled = true;
  if (!csv_path.empty() && !s.csv) {
    s.csv = std::fopen(csv_path.c_str(), "w");
    if (s.csv) {
      std::fprintf(s.csv, "second,frames,fps,frame_avg_ms,frame_p95_ms,frame_max_ms");
      for (const char* name : kZoneNames) std::fprintf(s.csv, ",%s_ms", name);
      std::fprintf(s.csv,
                   ",render_thread_ms,gpu_ms,wait_render_ms,draws,instances,batch_instances\n");
      std::fflush(s.csv);
    } else {
      std::fprintf(stderr, "[perf] cannot open %s\n", csv_path.c_str());
    }
  }
}

bool enabled() { return state().enabled; }

void restart(int frames_per_window) {
  auto& s = state();
  s.frames_per_window = std::max(0, frames_per_window);
  s.last_frame = s.window_start = 0;
  s.frame_zones.fill(0);
  s.window_zones.fill(0.);
  s.frame_ms.clear();
  s.render_thread = s.gpu = s.wait_render = 0.;
  s.draws = s.instances = s.batch_instances = 0.;
  s.render_samples = 0;
  s.second = 0;
}

void add(Zone zone, int64_t ticks) {
  state().frame_zones[static_cast<size_t>(zone)] += ticks;
}

void set_render_sample(const RenderSample& sample) {
  auto& s = state();
  if (!s.enabled) return;
  s.render_thread += sample.render_thread_ms;
  s.gpu += sample.gpu_ms;
  s.wait_render += sample.wait_render_ms;
  s.draws += sample.draws;
  s.instances += sample.instances;
  s.batch_instances += sample.batch_instances;
  ++s.render_samples;
}

void end_frame() {
  auto& s = state();
  if (!s.enabled) return;
  const int64_t now = now_ticks();
  if (!s.last_frame) {
    s.last_frame = s.window_start = now;
    s.frame_zones.fill(0);
    return;
  }
  s.frame_ms.push_back(to_ms(now - s.last_frame));
  s.last_frame = now;
  for (size_t i = 0; i < s.frame_zones.size(); ++i) s.window_zones[i] += to_ms(s.frame_zones[i]);
  s.frame_zones.fill(0);
  const double window = to_ms(now - s.window_start);
  if (s.frames_per_window > 0 ? static_cast<int>(s.frame_ms.size()) < s.frames_per_window
                              : window < 1000.)
    return;

  const size_t frames = s.frame_ms.size();
  double total = 0.;
  for (const double ms : s.frame_ms) total += ms;
  std::vector<double> sorted = s.frame_ms;
  std::sort(sorted.begin(), sorted.end());
  const double p95 = sorted[std::min(frames - 1, frames * 95 / 100)];
  const double worst = sorted.back();
  const double avg = total / static_cast<double>(frames);
  const double samples = s.render_samples ? static_cast<double>(s.render_samples) : 1.;
  ++s.second;
  std::printf("[perf] t=%us fps=%.1f frame avg=%.2f p95=%.2f max=%.2f ms |", s.second,
              1000. * static_cast<double>(frames) / window, avg, p95, worst);
  for (size_t i = 0; i < s.window_zones.size(); ++i)
    std::printf(" %s=%.2f", kZoneNames[i], s.window_zones[i] / static_cast<double>(frames));
  std::printf(" | render_thread=%.2f gpu=%.2f wait_render=%.2f ms draws=%.0f inst=%.0f batched=%.0f\n",
              s.render_thread / samples, s.gpu / samples, s.wait_render / samples,
              s.draws / samples, s.instances / samples, s.batch_instances / samples);
  std::fflush(stdout);
  if (s.csv) {
    std::fprintf(s.csv, "%u,%zu,%.2f,%.3f,%.3f,%.3f", s.second, frames,
                 1000. * static_cast<double>(frames) / window, avg, p95, worst);
    for (const double zone : s.window_zones)
      std::fprintf(s.csv, ",%.3f", zone / static_cast<double>(frames));
    std::fprintf(s.csv, ",%.3f,%.3f,%.3f,%.0f,%.0f,%.0f\n", s.render_thread / samples,
                 s.gpu / samples, s.wait_render / samples, s.draws / samples,
                 s.instances / samples, s.batch_instances / samples);
    std::fflush(s.csv);
  }
  s.frame_ms.clear();
  s.window_zones.fill(0.);
  s.render_thread = s.gpu = s.wait_render = 0.;
  s.draws = s.instances = s.batch_instances = 0.;
  s.render_samples = 0;
  s.window_start = now;
}

}  // namespace slrr::perf
