#pragma once
#include "slrr/part_config.hpp"
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
bool part_install(Object *child, Object *root, int own_slot = 0,
                  int parent_slot = 0);
bool part_destroy(Object *part);
bool part_command(Object *part, const std::string &command);
// Car setMatrix: loaded springs, contacts on the given point, body asleep.
// shift is false for reset, which tucks the springs and does not move the body.
void part_park_chassis(Object *car, bool shift = true);
// Render node or one of its parents, when that node is an installed part.
Object *part_render_owner(void *key);
// World height of the tyre contact under a car. The camera's `move` spring
// hangs on this point; `look` stays on the chassis origin.
bool part_car_ground(void *key, float *y);
// Chassis owning a render node, with its active-world script frame.
void *part_shadow_source(void *render, float *frame);
// Advance only cars belonging to the active world, at a fixed physics step.
void part_physics_advance(double seconds);
struct PartPhysicsState {
  bool active = false;
  double mass = 0, height = 0, vertical_speed = 0;
  uint64_t ticks = 0, backlog = 0;
  std::array<double, 4> length{}, force{};
  std::array<std::array<double, 3>, 4> wheel_center{};
  std::array<bool, 4> contact{};
  std::array<bool, 4> geometry_solved{};
};
bool part_physics_state(Object *car, PartPhysicsState &state);
std::vector<InstalledPart> part_installed_tree(Object *root);
Object *vehicle_spawn_stock(Jvm &jvm, RpkIndex &resources, Object *parent,
                            uint32_t chassis, uint32_t texture = 0);
} // namespace slrr
