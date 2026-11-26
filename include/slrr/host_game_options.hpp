#pragma once
#include <cstdint>

namespace slrr {
class Jvm;
struct Object;
struct EnhancedGraphics {
  int msaa = 1;
  int ao_method = 0; // 0 off, 1 HBAO, 2 GTAO
  int ao_quality = 1; // low / medium / high
  int ao_strength = 100, ao_radius = 100; // percent / centimetres
  int shadow_quality = 1, shadow_smoothing = 1;
  bool anisotropic_filtering = true;
  bool dynamic_reflections = true;
  bool volumetric_clouds = true;
  bool dynamic_lighting = true;
  bool local_lights = true;
  // Advanced video options (#377). The F12 debug switches still turn each
  // feature off on top of these.
  bool vsync = true;
  int view_distance = 3000;      // metres of exterior scenery, 500-3000
  int prop_detail = 2;           // map prop LOD: 0 low, 1 medium, 2 high, 3 ultra
  bool fog = true;
  bool car_lights = true;        // headlight beams and brake lights
  bool headlight_shadows = true;
  bool street_lamps = true;      // street lamps light the scene at night
  int shadowed_lamps = 6;        // street lamps with a shadow tile, 0-6
  bool operator==(const EnhancedGraphics&) const = default;
};
EnhancedGraphics& host_enhanced_graphics();
// Host-only input settings (#383), saved with the graphics ones.
struct InputTuning {
  // How much an analog wheel or stick's full lock shrinks with speed, in
  // percent: 0 keeps full lock at any speed, 100 narrows it like keyboard
  // steering (the angle a 1.2 g turn needs plus the tyre's peak slip).
  int steer_speed_sensitivity = 50;
  // Force feedback direction flip (option 25), for a wheel whose driver
  // reports its force axis the other way round.
  bool ffb_invert = false;
  // Force feedback road feel (option 26, percent, 0-200): bump and kerb
  // kicks on top of the tyres' aligning torque. Saved as ffb_detail.
  int ffb_detail = 100;
  // Force feedback settings (#405), each in percent unless noted:
  int ffb_impacts = 100;   // option 27, 0-200: knocks from crashes
  int ffb_vibration = 50;  // option 28, 0-200: tyre scrub vibration
  int ffb_damper = 100;    // option 29, 0-200: weight when parked, friction
  int ffb_min_force = 0;   // option 30, 0-20: lifts small forces past a
                           // wheel's dead band (belt and gear wheels)
  int ffb_smoothing = 10;  // option 31, 0-50 ms: low-pass on the force
};
InputTuning& host_input_tuning();
// Host-only HUD settings (#446), saved with the others.
struct HudTuning {
  // Option 32: the brake and tyre temperature panel (0 off, 1 on).
  bool temperatures = true;
};
HudTuning& host_hud_tuning();
// Persist host-only graphics alongside the stock options, without changing Config.
// Saved as JSON in save/game/openslrr_options.json (#405); the key=value
// save/game/openslrr_options of older builds is read once and kept.
bool host_save_graphics_options();
// A host-only option by its OptionsDialog renderer option id (0-32, see the
// table in host_game_options.cpp). get returns false for an unknown id; set
// clamps to the option's range, applies it and saves when it changed.
bool host_option_get(int id, int& value);
bool host_option_set(int id, int value);
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
  // Stock Config.ForceFeedBack and FFB_strength (0..1).
  bool ffb = true;
  float ffb_strength = 1.f;
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
