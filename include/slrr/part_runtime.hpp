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
                    uint32_t resource, const std::string &alias);
bool part_install(Object *child, Object *root, int own_slot = 0,
                  int parent_slot = 0);
bool part_destroy(Object *part);
bool part_command(Object *part, const std::string &command);
// World height of the tyre contact under a car. The camera's `move` spring
// hangs on this point; `look` stays on the chassis origin.
bool part_car_ground(void *key, float *y);
std::vector<InstalledPart> part_installed_tree(Object *root);
Object *vehicle_spawn_stock(Jvm &jvm, RpkIndex &resources, Object *parent,
                            uint32_t chassis, uint32_t texture = 0);
} // namespace slrr
