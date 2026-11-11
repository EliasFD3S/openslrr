#include "slrr/host_game_options.hpp"
#include "slrr/jvm.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace slrr {
namespace {
using Clock = std::chrono::steady_clock;
bool loaded = false, dirty = false;
Clock::time_point changed, next_sample;
GameOptions current;
uint64_t driving_revision = 0;

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
  return options;
}
}
EnhancedGraphics& host_enhanced_graphics() {
  static EnhancedGraphics graphics;
  return graphics;
}
const GameOptions& host_game_options() { return current; }
uint64_t host_driving_options_revision() { return driving_revision; }

void host_game_options_load(Jvm& jvm) {
  loaded=false;dirty=false;
  // Engine-owned Config in stock. A VM static keeps it alive through GC.
  Object* options=jvm.new_object("java.util.Config");
  jvm.set_static("java.lang.System","_hostGameOptions",Value::make_obj(options));
  jvm.invoke_by_name(options->class_name,"<init>",
                     {Value::make_obj(options),Value::make_int(0)},false);
  current = read_options(jvm);
  ++driving_revision;
  next_sample = Clock::now();
  loaded=true;
}
void host_game_options_dirty() {
  if(!loaded)return;
  dirty=true;changed=std::chrono::steady_clock::now();
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
  if (!dirty || (!force && Clock::now() - changed < std::chrono::milliseconds(500))) return;
  Value v=jvm.get_static("java.lang.System","_hostGameOptions");
  if(v.tag!=ValueTag::Obj || !v.v.o)return;
  Value result=jvm.invoke_by_name(v.v.o->class_name,"saveConfig",
       {v,Value::make_obj(jvm.new_string("save/game/options"))},false);
  if(result.tag==ValueTag::Int && result.v.i){dirty=false;std::printf("[options] saved save/game/options\n");}
  else {changed=std::chrono::steady_clock::now();std::fprintf(stderr,"[options] save failed\n");}
}
}
