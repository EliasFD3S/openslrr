#include "slrr/host_game_options.hpp"
#include "slrr/jvm.hpp"
#include "host_input_devices.hpp"
#include "slrr/host_sfx.hpp"
#include "host_options_json.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string_view>
#include <vector>
#include <filesystem>
#include <string>

namespace slrr {
namespace {
using Clock = std::chrono::steady_clock;
bool loaded = false, dirty = false, save_failed = false;
Clock::time_point changed, next_sample;
GameOptions current;
uint64_t driving_revision = 0;
std::string host_options_path;
bool save_host_options();

// Host-only settings (#405: one table, saved as JSON). Config has a fixed
// binary layout, so a new field there would need a patched Config class.
// Each entry is one OptionsDialog renderer option: its id, where it is saved,
// its range, and how to read and write it. Booleans are 0..1. A new option
// is one line here. Id 5 was the framerate mode: cars are always drawn
// between ticks now, and an old "framerate_mode" key is ignored.
struct HostOption {
  int id;
  const char* section;
  const char* key;
  int low, high;
  bool boolean;
  int (*get)();
  void (*set)(int);
  const char* names[2];  // a named two-value option, saved by name; else null
};

#define SLRR_GRAPHICS_INT(id, key, low, high)                         \
  HostOption{id, "video", #key, low, high, false,                      \
             +[] { return host_enhanced_graphics().key; },             \
             +[](int v) { host_enhanced_graphics().key = v; }, {}}
#define SLRR_GRAPHICS_BOOL(id, key)                                   \
  HostOption{id, "video", #key, 0, 1, true,                            \
             +[] { return int(host_enhanced_graphics().key); },        \
             +[](int v) { host_enhanced_graphics().key = v != 0; }, {}}
#define SLRR_AUDIO_BOOL(id, key, field)                               \
  HostOption{id, "audio", key, 0, 1, true,                             \
             +[] { return int(host_sfx_spatial_options().field); },    \
             +[](int v) { host_sfx_spatial_options().field = v != 0; }, {}}
#define SLRR_INPUT_INT(id, key, low, high)                            \
  HostOption{id, "input", #key, low, high, false,                      \
             +[] { return host_input_tuning().key; },                  \
             +[](int v) { host_input_tuning().key = v; }, {}}

const HostOption kOptions[] = {
    SLRR_GRAPHICS_INT(0, msaa, 0, 3),
    SLRR_GRAPHICS_BOOL(6, anisotropic_filtering),
    SLRR_GRAPHICS_BOOL(1, dynamic_reflections),
    SLRR_GRAPHICS_BOOL(2, volumetric_clouds),
    SLRR_GRAPHICS_BOOL(3, dynamic_lighting),
    SLRR_GRAPHICS_BOOL(4, local_lights),
    SLRR_GRAPHICS_INT(7, ao_method, 0, 2),
    SLRR_GRAPHICS_INT(8, ao_quality, 0, 2),
    SLRR_GRAPHICS_INT(11, ao_strength, 0, 200),
    SLRR_GRAPHICS_INT(12, ao_radius, 25, 300),
    SLRR_GRAPHICS_INT(9, shadow_quality, 0, 2),
    SLRR_GRAPHICS_INT(10, shadow_smoothing, 0, 2),
    // Advanced video page (#377).
    SLRR_GRAPHICS_BOOL(13, vsync),
    SLRR_GRAPHICS_INT(14, view_distance, 500, 3000),
    SLRR_GRAPHICS_INT(15, prop_detail, 0, 3),
    SLRR_GRAPHICS_BOOL(16, fog),
    SLRR_GRAPHICS_BOOL(17, car_lights),
    SLRR_GRAPHICS_BOOL(18, headlight_shadows),
    SLRR_GRAPHICS_BOOL(19, street_lamps),
    SLRR_GRAPHICS_INT(20, shadowed_lamps, 0, 6),
    // Spatial audio (#390).
    SLRR_AUDIO_BOOL(22, "audio_positioning", positioning),
    SLRR_AUDIO_BOOL(23, "audio_doppler", doppler),
    SLRR_AUDIO_BOOL(24, "audio_reverb", reverb),
    // Controls (#383) and force feedback (#395, #399, #405).
    SLRR_INPUT_INT(21, steer_speed_sensitivity, 0, 100),
    HostOption{25, "input", "ffb_invert", 0, 1, true,
               +[] { return int(host_input_tuning().ffb_invert); },
               +[](int v) { host_input_tuning().ffb_invert = v != 0; }, {}},
    SLRR_INPUT_INT(26, ffb_detail, 0, 200),
    SLRR_INPUT_INT(27, ffb_impacts, 0, 200),
    SLRR_INPUT_INT(28, ffb_vibration, 0, 200),
    SLRR_INPUT_INT(29, ffb_damper, 0, 200),
    SLRR_INPUT_INT(30, ffb_min_force, 0, 20),
    SLRR_INPUT_INT(31, ffb_smoothing, 0, 50),
    // HUD (#446).
    HostOption{32, "hud", "temperatures", 0, 1, true,
               +[] { return int(host_hud_tuning().temperatures); },
               +[](int v) { host_hud_tuning().temperatures = v != 0; }, {}},
};
#undef SLRR_GRAPHICS_INT
#undef SLRR_GRAPHICS_BOOL
#undef SLRR_AUDIO_BOOL
#undef SLRR_INPUT_INT

constexpr int kOptionsVersion = 1;

const HostOption* option_by_id(int id) {
  for (const auto& o : kOptions)
    if (o.id == id) return &o;
  return nullptr;
}

void set_option(const HostOption& o, int value) {
  o.set(o.boolean ? int(value != 0) : std::clamp(value, o.low, o.high));
}

// A saved value: a number (booleans also as 0/1), a boolean, or a name.
bool read_value(const HostOption& o, const options_json::Scalar& v, int& out) {
  switch (v.kind) {
    case options_json::Scalar::Number:
      if (!std::isfinite(v.number)) return false;
      out = static_cast<int>(std::lround(std::clamp(v.number, -1e9, 1e9)));
      return true;
    case options_json::Scalar::Bool:
      out = v.boolean;
      return o.boolean;
    case options_json::Scalar::String:
      for (int i = 0; i < 2; ++i)
        if (o.names[i] && v.text == o.names[i]) {
          out = i;
          return true;
        }
      return false;
  }
  return false;
}

// The key=value file written before #405 (save/game/openslrr_options).
void read_legacy_options(std::istream& in) {
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), text = line.substr(eq + 1);
    for (const auto& o : kOptions) {
      if (key != o.key) continue;
      options_json::Scalar value;
      int parsed = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
      if (error == std::errc{} && end == text.data() + text.size()) {
        value.number = parsed;
      } else {
        value.kind = options_json::Scalar::String;
        value.text = text;
      }
      if (read_value(o, value, parsed)) set_option(o, parsed);
    }
  }
}

void load_host_options(const std::string& root) {
  std::string dir = root;
  if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir.push_back('\\');
  dir += "save\\game\\";
  host_input_devices().set_calibration_file(dir + "openslrr_axis_calibration");
  host_options_path = dir + "openslrr_options.json";
  const std::string legacy_path = dir + "openslrr_options";
  host_enhanced_graphics() = {};
  host_input_tuning() = {};
  host_sfx_spatial_options() = {};

  bool loaded_json = false;
  if (std::ifstream in{host_options_path, std::ios::binary}) {
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    in.close();
    std::map<std::string, options_json::Scalar> values;
    std::string error;
    if (options_json::parse(text, values, &error)) {
      // A missing or unreadable key keeps its default.
      for (const auto& o : kOptions) {
        const auto it = values.find(std::string(o.section) + "." + o.key);
        int value = 0;
        if (it != values.end() && read_value(o, it->second, value)) set_option(o, value);
      }
      loaded_json = true;
    } else {
      // Keep the damaged file for a look; the next save writes a new one.
      std::fprintf(stderr, "[options] %s: %s; using defaults\n", host_options_path.c_str(),
                   error.c_str());
      std::error_code ec;
      std::filesystem::rename(host_options_path, host_options_path + ".bad", ec);
    }
  }
  if (!loaded_json) {
    // First start after #405: take the old file over and save it as JSON.
    // The old file stays, for older builds.
    if (std::ifstream legacy{legacy_path}) {
      read_legacy_options(legacy);
      legacy.close();
      save_host_options();
    }
  }
  host_sfx_apply_spatial_options();
}

bool save_host_options() {
  if (host_options_path.empty()) return false;
  std::error_code error;
  const std::filesystem::path path(host_options_path);
  std::filesystem::create_directories(path.parent_path(), error);
  std::vector<options_json::Entry> entries;
  for (const auto& o : kOptions) {
    options_json::Entry e{o.section, o.key, {}};
    const int value = std::clamp(o.get(), o.low, o.high);
    if (o.names[0]) {
      e.value.kind = options_json::Scalar::String;
      e.value.text = o.names[value];
    } else if (o.boolean) {
      e.value.kind = options_json::Scalar::Bool;
      e.value.boolean = value != 0;
    } else {
      e.value.number = value;
    }
    entries.push_back(std::move(e));
  }
  // Write beside it, then replace: a crash mid-save keeps the old file.
  const std::string temporary = host_options_path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << options_json::write(kOptionsVersion, entries);
    out.close();
    if (!out) {
      std::fprintf(stderr, "[options] could not save %s\n", temporary.c_str());
      return false;
    }
  }
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::fprintf(stderr, "[options] could not save %s: %s\n", host_options_path.c_str(),
                 error.message().c_str());
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

void write_video_mode(Jvm& jvm, const VideoMode& mode) {
  for (const auto* config : {"java.util.Config", "java.game.Config"}) {
    jvm.set_static(config, "video_x", Value::make_int(mode.width));
    jvm.set_static(config, "video_y", Value::make_int(mode.height));
    jvm.set_static(config, "video_depth", Value::make_int(mode.depth));
    jvm.set_static(config, "video_windowed", Value::make_int(mode.windowed ? 1 : 0));
  }
}

GameOptions read_options(Jvm& jvm) {
  const auto read = [&jvm](const char* field, float fallback) {
    const auto value = jvm.get_static("java.util.Config", field);
    if (value.tag == ValueTag::Int) return float(value.v.i);
    return value.tag == ValueTag::Float && std::isfinite(value.v.f) ? value.v.f : fallback;
  };
  const auto integer = [&jvm](const char* field, int fallback) {
    const auto value = jvm.get_static("java.util.Config", field);
    return value.tag == ValueTag::Int ? value.v.i : fallback;
  };
  GameOptions options;
  auto& video = options.video;
  video.mode = {std::clamp(integer("video_x", 800), 320, 8192),
                std::clamp(integer("video_y", 600), 240, 8192),
                integer("video_depth", 32), integer("video_windowed", 1) != 0};
  video.view_distance = std::clamp(read("camera_ext_viewrange", 400.f), 50.f, 2050.f);
  const float shadow_detail = std::max(read("shadow_detail", .25f), .001f);
  video.shadow_distance = std::clamp(10.f / shadow_detail, 10.f, 210.f);
  video.shadow_size = std::clamp(integer("shadow_size", 256), 32, 1024);
  video.shadows = std::clamp(integer("shadows", 1), 0, 2);
  video.flares = integer("flares", 1) != 0;
  const int mode = integer("player_transmission", 1);
  options.driving.transmission = mode == 0 || mode == 2 || mode == 5 ? mode : 1;
  options.driving.steering_help = std::clamp(read("player_steeringhelp", .666f), 0.f, 1.f);
  options.driving.abs = read("player_abs", 0.f);
  options.driving.asr = read("player_asr", 0.f);
  options.gps_mode = std::clamp(integer("gpsMode", 0), 0, 3);
  const auto distance = jvm.get_static("java.lang.System", "_hostDistanceUnitMeters");
  options.unit_meters = distance.tag == ValueTag::Float && distance.v.f > 0.f
      ? distance.v.f : integer("metricSystem", 1) == 0 ? 1600.f : 1000.f;
  options.deformation = std::max(0.f, read("deformation", 1.6f));
  options.internal_damage = std::max(0.f, read("internal_damage", 1.f));
  options.player_damage = std::max(0.f, read("player_damage_multiplier", .8f));
  options.head_steer = std::clamp(read("head_move_steer", 1.f), 0.f, 2.f);
  options.head_velocity = std::clamp(read("head_move_vel", 1.f), 0.f, 2.f);
  options.head_acceleration = std::clamp(read("head_move_acc", 1.f), 0.f, 2.f);
  options.traffic_density = read("trafficDensity", .2f);
  options.pedestrian_density = read("pedestrianDensity", .1f);
  options.ffb = integer("ForceFeedBack", 1) != 0;
  options.ffb_strength = std::clamp(read("FFB_strength", 1.f), 0.f, 1.f);
  return options;
}
}
InputTuning& host_input_tuning() {
  static InputTuning tuning;
  return tuning;
}
HudTuning& host_hud_tuning() {
  static HudTuning tuning;
  return tuning;
}
EnhancedGraphics& host_enhanced_graphics() {
  static EnhancedGraphics graphics;
  return graphics;
}
bool host_save_graphics_options() { return save_host_options(); }
bool host_option_get(int id, int& value) {
  const HostOption* o = option_by_id(id);
  if (!o) return false;
  value = o->get();
  return true;
}
bool host_option_set(int id, int value) {
  const HostOption* o = option_by_id(id);
  if (!o) return false;
  const int before = o->get();
  set_option(*o, value);
  if (std::string_view(o->section) == "audio") host_sfx_apply_spatial_options();
  if (o->get() != before) save_host_options();
  return true;
}
const GameOptions& host_game_options() { return current; }
uint64_t host_driving_options_revision() { return driving_revision; }

void host_game_options_load(Jvm& jvm) {
  loaded=false;dirty=false;save_failed=false;
  // Engine-owned Config in stock. A VM static keeps it alive through GC.
  Object* options=jvm.new_object("java.util.Config");
  jvm.set_static("java.lang.System","_hostGameOptions",Value::make_obj(options));
  jvm.invoke_by_name(options->class_name,"<init>",
                     {Value::make_obj(options),Value::make_int(0)},false);
  current = read_options(jvm);
  load_host_options(jvm.game_root());
  ++driving_revision;
  next_sample = Clock::now();
  loaded=true;
}
void host_game_options_dirty() {
  if(!loaded)return;
  dirty=true;save_failed=false;changed=std::chrono::steady_clock::now();
}
void host_game_options_apply(Jvm& jvm) {
  if (!loaded) return;
  next_sample = Clock::now() + std::chrono::milliseconds(100);
  auto updated = read_options(jvm);
  // Only an explicit mode change may resize the window. Restore the
  // selected values if a script reinitializes Config while opening a screen.
  if (updated.video.mode != current.video.mode) {
    write_video_mode(jvm, current.video.mode);
    updated.video.mode = current.video.mode;
  }
  if (updated == current) return;
  if (updated.driving != current.driving) ++driving_revision;
  current = updated;
  host_game_options_dirty();
}

void host_set_video_mode(Jvm& jvm, VideoMode mode) {
  if (mode.width < 320 || mode.width > 8192 || mode.height < 240 || mode.height > 8192)
    return;
  mode.depth = mode.depth == 16 ? 16 : 32;
  const bool changed_mode = mode != current.video.mode;
  current.video.mode = mode;
  write_video_mode(jvm, mode);
  if (changed_mode) host_game_options_dirty();
  host_game_options_apply(jvm);
}

bool host_commit_video_dialog(Jvm& jvm, Object* handler, int command) {
  if (!handler || handler->class_name != "java.game.OptionsDialog") return false;
  const auto live_video = jvm.get_static(handler->class_name, "OPENSLRR_LIVE_VIDEO");
  if (live_video.tag == ValueTag::Int && live_video.v.i == 1) return false;
  const auto done = jvm.get_static(handler->class_name, "CMD_VIDEO_OPTIONS_DONE");
  if (done.tag != ValueTag::Int || done.v.i != command) return false;
  // MWM's Back handler treats a new mode as a restart request. Keep the
  // original Java navigation and apply the pending mode in the host.
  const auto width = jvm.get_static(handler->class_name, "aspectX");
  const auto height = jvm.get_static(handler->class_name, "aspectY");
  const auto depth = handler->fields.find("color_depth");
  const auto windowed = handler->fields.find("windowed");
  const auto dimension = [](Value value) {
    const float n = value.tag == ValueTag::Int ? float(value.v.i) :
                    value.tag == ValueTag::Float ? value.v.f : 0.f;
    return n >= 240.f && n <= 8192.f ? static_cast<int>(n) : 0;
  };
  const auto group = handler->fields.find("optionsGroup");
  const auto active = handler->fields.find("actGroup");
  const auto video = handler->fields.find("videoGroup");
  if (depth == handler->fields.end() || windowed == handler->fields.end() ||
      group == handler->fields.end() || active == handler->fields.end() ||
      video == handler->fields.end() || active->second.v.i != video->second.v.i ||
      depth->second.tag != ValueTag::Int || windowed->second.tag != ValueTag::Int)
    return false;
  const VideoMode mode{dimension(width), dimension(height), depth->second.v.i,
                       windowed->second.v.i != 0};
  if (mode.width < 320 || mode.height < 240) return false;
  const auto pending_loading = handler->fields.find("loading_primary");
  const auto original_loading = handler->fields.find("tmp_lp");
  if (mode == current.video.mode && pending_loading != handler->fields.end() &&
      original_loading != handler->fields.end() &&
      pending_loading->second.v.i != original_loading->second.v.i)
    return false;
  host_set_video_mode(jvm, mode);
  jvm.invoke(handler->class_name, "changeMode", "(I)V",
             {Value::make_obj(handler), group->second}, false);
  return true;
}

void host_game_options_set_steering_help(Jvm& jvm, float strength) {
  if (!loaded || !std::isfinite(strength)) return;
  jvm.set_static("java.util.Config", "player_steeringhelp",
                 Value::make_float(std::clamp(strength, 0.f, 1.f)));
  host_game_options_apply(jvm);
}

void host_game_options_update(Jvm& jvm, bool force) {
  if (!loaded) return;
  if (force || Clock::now() >= next_sample) host_game_options_apply(jvm);
  if (!dirty || (!force && (save_failed || Clock::now() - changed < std::chrono::milliseconds(500)))) return;
  Value v=jvm.get_static("java.lang.System","_hostGameOptions");
  if(v.tag!=ValueTag::Obj || !v.v.o)return;
  Value result=jvm.invoke_by_name(v.v.o->class_name,"saveConfig",
       {v,Value::make_obj(jvm.new_string("save/game/options"))},false);
  if(result.tag==ValueTag::Int && result.v.i){dirty=false;save_failed=false;std::printf("[options] saved save/game/options\n");}
  else {
    if (!save_failed) std::fprintf(stderr,"[options] save failed; retry on next change or shutdown\n");
    save_failed = true;
  }
}
}
