#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace slrr {
class RpkIndex;
using PartMatrix = std::array<float, 16>;
struct PartPose {
  std::array<float, 3> position{};
  std::array<float, 3> rotation{}; // yaw, pitch, roll in radians
};
enum class PartBodyShape : uint8_t {
  None,
  Box,
  Sphere,
  Mesh,
};
// One `body` line. Mass is kilograms; the pose is in the part's own frame.
// Box values are half extents and the sphere value is a radius, all in metres.
struct PartBody {
  PartPose pose;
  float mass = 0;
  PartBodyShape shape = PartBodyShape::None;
  std::array<float, 3> box_half{};
  float sphere_radius = 0.f;
  bool collision = true;
  std::string collision_mesh;
};
// `spring base travel damping` consumed by the next `wheel`. Missing lines
// keep the stock fallback (0.29, 0.10, 0.05). Radius comes from `type sphere`.
struct PartWheelSpring {
  float base = 0.29f;
  float travel = 0.1f;
  float damping = 0.05f;
  float radius = 0.f;
  // `type <kg> sphere R`. Zero keeps the massless ray spring.
  float mass = 0.f;
  bool authored = false;
};
// `wing type x y z nx ny nz area suction exponent`: one aerodynamic panel in
// the part's frame. As the MWM EXE's CFG handler (0x4443C0) does, the normal
// is stored normalised, the area and suction factor as magnitudes, and a
// missing exponent is 1. Only type 4 occurs in the stock CFGs.
struct PartWing {
  int type = 0;
  std::array<float, 3> position{};
  std::array<float, 3> normal{};
  float area = 0.f;
  float suction = 0.f;
  float exponent = 1.f;
};
struct PartSlotLink {
  uint32_t resource = 0;
  int slot = 0;
};
struct PartSlot {
  int id = 0;
  int type = 0;
  PartPose pose;
  std::vector<PartSlotLink> attach;
  std::vector<PartSlotLink> compatible;
  // Bare 11-float line after the slot: pivot, axis, direction, closed, open.
  bool has_flap = false;
  float flap_pivot[3] = {};
  float flap_axis[3] = {};
  float flap_dir[3] = {};
  float flap_lo = 0.f;
  float flap_hi = 0.f;
};
// `linked a [b] [parentSlot]`. b == 0 is one anchor. The third value, when
// present, keeps the rule only for that parent slot. First match wins.
struct PartLink {
  int first = 0;
  int second = 0;
  int parent_slot = 0;
  bool filter = false;
};
struct PartRender {
  uint32_t resource = 0;
  std::vector<uint32_t> variants;
  PartPose pose;
  uint32_t flags = 0;
  // -1: always drawn. 0 lo-res, 1 hi-res, 2 extr-hi-res. Alternatives, not
  // layers.
  int lod_level = -1;
  int lod_last = -1;
  // A `render` line after `wheel` is that wheel's mesh. -1 is the body.
  int wheel = -1;
  // `colors N rgb...` on this render. The create string's seventh field
  // picks one: index modulo the count.
  std::vector<uint32_t> colors;
  std::vector<PartLink> links;
};
enum class HudSlot : size_t {
  Gauge, RpmPin, SpeedPin, Rpm, Speed, Gear, Mileage, Handbrake,
  Damage, Nitro, Turbo,
  // The ABS and ASR lamps (#446): build 606's `osd_abs` / `osd_asr`.
  Abs, Asr, Count
};
struct HudElement {
  bool defined = false;
  uint32_t resource = 0;
  std::array<float, 3> position{};
  std::array<float, 4> needle{}; // offset, radians per unit, lower/upper bounds
  uint32_t color = 0xffffffffu;
  std::string format;
};
using VehicleHudLayout = std::array<HudElement, static_cast<size_t>(HudSlot::Count)>;
// Chassis `cockpit_rpm` / `cockpit_speed` (#301): a 3D pin on the dashboard,
// distinct from the overlay `osd_rpmpin` / `osd_spdpin`. MWM build 606 reads
// `x y z yaw pitch roll render offset factor low high`
// (0x00444B60 rpm, 0x00444CB0 speed) and draws the pin at
// (yaw, pitch, roll + angle), angle = value * factor + offset limited to
// [low, high] (0x0043C0A0). Values: |engine rad/s| * 9.5493 rpm and
// |velocity| * 3.6 km/h. Absent fields keep the build-606 defaults.
enum class CockpitGauge : size_t { Rpm, Speed, Count };
struct CockpitNeedle {
  bool defined = false;
  uint32_t resource = 0;
  PartPose pose;
  float offset = 2.2f, factor = 0.f, low = -2.2f, high = 2.2f;
};
using CockpitNeedles =
    std::array<CockpitNeedle, static_cast<size_t>(CockpitGauge::Count)>;
// Below `low` gives `low`, otherwise above `high` gives `high`, in that
// order (no swap when low > high); a non-finite value reads as 0.
float cockpit_needle_angle(const CockpitNeedle &needle, double value);
struct PartConfig {
  uint32_t resource = 0;
  std::string source;
  std::string native_kind;
  std::string name;
  int category = 0;
  float mass = 0;
  std::vector<PartBody> bodies;
  // At most 16, the EXE's per-part capacity.
  std::vector<PartWing> wings;
  std::vector<PartWheelSpring> wheel_springs;
  uint32_t render = 0, mesh = 0, texture = 0;
  PartPose render_pose;
  PartPose steering;
  bool has_steering = false;
  // Driver locators. `seat` is the pose Vin is parented to. `pedals` is a
  // point (no orientation): the feet aim there. Neither is a separate mesh.
  PartPose seat;
  bool has_seat = false;
  float pedals[3] = {};
  bool has_pedals = false;
  // Gear lever knob, chassis-local metres (`shifter x y z`). Build 606
  // stores it at car+0x9AC and keeps it at the origin when the cfg has no
  // line, which is the case for every stock racer.
  float shifter[3] = {};
  std::vector<PartPose> wheels;
  float max_steer = .6f, ackermann_length = 0;
  std::vector<PartPose> cameras;
  float brake_power = 1000.f, handbrake_power = 2000.f;
  float brake_inertia = 50.f;
  std::vector<float> wheel_steering;
  std::vector<PartSlot> slots;
  std::vector<uint32_t> stock;
  // `sfx_loop <sound>` of a native car: the sound resource of its radio loop.
  uint32_t sfx_loop = 0;
  std::vector<PartRender> renders;
  // `lods N s0 s1 ...` (map objects): the size ratios at which the object
  // switches to LOD level 1, 2, ... (ascending). Renders choose their levels
  // with `lod first last`. Empty when the cfg has no `lods` line.
  std::vector<float> lod_sizes;
  VehicleHudLayout hud{};
  CockpitNeedles cockpit{};
  std::vector<std::string> unknown;
  std::vector<std::string> errors;
  bool valid = false;
};

// Resource references are qualified against their owning RPK, never against
// the caller's pack. CFG poses are meters, independently of SCX centimeters.
PartConfig parse_part_config(const std::string &source,
                             const RpkIndex *resources = nullptr,
                             int owner_pack = 0);
PartConfig load_part_config(const RpkIndex &resources, uint32_t resource);
PartMatrix part_pose_matrix(const PartPose &pose);
PartMatrix part_matrix_multiply(const PartMatrix &a, const PartMatrix &b);
PartMatrix part_matrix_inverse_rigid(const PartMatrix &matrix);
bool part_slot_matrix(const PartConfig &config, const PartSlot &slot,
                      PartMatrix &matrix);
// Parked ride length: rest minus the static sag, clamped to the Java limits.
// `force` is the spring rate in N/m. Returns false when the spring is unset.
bool part_parked_spring_length(float rest, float minimum, float maximum,
                               float force, float mass_on_wheel, float &length);
// How the original Invictus Aero calculator authored a `wing` line: a wing
// (airfoil) face or a body surface, and the face's flattened area in m².
// Its SLRR method stores 30× that area for surfaces and 90× for wings
// (exponent 0.667); its CRC and Experimental methods store about 1.27× and
// write wings as type 5.
bool part_wing_is_airfoil(const PartWing &wing);
float part_wing_area(const PartWing &wing);
} // namespace slrr
