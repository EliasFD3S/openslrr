#pragma once
#include <cstdint>

namespace slrr {
class Jvm;
struct Object;
struct VideoMode {
  int width = 800, height = 600, depth = 32;
  bool windowed = true;
  bool operator==(const VideoMode&) const = default;
};
struct VideoOptions {
  VideoMode mode;
  float view_distance = 400.f, shadow_distance = 40.f;
  int shadow_size = 256, shadows = 1;
  bool flares = true;
  bool operator==(const VideoOptions&) const = default;
};
struct DrivingOptions {
  int transmission = 1;
  float steering_help = .666f, abs = 0.f, asr = 0.f;
  bool operator==(const DrivingOptions&) const = default;
};
struct GameOptions {
  DrivingOptions driving;
  VideoOptions video;
  int gps_mode = 0;
  float unit_meters = 1000.f;
  float deformation = 1.6f, internal_damage = 1.f, player_damage = .8f;
  float head_steer = 1.f, head_velocity = 1.f, head_acceleration = 1.f;
  float traffic_density = .2f, pedestrian_density = .1f;
  bool operator==(const GameOptions&) const = default;
};
const GameOptions& host_game_options();
uint64_t host_driving_options_revision();
void host_game_options_load(Jvm& jvm);
void host_game_options_dirty();
void host_set_video_mode(Jvm& jvm, VideoMode mode);
bool host_commit_video_dialog(Jvm& jvm, Object* handler, int command);
void host_game_options_apply(Jvm& jvm);
void host_game_options_set_steering_help(Jvm& jvm, float strength);
void host_game_options_update(Jvm& jvm, bool force = false);
}
