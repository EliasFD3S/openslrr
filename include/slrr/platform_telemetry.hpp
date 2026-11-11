#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace slrr::platform_telemetry {

// Numeric-only, owned copies of bgfx statistics. No bgfx pointers or view names
// cross this boundary, and a snapshot remains valid after the next submission.
struct RendererViewStats {
  std::uint16_t view = 0;
  std::int64_t cpu_begin = 0, cpu_end = 0;
  std::int64_t gpu_begin = 0, gpu_end = 0;
  std::uint32_t gpu_frame = 0;
};

struct RendererFrameStats {
  std::uint32_t gpu_frame = 0;
  std::int64_t cpu_frame = 0, cpu_begin = 0, cpu_end = 0, cpu_frequency = 0;
  std::int64_t gpu_begin = 0, gpu_end = 0, gpu_frequency = 0;
  std::uint32_t draw_calls = 0, compute_calls = 0, blit_calls = 0;
  std::uint32_t vertex_buffers = 0, index_buffers = 0, textures = 0;
  std::uint32_t dynamic_vertex_buffers = 0, dynamic_index_buffers = 0;
  std::uint32_t frame_buffers = 0, programs = 0, shaders = 0;
  std::uint16_t view_count = 0;
  std::array<RendererViewStats, 16> views{};
  std::size_t copied_views = 0;
};

struct RendererDiagnostics {
  bool open = false, stats_valid = false, views_truncated = false;
  std::uint64_t generation = 0, submitted_frames = 0;
  std::uint32_t fmv_texture_handles = 0;
  RendererFrameStats frame;
};

enum class MediaBackend : std::int32_t { DirectShow = 1, LibVlc = 2 };
enum class MediaOrientation : std::int32_t { Unknown = 0, TopDown = 1, BottomUp = 2 };
struct MediaSample {
  std::int64_t width = 0, height = 0, pitch = 0, byte_length = 0;
  MediaOrientation orientation = MediaOrientation::Unknown;
};

enum class VlcCallback : std::int32_t { Format = 1, Lock = 2, Display = 3, Cleanup = 4 };
enum class CallbackPhase : std::int32_t { Begin = 1, End = 2 };

std::uint64_t next_renderer_generation() noexcept;
std::uint64_t next_vlc_generation() noexcept;
void renderer_open(std::uint64_t generation) noexcept;
void renderer_frame(std::uint64_t generation, const RendererFrameStats& stats,
                    bool fmv_texture_active) noexcept;
void renderer_close(std::uint64_t generation) noexcept;
RendererDiagnostics renderer_snapshot() noexcept;

void media_sample(MediaBackend backend, std::uint64_t generation,
                  const MediaSample& sample, bool accepted) noexcept;
void vlc_callback(std::uint64_t generation, VlcCallback callback,
                  CallbackPhase phase) noexcept;
void vlc_decoder_open(std::uint64_t generation) noexcept;
void vlc_decoder_close(std::uint64_t generation, std::int32_t reason) noexcept;

// Source/fallback values are defined by host_input.hpp. Changes are traced once;
// repeated polls only update the numeric snapshot. No key names or typed text.
void input_axis(std::int32_t axis, std::int32_t source, std::int32_t fallback,
                std::int32_t profile_bindings, std::int32_t xinput_bindings) noexcept;

// Clears process-local observations, not metric/provider registrations. Used by
// hermetic tests; production renderer/decoder generations remain monotonic.
void reset_observations_for_tests() noexcept;

}  // namespace slrr::platform_telemetry
