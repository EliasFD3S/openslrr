#pragma once

#include <array>
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

// One walker on a city.walk corridor. The step matches SLRR_rev
// WalkingPedestrian: advance along the paired boundaries, then cross a
// junction by its explicit gates.
struct PedestrianWalker {
  int zone = -1;
  float t = 0.5f;
  float lateral = 0.5f;
  int sign = 1;
  float speed = 0.f;
  float base_speed = 1.f;
  float flee_speed = 2.f;
  float cooldown = 0.f;
  bool fleeing = false;
  bool transitioning = false;
  int target = -1;
  int entry = 0;
  float tr_t = 0.f;
  std::vector<std::array<float, 3>> path;
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
  float yaw = 0.f;
  bool ok = false;
};

class PedestrianLines {
 public:
  PedestrianLines();
  ~PedestrianLines();
  PedestrianLines(const PedestrianLines&) = delete;
  PedestrianLines& operator=(const PedestrianLines&) = delete;

  bool load(const std::string& text);
  int zones() const;
  // Walkable area, in square metres, of the corridor stretches inside the
  // ring. The original density is people per square metre of that area.
  float area_within(float x, float y, float z, float radius) const;
  // Reservoir-sample a corridor point inside the ring. min_distance is 35
  // in the city (SLRR_rev hides on-screen spawns closer than that).
  bool spawn(float x, float y, float z, float radius, float min_distance,
             uint32_t& rng, PedestrianWalker& out, const PedestrianBlock* blocked = nullptr,
             int blocked_count = 0) const;
  void step(PedestrianWalker& walker, float dt, float car_x, float car_y,
            float car_z, uint32_t& rng) const;

 private:
  struct Data;
  Data* data_;
};

}  // namespace slrr
