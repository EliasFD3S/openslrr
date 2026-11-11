#include "slrr/platform_telemetry.hpp"

#include "slrr/diagnostics.hpp"
#include "slrr/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <vector>

namespace slrr::platform_telemetry {
namespace {

struct InputAxis {
  std::int32_t source = 0, fallback = 0;
  std::int32_t profile_bindings = 0, xinput_bindings = 0;
};

struct State {
  std::mutex mutex;
  RendererDiagnostics renderer;
  std::uint64_t directshow_samples = 0, directshow_rejected = 0;
  std::uint64_t vlc_samples = 0, vlc_rejected = 0, vlc_closes = 0;
  std::uint64_t vlc_callback_begin[5]{}, vlc_callback_end[5]{};
  MediaSample last_directshow, last_vlc;
  std::map<std::int32_t, InputAxis> input_axes;
};

State& state() {
  static State instance;
  return instance;
}

std::atomic<std::uint64_t> g_renderer_generation{0};
std::atomic<std::uint64_t> g_vlc_generation{0};

std::int64_t metric_value(std::uint64_t value) {
  return value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
      ? std::numeric_limits<std::int64_t>::max()
      : static_cast<std::int64_t>(value);
}

void event(const char* name, std::initializer_list<diagnostics::Value> values) noexcept {
  try {
    diagnostics::Event event;
    event.category = diagnostics::Category::Runtime;
    event.name = name;
    event.values.assign(values.begin(), values.end());
    diagnostics::trace(std::move(event));
  } catch (...) {
    // Observation must never cross or alter a native/media callback boundary.
  }
}

void provide(void*, std::vector<metrics::Value>& values) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  const auto add = [&](const char* name, std::int64_t value,
                       metrics::Kind kind = metrics::Kind::Gauge) {
    values.push_back({name, value, kind});
  };
  const auto& r = s.renderer;
  add("renderer.open", r.open);
  add("renderer.generation", metric_value(r.generation));
  add("renderer.frames_submitted", metric_value(r.submitted_frames), metrics::Kind::Counter);
  add("renderer.stats_valid", r.stats_valid);
  add("renderer.views_truncated", r.views_truncated);
  add("renderer.views", r.frame.view_count);
  add("renderer.draw_calls", r.frame.draw_calls);
  add("renderer.compute_calls", r.frame.compute_calls);
  add("renderer.blit_calls", r.frame.blit_calls);
  add("renderer.gpu_vertex_buffers", r.frame.vertex_buffers);
  add("renderer.gpu_index_buffers", r.frame.index_buffers);
  add("renderer.gpu_dynamic_vertex_buffers", r.frame.dynamic_vertex_buffers);
  add("renderer.gpu_dynamic_index_buffers", r.frame.dynamic_index_buffers);
  add("renderer.gpu_textures", r.frame.textures);
  add("renderer.gpu_frame_buffers", r.frame.frame_buffers);
  add("renderer.fmv_texture_handles", r.fmv_texture_handles);
  add("media.directshow.samples", metric_value(s.directshow_samples), metrics::Kind::Counter);
  add("media.directshow.rejected", metric_value(s.directshow_rejected), metrics::Kind::Counter);
  add("media.libvlc.samples", metric_value(s.vlc_samples), metrics::Kind::Counter);
  add("media.libvlc.rejected", metric_value(s.vlc_rejected), metrics::Kind::Counter);
  add("media.libvlc.closes", metric_value(s.vlc_closes), metrics::Kind::Counter);
  for (std::size_t i = 1; i < 5; ++i) {
    static constexpr const char* begin[] = {"", "media.libvlc.format.begin", "media.libvlc.lock.begin",
        "media.libvlc.display.begin", "media.libvlc.cleanup.begin"};
    static constexpr const char* end[] = {"", "media.libvlc.format.end", "media.libvlc.lock.end",
        "media.libvlc.display.end", "media.libvlc.cleanup.end"};
    add(begin[i], metric_value(s.vlc_callback_begin[i]), metrics::Kind::Counter);
    add(end[i], metric_value(s.vlc_callback_end[i]), metrics::Kind::Counter);
  }
  for (const auto& [axis, input] : s.input_axes) {
    const std::string base = "input.axis." + std::to_string(axis);
    values.push_back({base + ".source", input.source, metrics::Kind::Gauge});
    values.push_back({base + ".fallback", input.fallback, metrics::Kind::Gauge});
  }
}

const bool provider_registered = [] {
  return metrics::register_provider(&provide) != 0;
}();

}  // namespace

std::uint64_t next_renderer_generation() noexcept {
  return g_renderer_generation.fetch_add(1, std::memory_order_relaxed) + 1;
}
std::uint64_t next_vlc_generation() noexcept {
  return g_vlc_generation.fetch_add(1, std::memory_order_relaxed) + 1;
}

void renderer_open(std::uint64_t generation) noexcept {
  if (!generation) return;
  {
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.renderer = {};
    s.renderer.open = true;
    s.renderer.generation = generation;
  }
  event("renderer.open", {{"generation", metric_value(generation)}});
}

void renderer_frame(std::uint64_t generation, const RendererFrameStats& stats,
                    bool fmv_texture_active) noexcept {
  {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.renderer.open || s.renderer.generation != generation) return;
    ++s.renderer.submitted_frames;
    s.renderer.frame = stats;
    s.renderer.stats_valid = true;
    s.renderer.views_truncated = stats.copied_views < stats.view_count;
    s.renderer.fmv_texture_handles = fmv_texture_active ? 1 : 0;
  }
}

void renderer_close(std::uint64_t generation) noexcept {
  bool matched = false;
  {
    State& s = state();
    std::lock_guard lock(s.mutex);
    matched = s.renderer.open && s.renderer.generation == generation;
    if (matched) {
      s.renderer.open = false;
      s.renderer.stats_valid = false;
      s.renderer.fmv_texture_handles = 0;
      s.renderer.frame.vertex_buffers = s.renderer.frame.index_buffers = 0;
      s.renderer.frame.dynamic_vertex_buffers = s.renderer.frame.dynamic_index_buffers = 0;
      s.renderer.frame.textures = s.renderer.frame.frame_buffers = 0;
      s.renderer.frame.view_count = 0;
      s.renderer.frame.copied_views = 0;
      s.renderer.frame.views = {};
    }
  }
  if (matched) event("renderer.close", {{"generation", metric_value(generation)}});
}

RendererDiagnostics renderer_snapshot() noexcept {
  State& s = state();
  std::lock_guard lock(s.mutex);
  return s.renderer;
}

void media_sample(MediaBackend backend, std::uint64_t generation,
                  const MediaSample& sample, bool accepted) noexcept {
  const bool directshow = backend == MediaBackend::DirectShow;
  {
    State& s = state();
    std::lock_guard lock(s.mutex);
    auto& samples = directshow ? s.directshow_samples : s.vlc_samples;
    auto& rejected = directshow ? s.directshow_rejected : s.vlc_rejected;
    if (accepted) ++samples;
    else ++rejected;
    (directshow ? s.last_directshow : s.last_vlc) = sample;
  }
  event(directshow ? "media.directshow.sample" : "media.libvlc.sample",
        {{"generation", metric_value(generation)},
         {"width", sample.width}, {"height", sample.height},
         {"pitch", sample.pitch}, {"byte_length", sample.byte_length},
         {"orientation", static_cast<std::int32_t>(sample.orientation)},
         {"accepted", accepted}});
}

void vlc_callback(std::uint64_t generation, VlcCallback callback,
                  CallbackPhase phase) noexcept {
  const auto index = static_cast<std::size_t>(callback);
  if (!generation || index == 0 || index >= 5) return;
  {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (phase == CallbackPhase::Begin) ++s.vlc_callback_begin[index];
    else ++s.vlc_callback_end[index];
  }
  event("media.libvlc.callback",
        {{"generation", metric_value(generation)},
         {"callback", static_cast<std::int32_t>(callback)},
         {"phase", static_cast<std::int32_t>(phase)}});
}

void vlc_decoder_open(std::uint64_t generation) noexcept {
  if (generation) event("media.libvlc.open", {{"generation", metric_value(generation)}});
}

void vlc_decoder_close(std::uint64_t generation, std::int32_t reason) noexcept {
  if (!generation) return;
  {
    State& s = state();
    std::lock_guard lock(s.mutex);
    ++s.vlc_closes;
  }
  event("media.libvlc.close", {{"generation", metric_value(generation)}, {"reason", reason}});
}

void input_axis(std::int32_t axis, std::int32_t source, std::int32_t fallback,
                std::int32_t profile_bindings, std::int32_t xinput_bindings) noexcept {
  if (axis < 0) return;
  try {
    bool changed = false;
    {
      State& s = state();
      std::lock_guard lock(s.mutex);
      const InputAxis next{source, fallback, profile_bindings, xinput_bindings};
      auto [it, inserted] = s.input_axes.try_emplace(axis, next);
      changed = inserted || it->second.source != source || it->second.fallback != fallback ||
          it->second.profile_bindings != profile_bindings ||
          it->second.xinput_bindings != xinput_bindings;
      if (changed) it->second = next;
    }
    if (changed) {
      event("input.axis.binding", {{"axis", axis}, {"source", source}, {"fallback", fallback},
          {"profile_bindings", profile_bindings}, {"xinput_bindings", xinput_bindings}});
    }
  } catch (...) {
  }
}

void reset_observations_for_tests() noexcept {
  State& s = state();
  std::lock_guard lock(s.mutex);
  s.renderer = {};
  s.directshow_samples = s.directshow_rejected = 0;
  s.vlc_samples = s.vlc_rejected = s.vlc_closes = 0;
  std::fill(std::begin(s.vlc_callback_begin), std::end(s.vlc_callback_begin), 0);
  std::fill(std::begin(s.vlc_callback_end), std::end(s.vlc_callback_end), 0);
  s.last_directshow = s.last_vlc = {};
  s.input_axes.clear();
}

}  // namespace slrr::platform_telemetry
