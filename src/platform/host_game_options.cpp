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
  const auto updated = read_options(jvm);
  if (updated == current) return;
  if (updated.driving != current.driving) ++driving_revision;
  current = updated;
  host_game_options_dirty();
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
