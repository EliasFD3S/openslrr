#pragma once

#include "slrr/format/matrixdata.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace slrr {

struct ScxGeometry;
class RpkIndex;

// BON and SKL share this text record. Positions stay in file centimetres.
// The render's declared SKL is the skin bind. A clip's sibling BON only
// supplies the bone length used to scale a matrixdata direction.
struct SkeletonBone {
  std::string name;
  int parent = -1;
  float rotation[9]{};
  float position[3]{};
  float limit_min[3]{};
  float limit_max[3]{};
};

struct Skeleton {
  std::vector<SkeletonBone> bones;
  std::vector<int> order;
  // Inverse of the bind-pose world matrix, column-major, one bone per 16.
  std::vector<float> inverse_bind;
};

bool skeleton_parse(std::string_view text, Skeleton* out, std::string* error);
// Inverse of each bind-pose world matrix. Not stored in the .skl or .bon.
void skeleton_prepare_bind(Skeleton& skeleton);

inline bool clip_parse(const uint8_t* data, size_t size, Clip* out, std::string* error) {
  return matrixdata_read(data, size, out, error);
}

// Column-major 4x4 locals and worlds, centimetres, one bone per 16 floats.
void skeleton_local_bind(const Skeleton& skeleton, float* locals);
void skeleton_world(const Skeleton& skeleton, const float* locals, float* worlds);

// Replaces the local matrix of each bone that has a track. Other bones are
// left as the caller wrote them. Translation is direction * bone length,
// in centimetres. Rotations are absolute local quaternions.
bool clip_sample(const Skeleton& bind, const Skeleton* lengths, const Clip& clip,
                 float time, float* locals);

// Writes posed positions and normals in centimetres. Weights are renormalized.
void skeleton_skin(const ScxGeometry& bind, const Skeleton& skeleton,
                   const float* worlds, std::vector<float>* positions,
                   std::vector<float>* normals);

// Per-render pose. Keys are the script objects, not GameRefState addresses.
void character_forget(void* key);
void character_forget_all();

// Posed mesh while a clip or a bone override is active. Null keeps the
// cached bind mesh.
std::shared_ptr<const ScxGeometry> character_posed(void* key, int32_t type_id,
                                                   const ScxGeometry* bind,
                                                   const RpkIndex& packs);

// Skeleton index, or 0 when the render has no matching bone. Never negative.
int character_bone_id(void* key, int32_t type_id, const RpkIndex& packs,
                      const std::string& name);
// Same lookup, or -1 when the name is absent. Host code that must not alias
// bone 0 uses this. The Java native stays on character_bone_id.
int character_find_bone(void* key, int32_t type_id, const RpkIndex& packs,
                        const std::string& name);

// One rotation of `bone` so `end` moves toward `target_cm` (character-local
// centimetres). The joint stays put. `max_radians` caps this step.
bool character_aim_bone(void* key, int32_t type_id, const RpkIndex& packs,
                        int bone, int end, const float target_cm[3], float max_radians);
// Rotate `bone` so its local axis (0 = X, 1 = Y, 2 = Z) points along
// `direction`. Twist around that axis stays. Character-local centimetres.
bool character_align_axis(void* key, int32_t type_id, const RpkIndex& packs,
                          int bone, int axis, const float direction[3]);
// Analytic two-bone reach. `pole` is a direction in character space, used so
// the elbow or knee bends to a stable side. Lengths are not stretched.
bool character_solve_limb(void* key, int32_t type_id, const RpkIndex& packs,
                          int upper, int lower, int end, const float target_cm[3],
                          const float pole[3]);
// Drop bone overrides so the next aim starts from the bind pose. A limb solved
// from last frame's result shrinks, because the measured bone length drifts.
void character_reset_pose(void* key);

// Script metres are stored as centimetres on the bone local matrix.
void character_set_bone_local(void* key, int32_t type_id, const RpkIndex& packs,
                              int bone, float x_m, float y_m, float z_m,
                              float yaw, float pitch, float roll);
void character_link_bone(void* child, void* parent, int bone);
void character_unlink(void* child);
bool character_linked_bone(void* child, void** parent, int* bone);
// Bone model matrix in metres, for a render parented to that bone.
bool character_bone_metres(void* owner, int bone, float out[16]);

void character_bind_animation(void* animation, void* render, int32_t clip_id,
                              const RpkIndex& packs);
void character_release_animation(void* animation);
void character_set_speed(void* animation, float speed);
void character_set_fade(void* animation, float fade);
void character_play(void* animation, bool loop);
void character_pause(void* animation);
bool character_seek(void* animation, float seconds);
float character_position(void* animation);
float character_duration(void* animation);

}  // namespace slrr
