#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slrr {

// Tyre skid marks (SLRR_rev 4ca11c2 src/render/tire-effects.ts, marks only):
// a ring of ground quads in script world space, fed once per host frame from
// the published wheel state, never per solver step. Owned by the host thread.

// A ground material's `skid_SM` (#454): an RPK `skidmark <cfg>` entry and
// its texture. The cfg (`particles/scripts/skids/sliding/*.cfg`,
// `MW_Mod/scripts/Skid_Grass.cfg`) gives `maxcount`, `width min max`,
// `height` (metres of track per texture tile), `uv_end` and `uv_normal`
// (u0 v0 u1 v1: the cap that starts and ends a mark, and the repeated
// piece) and `fade` (metres of alpha ramp at each end).
struct SkidMarkStyle {
  std::string texture;  // absolute path; empty draws the plain road mark
  int maxcount = 100;
  float width_min = .15f, width_max = .205f;  // SLRR_rev's untextured strip
  float height = .28f;
  std::array<float, 4> uv_end{0, 0, 1, 1}, uv_normal{0, 0, 1, 1};
  float fade = .45f;
};
SkidMarkStyle parse_skid_mark_config(std::string_view text);
// Styles by RPK id; index 0 is the untextured default.
uint16_t skid_mark_style_register(uint32_t key, SkidMarkStyle style);
uint16_t skid_mark_style_find(uint32_t key);  // 0xffff when unknown
const std::vector<SkidMarkStyle> &skid_mark_styles();

struct SkidWheelSample {
  bool contact = false;
  double force = 0;                  // N, tyre load
  double slip_speed = 0;             // m/s, tread minus road speed
  double slip_ratio = 0;             // slip_speed / road speed
  double lateral_speed = 0;          // m/s across the tread
  std::array<double, 3> point{};     // contact patch
  std::array<double, 3> normal{0, 1, 0};
  std::array<double, 3> forward{0, 0, 1};
  uint32_t mark_color = 0;           // the ground's skid_SMcolor (ARGB), 0 none (#452)
  uint16_t style = 0;                // skid_mark_styles() index (#454)
  // The original's combined slip (tyre_combined_slip), or -1 for SLRR_rev's
  // scrub law. With it, a mark exists while sigma > 0.4, as in the exe.
  double sigma = -1;
  // Tyre load over its nominal load, at most 1: the exe's mark alpha factor.
  double load_fraction = 1;
};

// The exe's combined tyre slip (#454), the value its wheel keeps at +0x154:
// sqrt(sin(a)^2 + (cos(a) k scale)^2) of the slip state 0x43BBD0 updates
// (k longitudinal, a the slip angle), with that function's stock constants
// (0x60E02C-0x60E048: limits 1 and 1, relaxations 0.3 and 0.5, transition
// 0.1 above 0.1 m/s, damping 0.1 and 0.2). Its relaxed branch is a first-
// order filter of about 20 ms, so the host takes its steady state
// (slip / 15, lateral / 50) once per frame.
double tyre_combined_slip(double slip_speed, double lateral_speed, double road_speed,
                          double longitudinal_scale = 1);
// The exe's skid effect laws (0x453680, 0x451280, 0x451540): nothing below
// sigma 0.4; the wheel's particle system emits at its cfg rate times
// max(1, (sigma - 0.4) * 0.2), and the mark's alpha takes that same factor.
constexpr double kSkidEffectSlip = .4;
double tyre_effect_frequency(double sigma);
// The intensity the exe keeps beside them, min(sigma - 0.4, 1) (0 below).
double tyre_effect_intensity(double sigma);

struct SkidMarkVertex {
  float x, y, z;
  float alpha;  // 0 no mark, 1 full mark
  uint32_t color = 0;  // the ground's skid_SMcolor (ARGB), 0 none (#452)
  float u = 0, v = 0;  // texture coordinates (#454)
  uint16_t style = 0;
};

// Each style keeps its own ring of maxcount x 14 quads: the stock 100 gives
// SLRR_rev's 1400. A style draws in one batch of 16-bit indices, hence the
// per-style ceiling; all styles together stay under kSkidMarkMaxQuads.
constexpr size_t kSkidQuadsPerMaxcount = 14;
constexpr size_t kSkidMarkStyleQuads = 16000;
constexpr size_t kSkidMarkMaxQuads = 24000;
size_t skid_mark_capacity(const SkidMarkStyle &style);

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

// Four vertices per quad (two triangles 0-1-2, 1-3-2), in no particular
// order once a ring wraps. revision changes whenever a vertex does.
const std::vector<SkidMarkVertex> &skid_mark_vertices();
uint64_t skid_marks_revision();

}  // namespace slrr
