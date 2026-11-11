#include "test_harness.hpp"
#include "support/test_fixtures.hpp"

#include "slrr/diagnostics.hpp"
#include "slrr/host_input.hpp"
#include "slrr/metrics.hpp"
#include "slrr/platform_telemetry.hpp"

#include <string>

namespace {
const slrr::metrics::Value* metric(const slrr::metrics::Snapshot& snapshot,
                                   const std::string& name) {
  for (const auto& value : snapshot.values)
    if (value.name == name) return &value;
  return nullptr;
}
}

SLRR_TEST(platform_renderer_generation_frame_views_and_reopen) {
  using namespace slrr::platform_telemetry;
  reset_observations_for_tests();
  slrr::metrics::reset();
  const auto first = next_renderer_generation();
  renderer_open(first);
  RendererFrameStats frame;
  frame.gpu_frame = 19;
  frame.cpu_frequency = frame.gpu_frequency = 1000;
  frame.draw_calls = 7;
  frame.compute_calls = 2;
  frame.blit_calls = 1;
  frame.vertex_buffers = 11;
  frame.index_buffers = 12;
  frame.textures = 13;
  frame.view_count = 18;
  frame.copied_views = 16;
  for (std::size_t i = 0; i < frame.copied_views; ++i)
    frame.views[i].view = static_cast<std::uint16_t>(i + 3);
  renderer_frame(first, frame, true);
  auto snapshot = renderer_snapshot();
  SLRR_REQUIRE(snapshot.open && snapshot.stats_valid);
  SLRR_REQUIRE(snapshot.generation == first);
  SLRR_REQUIRE(snapshot.submitted_frames == 1);
  SLRR_REQUIRE(snapshot.views_truncated);
  SLRR_REQUIRE(snapshot.frame.views[15].view == 18);
  SLRR_REQUIRE(snapshot.fmv_texture_handles == 1);

  renderer_close(first);
  snapshot = renderer_snapshot();
  SLRR_REQUIRE(!snapshot.open && !snapshot.stats_valid);
  SLRR_REQUIRE(snapshot.frame.textures == 0);
  SLRR_REQUIRE(snapshot.fmv_texture_handles == 0);
  renderer_frame(first, frame, true);
  SLRR_REQUIRE(renderer_snapshot().submitted_frames == 1);

  const auto second = next_renderer_generation();
  SLRR_REQUIRE(second > first);
  renderer_open(second);
  SLRR_REQUIRE(renderer_snapshot().generation == second);
  SLRR_REQUIRE(!renderer_snapshot().stats_valid);
  const auto values = slrr::metrics::snapshot();
  SLRR_REQUIRE(metric(values, "renderer.generation")->value == static_cast<std::int64_t>(second));
  SLRR_REQUIRE(metric(values, "renderer.frames_submitted")->kind == slrr::metrics::Kind::Counter);
  renderer_close(second);
}

SLRR_TEST(platform_media_events_are_numeric_and_counted) {
  using namespace slrr::platform_telemetry;
  reset_observations_for_tests();
  slrr::test::TemporaryDirectory temporary("platform-telemetry");
  const auto path = temporary.path() / "events.jsonl";
  std::string error;
  SLRR_REQUIRE(slrr::diagnostics::start(path.string(), error, 32));
  const auto generation = next_vlc_generation();
  vlc_decoder_open(generation);
  vlc_callback(generation, VlcCallback::Format, CallbackPhase::Begin);
  vlc_callback(generation, VlcCallback::Format, CallbackPhase::End);
  vlc_callback(generation, VlcCallback::Lock, CallbackPhase::Begin);
  vlc_callback(generation, VlcCallback::Lock, CallbackPhase::End);
  media_sample(MediaBackend::DirectShow, generation,
               {640, 480, 2560, 1228800, MediaOrientation::BottomUp}, true);
  media_sample(MediaBackend::LibVlc, generation,
               {640, 480, 2560, 1228800, MediaOrientation::TopDown}, false);
  vlc_decoder_close(generation, 3);
  slrr::diagnostics::flush();
  const auto text = slrr::test::read_text_file(path);
  SLRR_REQUIRE(text.find("media.directshow.sample") != std::string::npos);
  SLRR_REQUIRE(text.find("media.libvlc.sample") != std::string::npos);
  SLRR_REQUIRE(text.find("\"width\":640") != std::string::npos);
  SLRR_REQUIRE(text.find("\"pitch\":2560") != std::string::npos);
  SLRR_REQUIRE(text.find("\"byte_length\":1228800") != std::string::npos);
  SLRR_REQUIRE(text.find("\"orientation\":2") != std::string::npos);
  SLRR_REQUIRE(text.find("\"accepted\":0") != std::string::npos);
  SLRR_REQUIRE(text.find("media.libvlc.callback") != std::string::npos);
  SLRR_REQUIRE(text.find("media.libvlc.close") != std::string::npos);
  SLRR_REQUIRE(text.find("\"object\":null") != std::string::npos);
  const auto values = slrr::metrics::snapshot();
  SLRR_REQUIRE(metric(values, "media.directshow.samples")->value == 1);
  SLRR_REQUIRE(metric(values, "media.libvlc.rejected")->value == 1);
  SLRR_REQUIRE(metric(values, "media.libvlc.closes")->value == 1);
  SLRR_REQUIRE(metric(values, "media.libvlc.format.begin")->value == 1);
  SLRR_REQUIRE(metric(values, "media.libvlc.format.end")->value == 1);
  slrr::diagnostics::shutdown();
}

SLRR_TEST(input_partial_xinput_profile_diagnoses_fallback_without_text) {
  using namespace slrr;
  ControllerInput input;
  input.bindings.push_back({1, 0, 203, 0, 1, 0, 1});
  auto axis = input_axis_binding_diagnostics(&input, 1);
  SLRR_REQUIRE(axis.source == InputBindingSource::ProfileAndXInputFallback);
  SLRR_REQUIRE(axis.fallback == InputFallbackStatus::Available);
  axis = input_axis_binding_diagnostics(&input, 28);
  SLRR_REQUIRE(axis.source == InputBindingSource::XInputFallback);

  input.bindings.push_back({29, 2, 8, 0, 1, 0, 1});
  axis = input_axis_binding_diagnostics(&input, 28);
  SLRR_REQUIRE(axis.source == InputBindingSource::Unbound);
  SLRR_REQUIRE(axis.fallback == InputFallbackStatus::SuppressedByXInputProfile);
  SLRR_REQUIRE(axis.profile_bindings == 0);
  axis = input_axis_binding_diagnostics(&input, 29);
  SLRR_REQUIRE(axis.xinput_bindings == 1);
  input.forced_axes.emplace(29, 0.5f);
  axis = input_axis_binding_diagnostics(&input, 29);
  SLRR_REQUIRE(axis.source == InputBindingSource::Forced);
  SLRR_REQUIRE(axis.fallback == InputFallbackStatus::Overridden);
  SLRR_REQUIRE(input_axis_binding_diagnostics(nullptr, 1).source ==
               InputBindingSource::MissingController);
}
