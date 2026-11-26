#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace slrr {

// Tyre skid marks (SLRR_rev 4ca11c2 src/render/tire-effects.ts, marks only):
// a ring of ground quads in script world space, fed once per host frame from
// the published wheel state, never per solver step. Owned by the host thread.
struct SkidWheelSample {
  bool contact = false;
  double force = 0;                  // N, tyre load
  double slip_speed = 0;             // m/s, tread minus road speed
  double slip_ratio = 0;             // slip_speed / road speed
  double lateral_speed = 0;          // m/s across the tread
  std::array<double, 3> point{};     // contact patch
  std::array<double, 3> normal{0, 1, 0};
  std::array<double, 3> forward{0, 0, 1};
  uint32_t mark_color = 0;           // the ground's mark (ARGB), 0 the road's (#452)
};

struct SkidMarkVertex {
  float x, y, z;
  float alpha;  // 0 no mark, 1 full mark colour
  uint32_t color = 0;  // ARGB of the ground's mark, 0 for the road mark (#452)
};

constexpr size_t kSkidMarkQuads = 1400;

// 0..1 scrub strength; 0 below the visual threshold (SLRR_rev tireScrub).
double skid_scrub(const SkidWheelSample &wheel);
// Lighter deposit at speed (SLRR_rev skidMarkOpacity).
double skid_mark_opacity(double speed);

// Advances one wheel of one car. `car` is any stable per-car key.
void skid_marks_update(uintptr_t car, size_t wheel, const SkidWheelSample &sample,
                       double dt, double vehicle_speed);
// Ends the wheel tracks of a car that is gone; its marks stay on the ground.
void skid_marks_forget(uintptr_t car);
void skid_marks_clear();

// Four vertices per quad (two triangles 0-1-2, 1-3-2), oldest first after the
// ring wraps. revision changes whenever a vertex does.
const std::vector<SkidMarkVertex> &skid_mark_vertices();
uint64_t skid_marks_revision();

}  // namespace slrr
