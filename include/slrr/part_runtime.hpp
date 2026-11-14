#pragma once
#include "slrr/part_config.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace slrr {
class Jvm;
class RpkIndex;
struct Object;
struct InstalledPart {
  Object *object = nullptr;
  uint32_t resource = 0;
  Object *parent = nullptr;
  int parent_slot = 0, own_slot = 0;
  uint32_t texture = 0;
  float wear = 1.f, tear = 1.f;
};
void register_part_natives(Jvm &jvm, RpkIndex &resources);
Object *part_create(Jvm &jvm, RpkIndex &resources, Object *parent,
                    uint32_t resource, const std::string &alias,
                    const std::string &params = {});
// Positive-mass native map placement. Uses the loose-part mesh solver and
// exchanges contact impulses with the controlled car and static scene.
// Returns the loose body's node, or nullptr when it could not be created.
Object* part_add_dynamic_native(const PartConfig& config,
                             const std::vector<PartRender>& renders,
                             const PartMatrix& world, void* parent, int32_t seed);
bool part_install(Object *child, Object *root, int own_slot = 0,
                  int parent_slot = 0, const float *hint = nullptr);
bool part_destroy(Object *part);
bool part_command(Object *part, const std::string &command);
// Car setMatrix: loaded springs, contacts on the given point, body asleep.
// shift is false for reset, which tucks the springs and does not move the body.
void part_park_chassis(Object *car, bool shift = true);
// setState supplies world-space linear/angular velocity after setting the pose.
void part_apply_state(Object *car, const float* velocity, const float* angular_velocity);
// The point setMatrix consumed to produce the current pose: the midpoint of
// the static tyre contacts. getPos of a car returns this, so a saved pose
// placed again does not climb by the ride height.
bool part_car_script_pos(void *key, float *x, float *y, float *z);
// City lamps require a running engine at night. The showroom preview can
// demonstrate the lights with its own time-of-day toggle.
bool part_lamp_lit(void *part, uint32_t flags, bool preview);
// Render node or one of its parents, when that node is an installed part.
Object *part_render_owner(void *key);
// World height of the tyre contact under a car. The camera's `move` spring
// hangs on this point; `look` stays on the chassis origin.
bool part_car_ground(void *key, float *y);
bool part_camera_pose(void* car, int camera, float* matrix);
// Chassis owning a render node, with its active-world script frame.
void *part_shadow_source(void *render, float *frame);
// Advance only cars belonging to the active world, at a fixed physics step.
void part_physics_advance(double seconds);
// GroundRef pedestrian natives. Density is people per square metre after the
// script's Config factor. Types are the GameRefs Valocity registers.
void part_set_pedestrian_density(void* ground, float density);
void part_add_pedestrian_type(void* ground, void* type);
void part_remove_pedestrian_type(void* ground, void* type);
// Distance in metres from a point to the nearest live pedestrian of that
// type id on this ground, or of any type when type_id is 0. -1 when none.
float part_pedestrian_distance(void* ground, float x, float y, float z, int32_t type_id);
// Map unload. Drops the seated driver and the walking crowd.
void part_drop_actors();
void part_pause_engine_audio(bool paused);
struct PartPhysicsState {
  bool active = false;
  double mass = 0, height = 0, vertical_speed = 0;
  uint64_t ticks = 0, backlog = 0;
  std::array<double, 4> length{}, force{};
  std::array<std::array<double, 3>, 4> wheel_center{};
  std::array<bool, 4> contact{};
  std::array<bool, 4> geometry_solved{};
  struct TireSample {
    double steer = 0, slip_speed = 0, lateral_speed = 0;
    double longitudinal_force = 0, lateral_force = 0;
  };
  std::array<TireSample, 4> tires{};
  double speed_kph = 0, rpm = 0, yaw_rate = 0;
  double steering_help = 0, steering_request = 0, steering_angle = 0;
  double steering_correction = 0;
  int gear = 0;
};
bool part_physics_state(Object *car, PartPhysicsState &state);
Object* part_controlled_chassis();
// Top-level cars of the active world, for trigger volumes: the chassis key,
// the id() scripts see for its Vehicle, and the body position in script metres.
struct PartVehiclePosition {
  Object* chassis = nullptr;
  int32_t id = 0;
  float position[3] = {0.f, 0.f, 0.f};
};
std::vector<PartVehiclePosition> part_vehicle_positions();
// Chassis mesh of a Vehicle. Follows a bound alias, then the chassis field.
Object* part_chassis(Object* object);
Object* part_debug_chassis(Object* object);
// One installed part as the damage gauge (osd_damage) sees it. `x` and `z`
// are the centre of its drawn meshes seen from above, normalised to the
// car's top-down extent: -1 is the left / front edge, +1 the right / rear.
struct VehicleHudPart {
  enum class Kind : uint8_t { Engine, Body, Wheel };
  Kind kind = Kind::Body;
  int8_t corner = -1;   // Wheel: the corner of its chassis slot (101 + corner)
  float x = 0, z = 0;
  float condition = 1;  // Stock Part.getConditionNoAttach: wear * tear
};
struct VehicleHudState {
  const VehicleHudLayout *layout = nullptr;
  const std::string *preview_directory = nullptr;
  Object *owner = nullptr;
  double rpm = 0, speed = 0, mileage = 0;
  float unit_scale = 1.f;
  float nitro = 0, turbo = 0;
  int gear = 0;
  bool handbrake = false;
  // Engine (category 1), body (category 2) and wheel parts of the car. A
  // part absent from the tree is absent here, so its gauge blip goes away.
  std::array<VehicleHudPart, 192> parts{};
  size_t part_count = 0;
};
void part_set_hud_preview(const std::string &path);
bool part_vehicle_hud(VehicleHudState &state);
std::vector<InstalledPart> part_installed_tree(Object *root);
Object *vehicle_spawn_stock(Jvm &jvm, RpkIndex &resources, Object *parent,
                            uint32_t chassis, uint32_t texture = 0);
} // namespace slrr
