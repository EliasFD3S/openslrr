#pragma once
#include "slrr/road_routes.hpp"
#include <memory>
#include <string>
namespace slrr {
class Jvm;
class RpkIndex;
class SectionMachine;
struct Object;
void register_race_natives(Jvm &, RpkIndex &, SectionMachine &);
std::shared_ptr<const RoadRoutes> race_ground_roads(const RpkIndex &,
                                                    void *ground);
void race_forget(void *key);
void race_reset();
struct RaceDriveInput {
  float throttle = 0, steer = 0, brake = 1;
  bool start = false, reverse = false;
};
bool race_brain_command(const RpkIndex &, void *brain, const std::string &);
bool race_drive_input(Object *car, const RoadPoint &position, float yaw,
                      float speed, float dt, RaceDriveInput &);
void race_brain_forget(void *);
void race_brains_reset();
void race_set_traffic_mode(const RpkIndex &, Object *);
void register_traffic_natives(Jvm &, RpkIndex &, SectionMachine &);
// Motion is bounded independently of the pause/warp-aware signal clock.
// Omit clock_seconds for callers already supplying simulation steps.
void race_traffic_tick(Jvm &, float seconds, double clock_seconds = -1);
int race_signal_at(const RoadPoint &position);
uint64_t race_signal_revision();
void race_traffic_forget(void *);
void race_traffic_reset();
int32_t race_traffic_pointer(Object *);
// Canonical, incarnation-checked map retained by the traffic registration.
void *race_traffic_ground(Object *);
bool race_traffic_halted(void *ground, const RoadPoint &position,
                         Object *car = nullptr);
void race_halt_path(const RpkIndex &, void *, const RoadPoint &,
                    const RoadPoint &);
void race_halt_cross(const RpkIndex &, void *, const RoadPoint &, float);
void race_clear_halts(void *);
// Reuses the route queried by Java RaceSetup; a miss clears it.
std::optional<RoadRoute> race_query_route(const RpkIndex &, void *,
                                          const RoadPoint &, const RoadPoint &);
const RoadRoute *race_last_route(void *ground);
const RoadRoute *race_plot_route();
} // namespace slrr
