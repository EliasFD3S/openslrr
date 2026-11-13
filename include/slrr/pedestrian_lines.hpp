#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace slrr {

// One body already using a spot. SLRR_rev rejects a spawn inside
// radius + 1.5 m, ignoring a body more than 4 m above or below.
struct PedestrianBlock {
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
  float radius = 2.f;
  float vx = 0.f;
  float vy = 0.f;
  float vz = 0.f;
};

// A moving vehicle the walkers get out of the way of. The original turns
// every dynamic object into {position, direction, radius 3, speed}.
struct PedestrianCar {
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
  float vx = 0.f;
  float vz = 0.f;
};

// One walker. Like the original (0x00589cb0), it is a free agent inside a
// city.walk row: the row's walls and both sidewalk runs push it back, and it
// leaves the row only through a gate into the neighbouring row.
struct PedestrianWalker {
  int row = -1;
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
  // Unit heading on the ground plane. Only walls and runs turn it.
  float hx = 0.f;
  float hz = 1.f;
  // Ground velocity of the last step, metres per second.
  float vx = 0.f;
  float vz = 0.f;
  float speed = 0.f;
  // Facing, turned toward the direction of travel at pi radians per metre.
  float yaw = 0.f;
  // Walk-clip rate: the original advances the clip by the distance moved
  // plus half the turn (0x00587820), here per second.
  float gait = 0.f;
  bool fleeing = false;
  bool ok = false;
};

class PedestrianLines {
 public:
  PedestrianLines();
  ~PedestrianLines();
  PedestrianLines(const PedestrianLines&) = delete;
  PedestrianLines& operator=(const PedestrianLines&) = delete;

  bool load(const std::string& text);
  // Rows walkers can stand in.
  int rows() const;
  // Walkable area, in square metres, of the rows whose centre lies inside
  // the ring. The original density is people per square metre of that area.
  float area_within(float x, float y, float z, float radius) const;
  // Place a walker in a row near the focus with the original spawn rule.
  // Without `hidden`, min_distance applies to every candidate. With it, the
  // SLRR_rev rule: a hidden point is valid from 12 m, a visible one from
  // 35 m, and a hidden candidate is preferred.
  bool spawn(float x, float y, float z, float radius, float min_distance,
             uint32_t& rng, PedestrianWalker& out, const PedestrianBlock* blocked = nullptr,
             int blocked_count = 0,
             bool (*hidden)(float x, float y, float z, void* user) = nullptr,
             void* hidden_user = nullptr) const;
  // Advance every walker together, so they avoid each other. A walker that
  // leaves through a gate with no row behind it is retired (ok = false).
  void step(std::vector<PedestrianWalker>& walkers, float dt, const PedestrianCar* cars,
            int car_count) const;
  // True when (x, z) at height y lies inside a row, kept clear of its edges by
  // `clearance` metres. Used by checks and diagnostics.
  bool inside(float x, float y, float z, float clearance) const;

 private:
  struct Data;
  Data* data_;
};

}  // namespace slrr
