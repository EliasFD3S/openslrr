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
// One `body` line. Mass is kilograms; the pose is in the part's own frame.
struct PartBody {
  PartPose pose;
  float mass = 0;
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
  PartPose pose;
  uint32_t flags = 0;
  // -1: always drawn. 0 lo-res, 1 hi-res, 2 extr-hi-res. Alternatives, not
  // layers.
  int lod_level = -1;
  // A `render` line after `wheel` is that wheel's mesh. -1 is the body.
  int wheel = -1;
  // `colors N rgb...` on this render. The create string's seventh field
  // picks one: index modulo the count.
  std::vector<uint32_t> colors;
  std::vector<PartLink> links;
};
struct PartConfig {
  uint32_t resource = 0;
  std::string source;
  std::string native_kind;
  std::string name;
  int category = 0;
  float mass = 0;
  std::vector<PartBody> bodies;
  std::vector<PartWheelSpring> wheel_springs;
  uint32_t render = 0, mesh = 0, texture = 0;
  PartPose render_pose;
  PartPose steering;
  bool has_steering = false;
  std::vector<PartPose> wheels;
  float max_steer = .6f, ackermann_length = 0;
  std::vector<float> wheel_steering;
  std::vector<PartSlot> slots;
  std::vector<uint32_t> stock;
  std::vector<PartRender> renders;
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
} // namespace slrr
