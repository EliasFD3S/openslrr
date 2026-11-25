#include "slrr/host_game_options.hpp"
#include "slrr/jvm.hpp"
#include "host_input_devices.hpp"
#include "slrr/host_sfx.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <string>

namespace slrr {
namespace {
using Clock = std::chrono::steady_clock;
bool loaded = false, dirty = false, save_failed = false;
Clock::time_point changed, next_sample;
GameOptions current;
uint64_t driving_revision = 0;
FramerateMode framerate_mode = FramerateMode::Adaptive;
std::string host_options_path;

bool integer_option(const std::string& line,const char* key,int& target,int low,int high) {
  const std::string prefix=std::string(key)+"=";
  if(line.rfind(prefix,0)!=0) return false;
  int value=target;const auto* begin=line.data()+prefix.size();
  auto [end,error]=std::from_chars(begin,line.data()+line.size(),value);
  if(error==std::errc{} && end==line.data()+line.size()) target=std::clamp(value,low,high);
  return true;
}

// Host-only settings live beside the stock options. Config has a fixed
// binary layout, so a new field there would need a patched Config class.
void load_host_options(const std::string& root) {
  host_options_path = root;
  if (!host_options_path.empty() && host_options_path.back() != '\\' &&
      host_options_path.back() != '/')
    host_options_path.push_back('\\');
  host_input_devices().set_calibration_file(
      host_options_path + "save\\game\\openslrr_axis_calibration");
  host_options_path += "save\\game\\openslrr_options";
  host_enhanced_graphics() = {};
  host_input_tuning() = {};
  host_sfx_spatial_options() = {};
  framerate_mode = FramerateMode::Adaptive;
  auto& graphics = host_enhanced_graphics();
  auto& sound = host_sfx_spatial_options();
  std::ifstream in(host_options_path);
  for (std::string line; std::getline(in, line);)
    if (line == "framerate_mode=classic") framerate_mode = FramerateMode::Classic;
    else if (line == "framerate_mode=adaptive") framerate_mode = FramerateMode::Adaptive;
    else if (line == "msaa=0") graphics.msaa = 0;
    else if (line == "msaa=1") graphics.msaa = 1;
    else if (line == "msaa=2") graphics.msaa = 2;
    else if (line == "msaa=3") graphics.msaa = 3;
    else if(integer_option(line,"ao_method",graphics.ao_method,0,2) ||
            integer_option(line,"ao_quality",graphics.ao_quality,0,2) ||
            integer_option(line,"ao_strength",graphics.ao_strength,0,200) ||
            integer_option(line,"ao_radius",graphics.ao_radius,25,300) ||
            integer_option(line,"shadow_quality",graphics.shadow_quality,0,2) ||
            integer_option(line,"shadow_smoothing",graphics.shadow_smoothing,0,2) ||
            integer_option(line,"view_distance",graphics.view_distance,500,3000) ||
            integer_option(line,"prop_detail",graphics.prop_detail,0,3) ||
            integer_option(line,"shadowed_lamps",graphics.shadowed_lamps,0,6) ||
            integer_option(line,"steer_speed_sensitivity",
                           host_input_tuning().steer_speed_sensitivity,0,100)) {}
    else if (integer_option(line, "ffb_detail", host_input_tuning().ffb_detail, 0, 200) ||
             integer_option(line, "ffb_impacts", host_input_tuning().ffb_impacts, 0, 200) ||
             integer_option(line, "ffb_vibration", host_input_tuning().ffb_vibration, 0, 200) ||
             integer_option(line, "ffb_damper", host_input_tuning().ffb_damper, 0, 200) ||
             integer_option(line, "ffb_min_force", host_input_tuning().ffb_min_force, 0, 20) ||
             integer_option(line, "ffb_smoothing", host_input_tuning().ffb_smoothing, 0, 50)) {}
    else if (line == "ffb_invert=1") host_input_tuning().ffb_invert = true;
    else if (line == "ffb_invert=0") host_input_tuning().ffb_invert = false;
    else for (const auto& entry : {std::pair{"anisotropic_filtering", &graphics.anisotropic_filtering},
                                  std::pair{"dynamic_reflections", &graphics.dynamic_reflections},
                                  std::pair{"volumetric_clouds", &graphics.volumetric_clouds},
                                  std::pair{"dynamic_lighting", &graphics.dynamic_lighting},
                                  std::pair{"local_lights", &graphics.local_lights},
                                  std::pair{"vsync", &graphics.vsync},
                                  std::pair{"fog", &graphics.fog},
                                  std::pair{"car_lights", &graphics.car_lights},
                                  std::pair{"headlight_shadows", &graphics.headlight_shadows},
                                  std::pair{"street_lamps", &graphics.street_lamps},
                                  std::pair{"audio_positioning", &sound.positioning},
                                  std::pair{"audio_doppler", &sound.doppler},
                                  std::pair{"audio_reverb", &sound.reverb}}) {
      if (line == std::string(entry.first) + "=0") *entry.second = false;
      if (line == std::string(entry.first) + "=1") *entry.second = true;
    }
  host_sfx_apply_spatial_options();
}

bool save_host_options() {
  if (host_options_path.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(host_options_path).parent_path(), error);
  const auto& graphics = host_enhanced_graphics();
  std::ofstream out(host_options_path, std::ios::trunc);
  out << "framerate_mode="
      << (framerate_mode == FramerateMode::Classic ? "classic" : "adaptive") << '\n'
      << "msaa=" << std::clamp(graphics.msaa, 0, 3) << '\n'
      << "anisotropic_filtering=" << graphics.anisotropic_filtering << '\n'
      << "dynamic_reflections=" << graphics.dynamic_reflections << '\n'
      << "volumetric_clouds=" << graphics.volumetric_clouds << '\n'
      << "dynamic_lighting=" << graphics.dynamic_lighting << '\n'
      << "local_lights=" << graphics.local_lights << '\n'
      << "ao_method=" << std::clamp(graphics.ao_method,0,2) << '\n'
      << "ao_quality=" << std::clamp(graphics.ao_quality,0,2) << '\n'
      << "ao_strength=" << std::clamp(graphics.ao_strength,0,200) << '\n'
      << "ao_radius=" << std::clamp(graphics.ao_radius,25,300) << '\n'
      << "shadow_quality=" << std::clamp(graphics.shadow_quality,0,2) << '\n'
      << "shadow_smoothing=" << std::clamp(graphics.shadow_smoothing,0,2) << '\n'
      << "vsync=" << graphics.vsync << '\n'
      << "view_distance=" << std::clamp(graphics.view_distance,500,3000) << '\n'
      << "prop_detail=" << std::clamp(graphics.prop_detail,0,3) << '\n'
      << "fog=" << graphics.fog << '\n'
      << "car_lights=" << graphics.car_lights << '\n'
      << "headlight_shadows=" << graphics.headlight_shadows << '\n'
      << "street_lamps=" << graphics.street_lamps << '\n'
      << "shadowed_lamps=" << std::clamp(graphics.shadowed_lamps,0,6) << '\n'
      << "audio_positioning=" << host_sfx_spatial_options().positioning << '\n'
      << "audio_doppler=" << host_sfx_spatial_options().doppler << '\n'
      << "audio_reverb=" << host_sfx_spatial_options().reverb << '\n'
      << "steer_speed_sensitivity="
      << std::clamp(host_input_tuning().steer_speed_sensitivity,0,100) << '\n'
      << "ffb_invert=" << host_input_tuning().ffb_invert << '\n'
      << "ffb_detail=" << std::clamp(host_input_tuning().ffb_detail, 0, 200) << '\n'
      << "ffb_impacts=" << std::clamp(host_input_tuning().ffb_impacts, 0, 200) << '\n'
      << "ffb_vibration=" << std::clamp(host_input_tuning().ffb_vibration, 0, 200) << '\n'
      << "ffb_damper=" << std::clamp(host_input_tuning().ffb_damper, 0, 200) << '\n'
      << "ffb_min_force=" << std::clamp(host_input_tuning().ffb_min_force, 0, 20) << '\n'
      << "ffb_smoothing=" << std::clamp(host_input_tuning().ffb_smoothing, 0, 50) << '\n';
  out.close();
  if (!out) std::fprintf(stderr, "[options] could not save %s\n", host_options_path.c_str());
  return bool(out);
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
EnhancedGraphics& host_enhanced_graphics() {
  static EnhancedGraphics graphics;
  return graphics;
}
bool host_save_graphics_options() { return save_host_options(); }
const GameOptions& host_game_options() { return current; }
FramerateMode host_framerate_mode() { return framerate_mode; }
void host_set_framerate_mode(FramerateMode mode) {
  if (mode == framerate_mode) return;
  framerate_mode = mode;
  save_host_options();
}
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
