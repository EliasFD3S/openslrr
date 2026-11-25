#include "debug_overlay.hpp"
#include "slrr/frame_profiler.hpp"

#include "slrr/host_clipboard.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/host_input.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/gameref.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/scene_presentation.hpp"
#include "slrr/section_machine.hpp"
#include "slrr/tire_model.hpp"
#include "render_debug.hpp"

#include <imgui/imgui.h>

#include <cfloat>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace slrr {
namespace {
constexpr bgfx::ViewId kDebugView = 71;
constexpr double kDegrees = 57.29577951308232;

void draw_render_session(Jvm* vm) {
  if (const auto* stats = bgfx::getStats()) {
    const auto ms = [](int64_t ticks, int64_t frequency) {
      return frequency > 0 ? ticks * 1000. / frequency : 0.;
    };
    const auto frame = ms(stats->cpuTimeFrame, stats->cpuTimerFreq);
    ImGui::Text("%.1f FPS | %.2f ms/frame", frame > 0 ? 1000. / frame : 0., frame);
    ImGui::Text("CPU render %.2f ms | GPU %.2f ms",
                ms(stats->cpuTimeEnd - stats->cpuTimeBegin, stats->cpuTimerFreq),
                ms(stats->gpuTimeEnd - stats->gpuTimeBegin, stats->gpuTimerFreq));
    ImGui::Text("%u draws | %u textures | %u views", stats->numDraw,
                stats->numTextures, stats->numViews);
  }
  const auto timing = perf::snapshot();
  if (timing.ready) {
    auto ms = [&](perf::Zone z) {
      return timing.zones[static_cast<size_t>(z)];
    };
    ImGui::Text("CPU scripts %.2f | traffic %.2f | physics %.2f ms",
                ms(perf::Zone::Scripts), ms(perf::Zone::Traffic),
                ms(perf::Zone::Physics));
    ImGui::Text("Physics thread %.2f | pedestrians %.2f | collision %.2f ms",
                ms(perf::Zone::Vehicles), ms(perf::Zone::Pedestrians),
                ms(perf::Zone::Collision));
    ImGui::Text("CPU collect %.2f | shadows %.2f | reflections %.2f ms",
                ms(perf::Zone::Collect), ms(perf::Zone::Shadows),
                ms(perf::Zone::Reflections));
    ImGui::Text("CPU flush %.2f | present wait %.2f | peak frame %.2f ms",
                ms(perf::Zone::Flush), ms(perf::Zone::Present),
                timing.worst_ms);
    const double measured = ms(perf::Zone::Scripts) + ms(perf::Zone::Traffic) +
                            ms(perf::Zone::Physics) + ms(perf::Zone::Flush) +
                            ms(perf::Zone::Gc);
    ImGui::Text("CPU GC %.2f | other host/wait %.2f ms", ms(perf::Zone::Gc),
                std::max(0., timing.frame_ms - measured));
  }
  ImGui::Text("Collision mesh rebuilds: %llu",
              static_cast<unsigned long long>(scene_instances_floor_builds()));
  const auto audio = host_sfx_stats();
  ImGui::Text("SFX voices: %u | Coalesced: %llu", audio.active,
              static_cast<unsigned long long>(audio.coalesced));
  if (vm) {
    float hour = std::fmod(scene_sun_time(), 86400.f) / 3600.f;
    const int minutes = static_cast<int>(hour * 60.f);
    ImGui::Text("Game time %02d:%02d", minutes / 60, minutes % 60);
    if (ImGui::SliderFloat("Time of day", &hour, 0.f, 23.999f, "%.2f h")) {
      vm->invoke("java.game.GameLogic", "setTime", "(F)V",
                 {Value::make_float(hour * 3600.f)}, true);
      scene_sun_set_time(hour * 3600.f);
    }
    const auto time_factor = vm->get_static("java.game.GameLogic", "timeFactor");
    float rate = time_factor.tag == ValueTag::Float ? time_factor.v.f : 4.f;
    if (ImGui::SliderFloat("Clock speed", &rate, 0.f, 240.f, "%.1fx"))
      vm->set_static("java.game.GameLogic", "timeFactor", Value::make_float(rate));
  }
}

// Rolling window for the renderer timings (#243): 120 frames.
struct Rolling {
  std::array<float, 120> values{};
  int count = 0, head = 0;
  void add(float value) {
    values[size_t(head)] = value;
    head = (head + 1) % int(values.size());
    count = std::min(count + 1, int(values.size()));
  }
  float average() const {
    float sum = 0.f;
    for (int i = 0; i < count; ++i) sum += values[size_t(i)];
    return count ? sum / float(count) : 0.f;
  }
  float minimum() const {
    float out = count ? values[0] : 0.f;
    for (int i = 1; i < count; ++i) out = std::min(out, values[size_t(i)]);
    return out;
  }
  float maximum() const {
    float out = count ? values[0] : 0.f;
    for (int i = 1; i < count; ++i) out = std::max(out, values[size_t(i)]);
    return out;
  }
};

struct RenderTimings {
  bool frozen = false;
  Rolling frame, gpu, render_thread, draws, triangles;
  struct View {
    std::string name;
    Rolling gpu, cpu;
    uint32_t last_frame = 0;
  };
  std::map<uint16_t, View> views;
  uint32_t frames = 0;
  int64_t texture_memory = -1, target_memory = -1;
  uint32_t textures = 0, frame_buffers = 0;
};

RenderTimings& render_timings() {
  static RenderTimings timings;
  return timings;
}

void sample_render_timings() {
  auto& t = render_timings();
  const auto* stats = bgfx::getStats();
  if (!stats || t.frozen) return;
  const auto ms = [](int64_t ticks, int64_t frequency) {
    return frequency > 0 ? float(double(ticks) * 1000. / double(frequency)) : 0.f;
  };
  ++t.frames;
  t.frame.add(ms(stats->cpuTimeFrame, stats->cpuTimerFreq));
  t.gpu.add(ms(stats->gpuTimeEnd - stats->gpuTimeBegin, stats->gpuTimerFreq));
  t.render_thread.add(ms(stats->cpuTimeEnd - stats->cpuTimeBegin, stats->cpuTimerFreq));
  t.draws.add(float(stats->numDraw));
  uint64_t prims = 0;
  for (const auto count : stats->numPrims) prims += count;
  t.triangles.add(float(prims));
  t.texture_memory = stats->textureMemoryUsed;
  t.target_memory = stats->rtMemoryUsed;
  t.textures = stats->numTextures;
  t.frame_buffers = stats->numFrameBuffers;
  for (uint16_t i = 0; i < stats->numViews; ++i) {
    const auto& view = stats->viewStats[i];
    auto& entry = t.views[view.view];
    entry.name = view.name[0] ? view.name : render_debug_view_name(view.view);
    entry.gpu.add(ms(view.gpuTimeEnd - view.gpuTimeBegin, stats->gpuTimerFreq));
    entry.cpu.add(ms(view.cpuTimeEnd - view.cpuTimeBegin, stats->cpuTimerFreq));
    entry.last_frame = t.frames;
  }
  // A view not submitted this frame (pass switched off) reads as 0.
  for (auto& [id, entry] : t.views)
    if (entry.last_frame != t.frames) {
      entry.gpu.add(0.f);
      entry.cpu.add(0.f);
    }
}

constexpr std::pair<perf::Zone, const char*> kRenderZones[] = {
    {perf::Zone::Collect, "Scene collection + residency"},
    {perf::Zone::CollectBatches, "  instanced prop batches"},
    // #264: the world list is built by the first collection of the frame
    // (the batches above, in the main view); every collection copies it.
    {perf::Zone::WorldList, "    world list build"},
    {perf::Zone::CollectInstances, "  scene instances (main view)"},
    {perf::Zone::WorldCopy, "    world list copies (all collections)"},
    {perf::Zone::CollectSkins, "    walker palettes (all collections)"},
    {perf::Zone::CollectPreviews, "  viewport previews"},
    {perf::Zone::Residency, "  GPU residency"},
    // #270: AO prepass, car lamp tiles and reflection faces on worker
    // encoders, beside the world submission.
    {perf::Zone::WorkerSubmit, "Worker submission (sum over workers)"},
    {perf::Zone::SubmitWait, "  host wait for workers"},
    {perf::Zone::LampSelect, "Street lamp selection"},
    {perf::Zone::Shadows, "Shadow updates (all)"},
    {perf::Zone::SunShadows, "  sun cascades / point"},
    {perf::Zone::SunCollect, "    sun caster collection"},
    {perf::Zone::CarDepth, "  vehicle depth (sun)"},
    {perf::Zone::LampShadows, "  lamp + car lamp tiles"},
    {perf::Zone::FixtureShadows, "Garage fixture shadows"},
    {perf::Zone::AmbientOcclusion, "Ambient occlusion"},
    {perf::Zone::Reflections, "Reflections"},
    {perf::Zone::World, "World submission"},
    {perf::Zone::Osd, "OSD + debug submission"},
    {perf::Zone::Present, "Present wait"},
    // Main-thread host work outside the renderer (#260).
    {perf::Zone::Scripts, "Scripts"},
    {perf::Zone::Traffic, "Traffic"},
    {perf::Zone::Physics, "Physics on the host (prepare, wait, publish)"},
    {perf::Zone::Pedestrians, "  pedestrians"},
    {perf::Zone::Collision, "  collision"},
    {perf::Zone::Vehicles, "Physics thread (beside the host)"},
    {perf::Zone::Gc, "GC"},
    {perf::Zone::Flush, "Render flush (all renderer zones)"},
};
// Frame time not covered by scripts, traffic, physics, flush and GC.
double other_main_thread(const perf::Snapshot& timing) {
  const auto ms = [&](perf::Zone z) { return timing.zones[static_cast<size_t>(z)]; };
  return std::max(0., timing.frame_ms - ms(perf::Zone::Scripts) - ms(perf::Zone::Traffic) -
                          ms(perf::Zone::Physics) - ms(perf::Zone::Flush) - ms(perf::Zone::Gc));
}

// Session state the snapshot header reports (set by draw_render).
struct DumpContext {
  const DebugOverlay::RenderOptions* options = nullptr;
  uint16_t width = 0, height = 0;
};
DumpContext& dump_context() {
  static DumpContext context;
  return context;
}

std::string render_timing_dump() {
  auto& t = render_timings();
  const auto& s = render_debug_stats();
  std::string out;
  char line[512];
  // Header: what was overridden, so a snapshot can be compared later.
  {
    const auto& context = dump_context();
    const DebugOverlay::RenderOptions defaults;
    const auto* options = context.options ? context.options : &defaults;
    std::snprintf(line, sizeof(line),
                  "resolution %u x %u | VSync %s | frame cap none (VSync only)\noverrides:\n",
                  unsigned(context.width), unsigned(context.height), options->vsync ? "on" : "off");
    out += line;
    {
      // The saved Enhanced options (#270), so two snapshots compare settings too.
      const auto& g = host_enhanced_graphics();
      static const char* const ao_names[] = {"off", "HBAO", "GTAO"};
      static const char* const tiers[] = {"low", "medium", "high"};
      const auto tier = [](int v) { return tiers[std::clamp(v, 0, 2)]; };
      std::snprintf(line, sizeof(line),
                    "enhanced: MSAA %dx | AO %s %s, radius %.1f m, strength %d %% | "
                    "shadows %s, smoothing %d | dynamic lighting %s, local lights %s, "
                    "reflections %s, clouds %s, anisotropic %s\n",
                    g.msaa <= 0 ? 1 : 1 << std::clamp(g.msaa, 0, 3),
                    ao_names[std::clamp(g.ao_method, 0, 2)], tier(g.ao_quality), g.ao_radius * .01f,
                    g.ao_strength, tier(g.shadow_quality), g.shadow_smoothing,
                    g.dynamic_lighting ? "on" : "off", g.local_lights ? "on" : "off",
                    g.dynamic_reflections ? "on" : "off", g.volumetric_clouds ? "on" : "off",
                    g.anisotropic_filtering ? "on" : "off");
      out = line + out;
    }
    std::string changed = render_debug_overrides_text(render_debug());
    const auto flag = [&](const char* name, bool value, bool fallback) {
      if (value != fallback) changed += std::string("  ") + name + " = " + (value ? "on" : "off") + "\n";
    };
    flag("map shadows", options->map_shadows, defaults.map_shadows);
    flag("vehicle casters", options->shadows, defaults.shadows);
    flag("game OSD", options->game_osd, defaults.game_osd);
    flag("sun flare", options->sun_flare, defaults.sun_flare);
    if (options->reflection_faces != defaults.reflection_faces) {
      std::snprintf(line, sizeof(line), "  reflection faces per frame = %d\n", options->reflection_faces);
      changed += line;
    }
    out += changed.empty() ? "  none\n" : changed;
  }
  std::snprintf(line, sizeof(line),
                "frame %.2f ms (min %.2f max %.2f) | GPU %.2f ms | render thread %.2f ms\n"
                "draws %.0f | primitives %.0f | instances %u | batches %u (%u placements)\n"
                "lamps %u registered, %u lit, %u shadowed, %u car tiles\n",
                t.frame.average(), t.frame.minimum(), t.frame.maximum(), t.gpu.average(),
                t.render_thread.average(), t.draws.average(), t.triangles.average(), s.instances,
                s.batches, s.batch_instances, s.lamps_registered, s.lamps_lit, s.lamps_shadowed,
                s.car_tiles);
  out += line;
  {
    // The sun the renderer lights with and the street lamp factor (#271).
    const auto sun = scene_instances_sun_state();
    std::snprintf(line, sizeof(line),
                  "sun elevation %.1f deg | diffuse %.3f (noon %.3f, direct %.2f, shadows %s) | lamp night factor %.2f\n",
                  sun.elevation * 57.2958f, sun.diffuse, sun.noon, sun.direct,
                  sun.shadowing ? "on" : "off", sun.night);
  }
  out += line;
  out += "view | name | GPU avg/min/max ms | CPU avg ms | draws\n";
  for (const auto& [id, view] : t.views) {
    std::snprintf(line, sizeof(line), "%3u | %s | %.3f / %.3f / %.3f | %.3f | %u\n", unsigned(id),
                  view.name.c_str(), view.gpu.average(), view.gpu.minimum(), view.gpu.maximum(),
                  view.cpu.average(), id < s.view_draws.size() ? s.view_draws[id] : 0u);
    out += line;
  }
  const auto timing = perf::snapshot();
  if (timing.ready) {
    for (const auto& [zone, name] : kRenderZones) {
      std::snprintf(line, sizeof(line), "CPU %s: %.3f ms\n", name,
                    timing.zones[static_cast<size_t>(zone)]);
      out += line;
    }
    std::snprintf(line, sizeof(line), "CPU other main thread: %.3f ms (frame %.3f ms)\n",
                  other_main_thread(timing), timing.frame_ms);
    out += line;
  }
  return out;
}

// Writes the snapshot next to the executable, in perf/ (#243). Returns the
// path, or an error text.
std::string write_render_dump(const std::string& text) {
  wchar_t module[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
  std::error_code error;
  auto directory = length ? std::filesystem::path(module).parent_path() / "perf"
                          : std::filesystem::current_path(error) / "perf";
  std::filesystem::create_directories(directory, error);
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  char name[64];
  std::strftime(name, sizeof(name), "renderer_debug_%Y%m%d_%H%M%S.txt", &local);
  const auto path = directory / name;
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) return "cannot write " + path.string();
  out << text << "\n";
  return out ? path.string() : "cannot write " + path.string();
}

void draw_render_timings() {
  auto& t = render_timings();
  static std::string status;
  ImGui::Checkbox("Freeze", &t.frozen);
  ImGui::SameLine();
  if (ImGui::Button("Copy to clipboard"))
    status = host_clipboard_set_text(render_timing_dump()) ? "Copied to the clipboard."
                                                           : "Clipboard unavailable.";
  ImGui::SameLine();
  if (ImGui::Button("Log")) {
    const auto text = render_timing_dump();
    std::printf("[render-debug]\n%s", text.c_str());
    std::fflush(stdout);
    status = "Written to " + write_render_dump(text);
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear")) t = {};
  if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
  ImGui::Text("Frame %.2f ms (min %.2f, max %.2f) | GPU %.2f ms | render thread %.2f ms",
              t.frame.average(), t.frame.minimum(), t.frame.maximum(), t.gpu.average(),
              t.render_thread.average());
  const auto& s = render_debug_stats();
  ImGui::Text("Draws %.0f | primitives %.0f | instances %u | batches %u (%u placements)",
              t.draws.average(), t.triangles.average(), s.instances, s.batches,
              s.batch_instances);
  ImGui::Text("Textures %u (%.1f MB) | render targets %u (%.1f MB)", t.textures,
              t.texture_memory >= 0 ? t.texture_memory / 1048576. : 0., t.frame_buffers,
              t.target_memory >= 0 ? t.target_memory / 1048576. : 0.);
  ImGui::Text("Lamps %u registered, %u lit, %u shadowed, %u car tiles | tile redraws %llu / %llu",
              s.lamps_registered, s.lamps_lit, s.lamps_shadowed, s.car_tiles,
              static_cast<unsigned long long>(s.lamp_tile_redraws),
              static_cast<unsigned long long>(s.car_tile_redraws));
  if (ImGui::BeginTable("render-views", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                               ImGuiTableFlags_Sortable |
                                               ImGuiTableFlags_SizingStretchProp)) {
    for (const char* name : {"View", "Pass", "GPU avg ms", "GPU min / max", "CPU ms", "Draws"})
      ImGui::TableSetupColumn(name);
    ImGui::TableHeadersRow();
    std::vector<std::pair<uint16_t, const RenderTimings::View*>> rows;
    for (const auto& [id, view] : t.views) rows.push_back({id, &view});
    // Most expensive first.
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      return a.second->gpu.average() > b.second->gpu.average();
    });
    for (const auto& [id, view] : rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%u", unsigned(id));
      ImGui::TableNextColumn(); ImGui::TextUnformatted(view->name.c_str());
      ImGui::TableNextColumn(); ImGui::Text("%.3f", view->gpu.average());
      ImGui::TableNextColumn(); ImGui::Text("%.3f / %.3f", view->gpu.minimum(), view->gpu.maximum());
      ImGui::TableNextColumn(); ImGui::Text("%.3f", view->cpu.average());
      ImGui::TableNextColumn(); ImGui::Text("%u", id < s.view_draws.size() ? s.view_draws[id] : 0u);
    }
    ImGui::EndTable();
  }
  const auto timing = perf::snapshot();
  if (timing.ready) {
    ImGui::SeparatorText("CPU frame phases (1 s average)");
    for (const auto& [zone, name] : kRenderZones)
      ImGui::Text("%-32s %.3f ms", name, timing.zones[static_cast<size_t>(zone)]);
    ImGui::Text("%-32s %.3f ms", "Other main thread", other_main_thread(timing));
  }
}

void draw_render_passes(DebugOverlay::RenderOptions& options) {
  auto& o = render_debug();
  ImGui::TextDisabled("Debug overrides: not saved, reset on restart.");
  ImGui::Checkbox("Ambient occlusion pass", &o.ambient_occlusion);
  for (int i = 0; i < 4; ++i) {
    char label[48];
    std::snprintf(label, sizeof(label), "Sun cascade %d", i);
    if (i) ImGui::SameLine();
    ImGui::Checkbox(label, &o.sun_cascades[size_t(i)]);
  }
  ImGui::Checkbox("Shadow caching (off: redraw every frame)", &o.shadow_caching);
  ImGui::Checkbox("Far cascades on alternate frames while moving", &o.cascade_alternation);
  ImGui::Checkbox("Reuse the sun's map props across frames", &o.sun_batch_cache);
  ImGui::Checkbox("Worker encoders (AO prepass, car tiles, sun cascades, reflections)", &o.worker_encoders);
  ImGui::Checkbox("Headlight shadows", &o.headlight_shadows);
  ImGui::Checkbox("Map shadows", &options.map_shadows);
  ImGui::Checkbox("Vehicle shadow casters", &options.shadows);
  ImGui::Checkbox("Character shadow casters", &o.skinned_casters);
  ImGui::Checkbox("Garage fixture shadows", &o.fixture_shadows);
  ImGui::Checkbox("Street lamp lighting", &o.lamp_lighting);
  ImGui::Checkbox("Car lights (headlight beams, brake lights)", &o.car_lights);
  ImGui::Checkbox("Street lamp shadows", &o.lamp_shadows);
  ImGui::Checkbox("Car lamp shadow tiles", &o.car_lamp_tiles);
  ImGui::Checkbox("Lamp shadow reference path (slow)", &o.lamp_shadow_reference);
  ImGui::Checkbox("Reflections", &o.reflections);
  ImGui::Checkbox("Sky", &o.sky);
  ImGui::SameLine();
  ImGui::Checkbox("Clouds", &o.clouds);
  ImGui::SameLine();
  ImGui::Checkbox("Fog", &o.fog);
  ImGui::Checkbox("MSAA", &o.msaa);
  ImGui::SameLine();
  ImGui::Checkbox("Anisotropic", &o.anisotropic);
  ImGui::Checkbox("Map props / foliage", &o.map_props);
  ImGui::SameLine();
  ImGui::Checkbox("Vehicles", &o.vehicles);
  ImGui::SameLine();
  ImGui::Checkbox("Characters", &o.characters);
  ImGui::Checkbox("Game OSD", &options.game_osd);
  ImGui::SameLine();
  ImGui::Checkbox("Sun flare", &options.sun_flare);
}

void draw_render_budgets(DebugOverlay::RenderOptions& options) {
  auto& o = render_debug();
  const auto budget_slider = [](const char* label, int& value, int fallback, int maximum) {
    int shown = value < 0 ? fallback : value;
    if (ImGui::SliderInt(label, &shown, 0, maximum)) value = shown;
  };
  budget_slider("Lit street lamps", o.lamp_budget, 40, 40);
  float distance = o.lamp_distance > 0.f ? o.lamp_distance : 500.f;
  if (ImGui::SliderFloat("Lamp distance (m)", &distance, 50.f, 2000.f, "%.0f"))
    o.lamp_distance = distance;
  budget_slider("Shadowed lamps", o.shadowed_lamps, 6, 6);
  budget_slider("Car lamp tiles", o.car_tiles, 16, 16);
  budget_slider("Headlight shadow tiles", o.headlight_tiles, 6, 6);
  // The combos show sizes; the overrides store the size (0 = from quality).
  const auto size_combo = [](const char* label, int& size, int base, const char* items) {
    int index = 0;
    for (int i = 1; i <= 4; ++i)
      if (size == base << (i - 1)) index = i;
    if (ImGui::Combo(label, &index, items)) size = index <= 0 ? 0 : base << (index - 1);
  };
  size_combo("Local shadow tile", o.local_shadow_tile, 128,
             "From Shadow quality\0" "128\0" "256\0" "512\0" "1024\0");
  size_combo("Sun cascade tile", o.sun_shadow_size, 512,
             "From Shadow quality\0" "512\0" "1024\0" "2048\0" "4096\0");
  ImGui::SliderFloat("Cascade radius scale", &o.cascade_scale, .25f, 4.f, "%.2fx");
  ImGui::SliderFloat("Shadow sun step (0: every frame)", &o.shadow_sun_step, 0.f, .5f, "%.3f deg");
  ImGui::SliderFloat("Shadow crossfade after a step (0: jump)", &o.shadow_crossfade, 0.f, 3.f, "%.2f s");
  ImGui::SliderFloat("AO resolution scale (2 = full res)", &o.ao_resolution_scale, .25f, 2.f, "%.2fx");
  ImGui::Checkbox("AO reference sampling (full-res depth)", &o.ao_reference);
  ImGui::SliderInt("Reflection faces per frame (0 freezes)", &options.reflection_faces, 0, 6);
  ImGui::SliderFloat("Prop LOD bias", &o.lod_bias, .25f, 4.f, "%.2fx");
  ImGui::Checkbox("Force highest prop LOD", &o.force_highest_lod);
  ImGui::SliderFloat("Far size cull (px, beyond 40 m)", &o.far_cull_px, 0.f, 16.f, "%.1f");
  if (ImGui::Button("Reset debug overrides")) o = {};
}

void draw_render_views() {
  const auto& s = render_debug_stats();
  const float width = std::max(64.f, ImGui::GetContentRegionAvail().x);
  if (bgfx::isValid(s.fixture_atlas_texture) && s.fixture_atlas) {
    ImGui::Text("Fixture / lamp atlas (tile %u px; row 3 = car lamp tiles)", s.fixture_tile);
    ImGui::Image(s.fixture_atlas_texture, ImVec2(width, width * .75f));
  } else {
    ImGui::TextDisabled("Fixture / lamp atlas inactive.");
  }
  if (bgfx::isValid(s.sun_atlas_texture) && s.sun_cascades) {
    ImGui::Text("Sun cascade atlas (%u px)", s.sun_atlas_size);
    ImGui::Image(s.sun_atlas_texture, ImVec2(width * .5f, width * .5f));
  } else {
    ImGui::TextDisabled("Sun cascades inactive.");
  }
  if (bgfx::isValid(s.ao_texture) && s.ao) {
    ImGui::Text("AO buffer (%u x %u)", s.ao_width, s.ao_height);
    const float aspect = s.ao_width ? float(s.ao_height) / float(s.ao_width) : .5625f;
    ImGui::Image(s.ao_texture, ImVec2(width, width * aspect));
  } else {
    ImGui::TextDisabled("AO inactive.");
  }
}

void draw_render_options(DebugOverlay::RenderOptions& options, const char* reflection_status) {
  auto& enhanced = host_enhanced_graphics();
  const auto previous_enhanced = enhanced;
  ImGui::Checkbox("VSync", &options.vsync);
  ImGui::Checkbox("Anisotropic filtering (hardware max)", &enhanced.anisotropic_filtering);
  ImGui::Checkbox("Map sunlight", &enhanced.dynamic_lighting);
  ImGui::Checkbox("Local lights", &enhanced.local_lights);
  ImGui::Combo("Ambient occlusion",&enhanced.ao_method,"Off\0HBAO\0GTAO\0");
  ImGui::Combo("AO quality",&enhanced.ao_quality,"Low\0Medium\0High\0");
  ImGui::SliderInt("AO strength (%)",&enhanced.ao_strength,0,200);
  ImGui::SliderInt("AO radius (cm)",&enhanced.ao_radius,25,300);
  ImGui::Combo("Shadow quality",&enhanced.shadow_quality,"Low\0Medium\0High\0");
  ImGui::Combo("Shadow smoothing",&enhanced.shadow_smoothing,"Sharp\0Soft\0Very soft\0");
  ImGui::Checkbox("Dynamic reflections", &enhanced.dynamic_reflections);
  ImGui::Text("Reflection capture: %s", reflection_status);
  ImGui::Checkbox("Volumetric clouds", &enhanced.volumetric_clouds);
  ImGui::Combo("Anti-aliasing", &enhanced.msaa, "Off\0MSAA 2x\0MSAA 4x\0MSAA 8x\0");
  if (ImGui::Button("Reset render options")) {
    options = {};
    enhanced = {};
  }
  if (enhanced != previous_enhanced) host_save_graphics_options();
}

void draw_render(uint16_t width, uint16_t height, DebugOverlay::RenderOptions& options,
                 Jvm* vm, const char* reflection_status) {
  sample_render_timings();
  dump_context() = {&options, width, height};
  ImGui::Text("%s | %u x %u", bgfx::getRendererName(bgfx::getRendererType()), width, height);
  // Each section has its own ID scope: some labels repeat across sections.
  const auto section = [](const char* title, ImGuiTreeNodeFlags flags, const auto& body) {
    if (!ImGui::CollapsingHeader(title, flags)) return;
    ImGui::PushID(title);
    body();
    ImGui::PopID();
  };
  section("Timings", ImGuiTreeNodeFlags_DefaultOpen, [&] { draw_render_timings(); });
  section("Passes (debug overrides)", 0, [&] { draw_render_passes(options); });
  section("Budgets and quality (debug overrides)", 0, [&] { draw_render_budgets(options); });
  section("Buffers", 0, [&] { draw_render_views(); });
  section("Session and host", 0, [&] { draw_render_session(vm); });
  section("Options (saved)", 0, [&] { draw_render_options(options, reflection_status); });
}

void draw_tuning(Jvm* vm) {
  if (vm) {
    ImGui::SeparatorText("Driving");
    float help = host_game_options().driving.steering_help * 100.f;
    if (ImGui::SliderFloat("Steering help", &help, 0.f, 100.f, "%.0f%%",
                           ImGuiSliderFlags_NoInput))
      host_game_options_set_steering_help(*vm, help / 100.f);
    ImGui::SetItemTooltip("Saved in Game Options. Slows keyboard steering at speed "
                          "and helps countersteer during a slide.");
    ImGui::SeparatorText("Session");
    ImGui::Text("Simulation time: %.2f s", vm->sim_time());
    float warp = vm->time_warp(-1.f);
    bool changed = ImGui::SliderFloat("Time scale", &warp, 0.f, 4.f, "%.2fx",
                                      ImGuiSliderFlags_NoInput);
    if (ImGui::Button("Normal speed")) { warp = 1.f; changed = true; }
    if (changed)
      vm->natives().invoke("java.lang.System", "timeWarp", "(F)F", nullptr,
                           {Value::make_float(warp)});
  }
  ImGui::SeparatorText("Tyre model");
  // The physics thread reads the tuning during a batch: edit a copy and
  // apply it between batches.
  auto tuning = physics::tire_tuning();
  bool changed = false;
  const auto slider = [&changed](const char* label, float& value, float minimum, float maximum) {
    changed |= ImGui::SliderFloat(label, &value, minimum, maximum, "%.2fx",
                                  ImGuiSliderFlags_NoInput);
  };
  slider("Lateral grip", tuning.lateral_grip, .25f, 2.f);
  slider("Longitudinal grip", tuning.longitudinal_grip, .25f, 2.f);
  slider("Lateral response", tuning.lateral_response, .25f, 2.f);
  slider("Load sensitivity", tuning.load_sensitivity, 0.f, 2.5f);
  if (ImGui::Button("Reset tyre settings")) {
    tuning = {};
    changed = true;
  }
  if (changed) {
    part_physics_join();
    physics::tire_tuning() = tuning;
  }
}

void draw_section(Jvm* vm, const SectionMachine* sections, bool& edit_fields) {
  if (!vm || !sections) {
    ImGui::TextUnformatted("No script VM attached.");
    return;
  }
  auto* section = sections->actual();
  ImGui::Text("Classes %d | VM heap %zu objects | Last GC freed %zu",
              vm->classes_loaded(), vm->heap_size(), vm->gc_last_freed());
  if (ImGui::Button("Request garbage collection") && vm->request_gc() >= 0)
    vm->request_gc();
  if (!vm->unbound_native().empty())
    ImGui::TextWrapped("Unbound native: %s", vm->unbound_native().c_str());
  if (!section) return;
  ImGui::Checkbox("Edit numeric section fields", &edit_fields);
  if (!ImGui::BeginTable("section-fields", 2, ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) return;
  ImGui::TableSetupColumn("Script field");
  ImGui::TableSetupColumn("Value");
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();
  std::vector<std::string_view> names;
  for (const auto& [name, value] : section->fields)
    if (!name.empty() && name.front() != '_') names.push_back(name);
  std::sort(names.begin(), names.end());
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(names.size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const auto name = names[row];
      auto& value = section->fields.at(std::string(name));
      ImGui::PushID(name.data());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(name.data(), name.data() + name.size());
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1.f);
      switch (value.tag) {
        case ValueTag::Float:
          if (edit_fields && !scene_transition_active()) {
            float edited = value.v.f;
            if (ImGui::DragFloat("##value", &edited, .05f, 0, 0, "%.4g", ImGuiSliderFlags_NoInput) &&
                std::isfinite(edited))
              value = Value::make_float(edited);
          } else ImGui::Text("%.6g", value.v.f);
          break;
        case ValueTag::Int:
          if (edit_fields && !scene_transition_active())
            ImGui::DragScalar("##value", ImGuiDataType_S32, &value.v.i, .2f,
                              nullptr, nullptr, nullptr, ImGuiSliderFlags_NoInput);
          else ImGui::Text("%d", value.v.i);
          break;
        case ValueTag::Obj:
          ImGui::TextUnformatted(value.v.o ? value.v.o->class_name.c_str() : "null");
          break;
        default: ImGui::TextUnformatted("-"); break;
      }
      ImGui::PopID();
    }
  }
  ImGui::EndTable();
}

void draw_osd_inspector() {
  ImGui::Text("Loading %s | Modal %s | Hover index %d",
              osd_loading_dialog() ? "yes" : "no", osd_has_modal() ? "yes" : "no",
              osd_hover_index());
  const auto& widgets = osd_widgets();
  std::vector<size_t> visible;
  for (size_t i = 0; i < widgets.size(); ++i)
    if (!widgets[i].hidden && !widgets[i].dead) visible.push_back(i);
  ImGui::Text("%zu visible widgets / %zu registered", visible.size(), widgets.size());
  if (!ImGui::BeginTable("osd-widgets", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) return;
  for (const char* name : {"Index", "Command", "Group / layer", "Label"})
    ImGui::TableSetupColumn(name);
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(visible.size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const size_t i = visible[row];
      const auto& widget = widgets[i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%zu", i);
      ImGui::TableNextColumn(); ImGui::Text("%d%s", widget.cmd, widget.disabled ? " (off)" : "");
      ImGui::TableNextColumn(); ImGui::Text("%d / %d", widget.group, widget.layer);
      ImGui::TableNextColumn();
      const auto length = widget.tip.find_first_of("\r\n");
      ImGui::TextUnformatted(widget.tip.data(),
          widget.tip.data() + std::min(length, widget.tip.size()));
    }
  }
  ImGui::EndTable();
}

int vehicle_id(Object* vehicle) {
  const auto* node = gameref_get(vehicle);
  return node ? node->live_id : 0;
}

std::string vehicle_label(Object* vehicle) {
  const auto* node = gameref_get(vehicle);
  const std::string& name = node && !node->alias.empty() ? node->alias : vehicle->class_name;
  return name + " #" + std::to_string(vehicle_id(vehicle));
}

// Rolling telemetry history for the Vehicle tab (#380), one sample per frame.
struct TelemetryHistory {
  static constexpr int kSize = 600;
  float speed[kSize]{}, rpm[kSize]{}, yaw[kSize]{}, slip_angle[kSize]{}, rear_slip[kSize]{};
  int next = 0;
  void push(const PartPhysicsState& state) {
    speed[next] = static_cast<float>(state.speed_kph);
    rpm[next] = static_cast<float>(state.rpm);
    yaw[next] = static_cast<float>(state.yaw_rate * kDegrees);
    const auto& rl = state.tires[2];
    const auto& rr = state.tires[3];
    const double road = std::max(1.5, std::abs((rl.wheel_speed - rl.slip_speed +
                                                rr.wheel_speed - rr.slip_speed) * .5));
    slip_angle[next] = static_cast<float>(
        std::atan2((rl.lateral_speed + rr.lateral_speed) * .5, road) * kDegrees);
    rear_slip[next] = static_cast<float>((rl.slip_speed + rr.slip_speed) * .5);
    next = (next + 1) % kSize;
  }
};

void plot(const char* label, const float* values, int offset, const char* unit) {
  char overlay[64];
  std::snprintf(overlay, sizeof overlay, "%.1f %s",
                values[(offset + TelemetryHistory::kSize - 1) % TelemetryHistory::kSize], unit);
  ImGui::PlotLines(label, values, TelemetryHistory::kSize, offset, overlay, FLT_MAX, FLT_MAX,
                   ImVec2(0, 48));
}

void draw_vehicle(const PartPhysicsState& state) {
  static TelemetryHistory history;
  history.push(state);
  const auto clock = part_physics_clock_stats();
  ImGui::Text("Solver %u Hz | %s | %.0f steps/s | thread %.1f %%", clock.hz,
              clock.free_running ? "free-running" : "frame-locked", clock.steps_per_second,
              clock.thread_load * 100.);
  if (clock.dropped > 0 || clock.host_wait > .01)
    ImGui::TextColored(ImVec4(1, .55f, .2f, 1),
                       "Behind real time: frame waits %.0f %% | %.0f ms/s dropped",
                       clock.host_wait * 100., clock.dropped * 1e3);
  ImGui::Text("Throttle %.2f | Brake %.2f | Clutch %.2f", state.throttle, state.brake,
              state.clutch);
  ImGui::Text("%.1f km/h | %.0f rpm | Gear %d", state.speed_kph, state.rpm, state.gear);
  ImGui::Text("Physics %s | Mass %.0f kg | Yaw %.1f deg/s",
              state.active ? "active" : "inactive", state.mass, state.yaw_rate * kDegrees);
  ImGui::Text("Steering %.0f%% help | %+.1f deg requested | %+.1f deg applied | %+.1f deg assist",
              state.steering_help * 100., state.steering_request * kDegrees,
              state.steering_angle * kDegrees, state.steering_correction * kDegrees);
  ImGui::Text("Ticks %llu | Backlog %llu",
              static_cast<unsigned long long>(state.ticks),
              static_cast<unsigned long long>(state.backlog));
  plot("Speed", history.speed, history.next, "km/h");
  plot("RPM", history.rpm, history.next, "rpm");
  plot("Yaw rate", history.yaw, history.next, "deg/s");
  plot("Rear slip angle", history.slip_angle, history.next, "deg");
  plot("Rear wheelspin", history.rear_slip, history.next, "m/s");
  if (!ImGui::BeginTable("wheels", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    return;
  for (const char* name : {"Wheel", "Load / steer", "Wheel speed", "Slip ratio / angle",
                           "Slip X / Y", "Force X / Y"})
    ImGui::TableSetupColumn(name);
  ImGui::TableHeadersRow();
  const char* names[] = {"FL", "FR", "RL", "RR"};
  for (size_t i = 0; i < state.tires.size(); ++i) {
    const auto& tire = state.tires[i];
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("%s %s", names[i], state.contact[i] ? "ground" : "air");
    ImGui::TableNextColumn();
    ImGui::Text("%.0f N / %+.1f deg", state.force[i], tire.steer * kDegrees);
    ImGui::TableNextColumn();
    ImGui::Text("%.2f m/s", tire.wheel_speed);
    ImGui::TableNextColumn();
    {
      const double road = tire.wheel_speed - tire.slip_speed;
      const double reference = std::max(1.5, std::abs(road));
      ImGui::Text("%+.2f / %+.1f deg", tire.slip_speed / reference,
                  std::atan2(tire.lateral_speed, reference) * kDegrees);
    }
    ImGui::TableNextColumn();
    ImGui::Text("%+.2f / %+.2f m/s", tire.slip_speed, tire.lateral_speed);
    ImGui::TableNextColumn();
    ImGui::Text("%+.0f / %+.0f N", tire.longitudinal_force, tire.lateral_force);
  }
  ImGui::EndTable();
}

void draw_input() {
  const auto& input = host_input();
  constexpr const char* kinds[] = {"none", "keyboard", "mouse", "pad", "wheel"};
  ImGui::Text("Steering %+.3f (%s, %s)", input.drive_steer,
              input.drive_steer_analog ? "analog" : "digital",
              kinds[static_cast<int>(input.drive_steer_kind) + 1]);
  ImGui::Text("Throttle %.3f | Brake %.3f | Clutch %.3f",
              input.drive_throttle, input.drive_brake, input.drive_clutch);
  ImGui::Text("Handbrake %s | Reverse %s | Shift %+.2f",
              input.drive_handbrake ? "on" : "off",
              input.drive_reverse ? "on" : "off", input.drive_shift);
}
}  // namespace

void DebugOverlay::set_context(Jvm& jvm, const SectionMachine& sections) {
  jvm_ = &jvm;
  sections_ = &sections;
}

void DebugOverlay::draw_vehicle_tab(std::span<Object* const> vehicles) {
  Object* player_chassis = nullptr;
  if (vehicles.empty() && jvm_) {
    const auto player = jvm_->get_static("java.game.GameLogic", "player");
    if (player.tag == ValueTag::Obj && player.v.o) {
      const auto field = player.v.o->fields.find("car");
      if (field != player.v.o->fields.end() && field->second.tag == ValueTag::Obj)
        player_chassis = part_debug_chassis(field->second.v.o);
    }
    if (player_chassis) vehicles = std::span<Object* const>(&player_chassis, 1);
  }
  auto selected = std::find_if(vehicles.begin(), vehicles.end(),
      [&](Object* object) { return vehicle_id(object) == selected_vehicle_; });
  if (selected == vehicles.end() && !vehicles.empty()) {
    selected = vehicles.begin();
    selected_vehicle_ = vehicle_id(*selected);
    freeze_vehicle_ = false;
  }
  if (selected != vehicles.end() &&
      ImGui::BeginCombo("Vehicle", vehicle_label(*selected).c_str())) {
    for (auto candidate = vehicles.begin(); candidate != vehicles.end(); ++candidate) {
      if (ImGui::Selectable(vehicle_label(*candidate).c_str(), candidate == selected)) {
        selected = candidate;
        selected_vehicle_ = vehicle_id(*selected);
        freeze_vehicle_ = false;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::Checkbox("Freeze telemetry", &freeze_vehicle_);
  if (!freeze_vehicle_)
    have_vehicle_ = selected != vehicles.end() && part_physics_state(*selected, vehicle_);
  if (have_vehicle_) draw_vehicle(vehicle_);
  else ImGui::TextUnformatted("No vehicle in this section.");
}

void DebugOverlay::draw(HWND hwnd, uint16_t width, uint16_t height, const char* reflection_status,
                        std::span<Object* const> vehicles) {
  if (!open_) return;
  if(!perf::enabled())perf::enable("");
  if (!initialized_) {
    imguiCreate();
    ImGui::GetIO().IniFilename = nullptr;
    // bgfx's Dear ImGui build disables the Win32 clipboard, so Ctrl+C and
    // SetClipboardText only reached an in-process buffer (#243).
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
      host_clipboard_set_text(text ? text : "");
    };
    platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
      static std::string text;
      text = host_clipboard_get_text();
      return text.c_str();
    };
    initialized_ = true;
  }
  POINT point{};
  if (!GetCursorPos(&point) || !ScreenToClient(hwnd, &point))
    point = {-1, -1};
  imguiBeginFrame(point.x, point.y, mouse_buttons_, 0, width, height, -1, kDebugView);
  bgfx::setViewName(kDebugView, "OpenSLRR debug overlay");
  ImGui::SetNextWindowSize(ImVec2(std::min(640.f, width * .9f),
                                 std::min(380.f, height * .7f)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(20, 100), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("OpenSLRR Debug", &open_)) {
    ImGui::Text("Section: %s", sections_ ? sections_->actual_name().c_str() : "(none)");
    if (ImGui::BeginTabBar("debug-tabs")) {
      if (ImGui::BeginTabItem("Vehicle")) {
        draw_vehicle_tab(vehicles);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Tuning")) {
        draw_tuning(jvm_);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Section")) {
        ImGui::PushID(sections_ ? sections_->actual() : nullptr);
        draw_section(jvm_, sections_, edit_fields_);
        ImGui::PopID();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("OSD")) {
        draw_osd_inspector();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Renderer")) {
        draw_render(width, height, render_options_, jvm_, reflection_status);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Input")) {
        draw_input();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
  imguiEndFrame();
}

bool DebugOverlay::captures_mouse() const {
  return mouse_buttons_ || (initialized_ && open_ && ImGui::GetIO().WantCaptureMouse);
}

bool DebugOverlay::handle_message(HWND hwnd, UINT message, WPARAM key,
                                  LPARAM detail, bool game_dragging) {
  if (key == VK_F12) {
    if (message == WM_KEYDOWN && (shortcut_down_ || GetKeyState(VK_CONTROL) < 0)) {
      if (!(detail & (1L << 30))) open_ = !open_;
      shortcut_down_ = true;
      return true;
    }
    if (message == WM_KEYUP && shortcut_down_) {
      shortcut_down_ = false;
      return true;
    }
  }
  if (message == WM_KILLFOCUS) shortcut_down_ = false;
  if (message == WM_KILLFOCUS || message == WM_CANCELMODE || message == WM_CAPTURECHANGED) {
    if (initialized_ && mouse_buttons_) {
      for (int button = 0; button < 3; ++button)
        ImGui::GetIO().AddMouseButtonEvent(button, false);
    }
    mouse_buttons_ = 0;
    return false;
  }
  if (!initialized_ || game_dragging) return false;
  const bool capture = captures_mouse();
  if (message == WM_SETCURSOR && capture && LOWORD(detail) == HTCLIENT) {
    SetCursor(LoadCursorA(nullptr, IDC_ARROW));
    return true;
  }
  if (message == WM_MOUSEMOVE) return capture;
  if (message == WM_MOUSEWHEEL && capture) {
    ImGui::GetIO().AddMouseWheelEvent(0.f, GET_WHEEL_DELTA_WPARAM(key) / float(WHEEL_DELTA));
    return true;
  }
  int button;
  switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: button = 0; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: button = 1; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: button = 2; break;
    default: return false;
  }
  const auto mask = static_cast<uint8_t>(1u << button);
  const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                    message == WM_MBUTTONDOWN;
  if (down ? !capture : !(mouse_buttons_ & mask)) return false;
  ImGui::GetIO().AddMousePosEvent(static_cast<short>(LOWORD(detail)),
                                 static_cast<short>(HIWORD(detail)));
  ImGui::GetIO().AddMouseButtonEvent(button, down);
  if (down) {
    if (!mouse_buttons_) SetCapture(hwnd);
    mouse_buttons_ |= mask;
  } else {
    mouse_buttons_ &= ~mask;
    if (!mouse_buttons_ && GetCapture() == hwnd) ReleaseCapture();
  }
  return true;
}

void DebugOverlay::shutdown() {
  if (initialized_) imguiDestroy();
  initialized_ = open_ = shortcut_down_ = false;
  freeze_vehicle_ = have_vehicle_ = false;
  edit_fields_ = false;
  selected_vehicle_ = 0;
  sections_ = nullptr;
  jvm_ = nullptr;
  render_options_ = {};
  part_physics_join();
  physics::tire_tuning() = {};
  mouse_buttons_ = 0;
}

}  // namespace slrr
