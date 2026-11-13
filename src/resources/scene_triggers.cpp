#include "slrr/scene_triggers.hpp"

#include "slrr/gameref.hpp"
#include "slrr/jvm.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/script_events.hpp"

#include <array>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slrr {
namespace {

constexpr int kEventTriggerOn = 0x20;
constexpr int kEventTriggerOff = 0x40;
// A car that moves further than this between two ticks was placed by
// setMatrix, not driven: its trigger states are re-read without events.
constexpr float kTeleportDistance = 25.f;

struct Contact {
  TriggerContact state;
  uint64_t seen = 0;
};
// Keyed by trigger and car live ids, so a freed and reallocated object
// cannot inherit an old state.
std::map<std::pair<int32_t, int32_t>, Contact> g_contacts;
std::unordered_map<int32_t, std::array<float, 3>> g_last_position;
uint64_t g_tick = 0;

}  // namespace

void scene_triggers_reset() {
  g_contacts.clear();
  g_last_position.clear();
}

void scene_triggers_tick(Jvm& jvm) {
  const auto triggers = gameref_trigger_keys();
  if (triggers.empty()) {
    scene_triggers_reset();
    return;
  }
  const auto vehicles = part_vehicle_positions();
  ++g_tick;
  std::unordered_map<int32_t, bool> teleported;
  for (const auto& vehicle : vehicles) {
    const std::array<float, 3> now = {vehicle.position[0], vehicle.position[1],
                                      vehicle.position[2]};
    const auto last = g_last_position.find(vehicle.id);
    teleported[vehicle.id] =
        last != g_last_position.end() &&
        std::hypot(now[0] - last->second[0], now[1] - last->second[1],
                   now[2] - last->second[2]) > kTeleportDistance;
    g_last_position[vehicle.id] = now;
  }
  struct Event {
    Object* trigger;
    int32_t car;
    bool inside;
  };
  std::vector<Event> events;
  for (void* key : triggers) {
    const GameRefState* trigger = gameref_get(key);
    if (!trigger || !trigger->live_id) continue;
    PartMatrix world;
    if (!scene_instances_script_matrix(key, world.data())) continue;
    const auto inverse = part_matrix_inverse_rigid(world);
    for (const auto& vehicle : vehicles) {
      const float* p = vehicle.position;
      float local[3];
      for (int axis = 0; axis < 3; ++axis)
        local[axis] = inverse[axis] * p[0] + inverse[4 + axis] * p[1] +
                      inverse[8 + axis] * p[2] + inverse[12 + axis];
      const bool inside =
          trigger_volume_contains(trigger->trigger_shape, trigger->trigger_size, local);
      auto& contact = g_contacts[{trigger->live_id, vehicle.id}];
      contact.seen = g_tick;
      const auto edge = trigger_contact_update(contact.state, inside, teleported[vehicle.id]);
      if (edge != TriggerEdge::None)
        events.push_back({static_cast<Object*>(key), vehicle.id, edge == TriggerEdge::Enter});
    }
  }
  std::erase_if(g_contacts, [](const auto& entry) { return entry.second.seen != g_tick; });
  for (auto it = g_last_position.begin(); it != g_last_position.end();) {
    if (!teleported.contains(it->first)) it = g_last_position.erase(it);
    else ++it;
  }
  if (events.empty()) return;
  Jvm::RootScope roots(jvm, [&](const GcRootVisitor& visit) {
    for (const auto& event : events) visit(event.trigger);
  });
  for (const auto& event : events) {
    // A handler may destroy a later trigger; skip it once it is gone.
    if (!gameref_get(event.trigger)) continue;
    Object* param = jvm.new_string(std::to_string(event.car));
    script_event_emit(jvm, event.trigger, nullptr,
                      event.inside ? kEventTriggerOn : kEventTriggerOff,
                      Value::make_obj(param));
  }
}

}  // namespace slrr
