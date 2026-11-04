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
};
struct PartRender {
  uint32_t resource = 0;
  PartPose pose;
  uint32_t flags = 0;
  bool lod = false;
};
struct PartConfig {
  uint32_t resource = 0;
  std::string source;
  std::string native_kind;
  std::string name;
  int category = 0;
  float mass = 0;
  uint32_t render = 0, mesh = 0, texture = 0;
  PartPose render_pose;
  PartPose steering;
  bool has_steering = false;
  std::vector<PartPose> wheels;
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
} // namespace slrr
