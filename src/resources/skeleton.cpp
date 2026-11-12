#include "slrr/skeleton.hpp"
#include "slrr/format/skl.hpp"

#include "slrr/scx_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace slrr {
namespace {

void identity(float* m) {
  std::memset(m, 0, sizeof(float) * 16);
  m[0] = m[5] = m[10] = m[15] = 1.f;
}

void multiply(float* out, const float* a, const float* b) {
  float result[16]{};
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row)
      for (int k = 0; k < 4; ++k)
        result[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
  std::memcpy(out, result, sizeof(result));
}

void transform_point(const float* m, float x, float y, float z, float* out) {
  out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
  out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
  out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

void transform_vector(const float* m, float x, float y, float z, float* out) {
  out[0] = m[0] * x + m[4] * y + m[8] * z;
  out[1] = m[1] * x + m[5] * y + m[9] * z;
  out[2] = m[2] * x + m[6] * y + m[10] * z;
}

bool rigid_inverse(const float* m, float* inv) {
  const float c0[3] = {m[0], m[1], m[2]};
  const float c1[3] = {m[4], m[5], m[6]};
  const float c2[3] = {m[8], m[9], m[10]};
  auto dot = [](const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  auto near_one = [](float length) { return std::fabs(length - 1.f) < 1e-2f; };
  if (!near_one(std::sqrt(dot(c0, c0))) || !near_one(std::sqrt(dot(c1, c1))) ||
      !near_one(std::sqrt(dot(c2, c2))) || std::fabs(dot(c0, c1)) > 1e-2f ||
      std::fabs(dot(c0, c2)) > 1e-2f || std::fabs(dot(c1, c2)) > 1e-2f)
    return false;
  identity(inv);
  inv[0] = m[0]; inv[1] = m[4]; inv[2] = m[8];
  inv[4] = m[1]; inv[5] = m[5]; inv[6] = m[9];
  inv[8] = m[2]; inv[9] = m[6]; inv[10] = m[10];
  const float t[3] = {m[12], m[13], m[14]};
  inv[12] = -(inv[0] * t[0] + inv[4] * t[1] + inv[8] * t[2]);
  inv[13] = -(inv[1] * t[0] + inv[5] * t[1] + inv[9] * t[2]);
  inv[14] = -(inv[2] * t[0] + inv[6] * t[1] + inv[10] * t[2]);
  return true;
}

bool invert4(const float* m, float* inv) {
  float row[4][8]{};
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) row[r][c] = m[c * 4 + r];
  for (int i = 0; i < 4; ++i) row[i][4 + i] = 1.f;
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r)
      if (std::fabs(row[r][col]) > std::fabs(row[pivot][col])) pivot = r;
    if (std::fabs(row[pivot][col]) < 1e-8f) return false;
    if (pivot != col)
      for (int k = 0; k < 8; ++k) std::swap(row[col][k], row[pivot][k]);
    const float divisor = row[col][col];
    for (int k = 0; k < 8; ++k) row[col][k] /= divisor;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const float factor = row[r][col];
      for (int k = 0; k < 8; ++k) row[r][k] -= factor * row[col][k];
    }
  }
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) inv[c * 4 + r] = row[r][4 + c];
  return true;
}

void invert_bind(const float* world, float* inv) {
  if (rigid_inverse(world, inv)) return;
  if (invert4(world, inv)) return;
  identity(inv);
}

void matrix_from_rows(float* m, const float rotation[9], float x, float y, float z) {
  // File rows become the matrix columns, matching the SLRR_rev bind.
  identity(m);
  m[0] = rotation[0]; m[1] = rotation[1]; m[2] = rotation[2];
  m[4] = rotation[3]; m[5] = rotation[4]; m[6] = rotation[5];
  m[8] = rotation[6]; m[9] = rotation[7]; m[10] = rotation[8];
  m[12] = x;
  m[13] = y;
  m[14] = z;
}

void quat_to_matrix(float* m, float x, float y, float z, float w) {
  const float xx = x * x, yy = y * y, zz = z * z;
  const float xy = x * y, xz = x * z, yz = y * z;
  const float wx = w * x, wy = w * y, wz = w * z;
  identity(m);
  m[0] = 1.f - 2.f * (yy + zz);
  m[1] = 2.f * (xy + wz);
  m[2] = 2.f * (xz - wy);
  m[4] = 2.f * (xy - wz);
  m[5] = 1.f - 2.f * (xx + zz);
  m[6] = 2.f * (yz + wx);
  m[8] = 2.f * (xz + wy);
  m[9] = 2.f * (yz - wx);
  m[10] = 1.f - 2.f * (xx + yy);
}

void quat_normalize(float* q) {
  const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (length < 1e-8f) {
    q[0] = q[1] = q[2] = 0.f;
    q[3] = 1.f;
    return;
  }
  for (int i = 0; i < 4; ++i) q[i] /= length;
}

void quat_slerp(const float* a, const float* b, float t, float* out) {
  float to[4] = {b[0], b[1], b[2], b[3]};
  float dot = a[0] * to[0] + a[1] * to[1] + a[2] * to[2] + a[3] * to[3];
  if (dot < 0.f) {
    for (float& value : to) value = -value;
    dot = -dot;
  }
  if (dot > 0.9995f) {
    for (int i = 0; i < 4; ++i) out[i] = a[i] + (to[i] - a[i]) * t;
    quat_normalize(out);
    return;
  }
  const float theta = std::acos(std::clamp(dot, -1.f, 1.f));
  const float sine = std::sin(theta);
  const float w0 = std::sin((1.f - t) * theta) / sine;
  const float w1 = std::sin(t * theta) / sine;
  for (int i = 0; i < 4; ++i) out[i] = a[i] * w0 + to[i] * w1;
}

void matrix_to_quat(const float* m, float* q) {
  const float trace = m[0] + m[5] + m[10];
  if (trace > 0.f) {
    const float s = std::sqrt(trace + 1.f) * 2.f;
    q[3] = 0.25f * s;
    q[0] = (m[6] - m[9]) / s;
    q[1] = (m[8] - m[2]) / s;
    q[2] = (m[1] - m[4]) / s;
  } else if (m[0] > m[5] && m[0] > m[10]) {
    const float s = std::sqrt(1.f + m[0] - m[5] - m[10]) * 2.f;
    q[3] = (m[6] - m[9]) / s;
    q[0] = 0.25f * s;
    q[1] = (m[1] + m[4]) / s;
    q[2] = (m[8] + m[2]) / s;
  } else if (m[5] > m[10]) {
    const float s = std::sqrt(1.f + m[5] - m[0] - m[10]) * 2.f;
    q[3] = (m[8] - m[2]) / s;
    q[0] = (m[1] + m[4]) / s;
    q[1] = 0.25f * s;
    q[2] = (m[6] + m[9]) / s;
  } else {
    const float s = std::sqrt(1.f + m[10] - m[0] - m[5]) * 2.f;
    q[3] = (m[1] - m[4]) / s;
    q[0] = (m[8] + m[2]) / s;
    q[1] = (m[6] + m[9]) / s;
    q[2] = 0.25f * s;
  }
  quat_normalize(q);
}

const ClipTrack* find_track(const Clip& clip, const std::string& name) {
  for (const ClipTrack& track : clip.tracks)
    if (track.name == name) return &track;
  return nullptr;
}

const SkeletonBone* find_bone(const Skeleton& skeleton, const std::string& name) {
  for (const SkeletonBone& bone : skeleton.bones)
    if (bone.name == name) return &bone;
  return nullptr;
}

void sample_track(const Clip& clip, const ClipTrack& track, float time,
                  float* rotation, float* direction) {
  const int keys = static_cast<int>(clip.times.size());
  if (keys <= 1 || time <= clip.times.front()) {
    std::memcpy(rotation, track.rotation.data(), sizeof(float) * 4);
    std::memcpy(direction, track.direction.data(), sizeof(float) * 3);
    return;
  }
  if (time >= clip.times.back()) {
    const int last = keys - 1;
    std::memcpy(rotation, track.rotation.data() + last * 4, sizeof(float) * 4);
    std::memcpy(direction, track.direction.data() + last * 3, sizeof(float) * 3);
    return;
  }
  int upper = 1;
  while (upper < keys && clip.times[static_cast<size_t>(upper)] < time) ++upper;
  const int lower = upper - 1;
  const float span = clip.times[static_cast<size_t>(upper)] -
                     clip.times[static_cast<size_t>(lower)];
  const float blend = span > 0.f
                          ? (time - clip.times[static_cast<size_t>(lower)]) / span
                          : 0.f;
  quat_slerp(track.rotation.data() + lower * 4, track.rotation.data() + upper * 4,
             blend, rotation);
  const float* from = track.direction.data() + lower * 3;
  const float* to = track.direction.data() + upper * 3;
  for (int i = 0; i < 3; ++i) direction[i] = from[i] + (to[i] - from[i]) * blend;
}

}  // namespace

bool skeleton_parse(std::string_view text, Skeleton* out, std::string* error) {
  if (!out || !skl_read(text, out, error)) return false;
  skeleton_prepare_bind(*out);
  return true;
}

void skeleton_prepare_bind(Skeleton& skeleton) {
  const size_t count = skeleton.bones.size();
  std::vector<float> locals(count * 16), worlds(count * 16);
  skeleton_local_bind(skeleton, locals.data());
  skeleton_world(skeleton, locals.data(), worlds.data());
  skeleton.inverse_bind.resize(count * 16);
  for (size_t i = 0; i < count; ++i)
    invert_bind(worlds.data() + i * 16, skeleton.inverse_bind.data() + i * 16);
}

void skeleton_local_bind(const Skeleton& skeleton, float* locals) {
  if (!locals) return;
  for (size_t i = 0; i < skeleton.bones.size(); ++i) {
    const SkeletonBone& bone = skeleton.bones[i];
    matrix_from_rows(locals + i * 16, bone.rotation, bone.position[0],
                     bone.position[1], bone.position[2]);
  }
}

void skeleton_world(const Skeleton& skeleton, const float* locals, float* worlds) {
  if (!locals || !worlds) return;
  const std::vector<int>& order = skeleton.order;
  if (order.empty()) {
    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
      const int parent = skeleton.bones[i].parent;
      if (parent < 0 || static_cast<size_t>(parent) >= skeleton.bones.size())
        std::memcpy(worlds + i * 16, locals + i * 16, sizeof(float) * 16);
      else
        multiply(worlds + i * 16, worlds + static_cast<size_t>(parent) * 16,
                 locals + i * 16);
    }
    return;
  }
  for (int index : order) {
    const size_t i = static_cast<size_t>(index);
    const int parent = skeleton.bones[i].parent;
    if (parent < 0)
      std::memcpy(worlds + i * 16, locals + i * 16, sizeof(float) * 16);
    else
      multiply(worlds + i * 16, worlds + static_cast<size_t>(parent) * 16,
               locals + i * 16);
  }
}

bool clip_sample(const Skeleton& bind, const Skeleton* lengths, const Clip& clip,
                 float time, float* locals) {
  if (!locals || clip.times.empty()) return false;
  for (size_t i = 0; i < bind.bones.size(); ++i) {
    const ClipTrack* track = find_track(clip, bind.bones[i].name);
    if (!track || track->rotation.size() < 4 || track->direction.size() < 3) continue;
    float rotation[4], direction[3];
    sample_track(clip, *track, time, rotation, direction);
    const SkeletonBone* donor = lengths ? find_bone(*lengths, bind.bones[i].name) : nullptr;
    if (!donor) donor = &bind.bones[i];
    const float* position = donor->position;
    const float length = std::sqrt(position[0] * position[0] + position[1] * position[1] +
                                   position[2] * position[2]);
    quat_to_matrix(locals + i * 16, rotation[0], rotation[1], rotation[2], rotation[3]);
    float* local = locals + i * 16;
    local[12] = direction[0] * length;
    local[13] = direction[1] * length;
    local[14] = direction[2] * length;
  }
  return true;
}

void skeleton_skin(const ScxGeometry& bind, const Skeleton& skeleton,
                   const float* worlds, std::vector<float>* positions,
                   std::vector<float>* normals) {
  if (!positions || !normals) return;
  const size_t count = bind.positions.size() / 3;
  if (positions->size() != bind.positions.size()) *positions = bind.positions;
  if (normals->size() != bind.normals.size()) *normals = bind.normals;
  if (!worlds || skeleton.inverse_bind.size() < skeleton.bones.size() * 16 ||
      bind.skin_weights.size() < count * 4 || bind.skin_bones.size() < count * 4) {
    *positions = bind.positions;
    if (bind.normals.size() == normals->size()) *normals = bind.normals;
    return;
  }
  std::vector<float> skin(skeleton.bones.size() * 16);
  for (size_t i = 0; i < skeleton.bones.size(); ++i)
    multiply(skin.data() + i * 16, worlds + i * 16,
             skeleton.inverse_bind.data() + i * 16);
  for (size_t v = 0; v < count; ++v) {
    float moved[3] = {}, turned[3] = {}, sum = 0.f;
    const float* source = bind.positions.data() + v * 3;
    const bool has_normal = bind.normals.size() >= (v + 1) * 3;
    const float* normal = has_normal ? bind.normals.data() + v * 3 : nullptr;
    for (int k = 0; k < 4; ++k) {
      const float weight = bind.skin_weights[v * 4 + static_cast<size_t>(k)];
      const uint16_t bone = bind.skin_bones[v * 4 + static_cast<size_t>(k)];
      if (!(weight > 0.f) || bone >= skeleton.bones.size()) continue;
      sum += weight;
      const float* matrix = skin.data() + static_cast<size_t>(bone) * 16;
      float point[3], vector[3] = {};
      transform_point(matrix, source[0], source[1], source[2], point);
      if (normal) transform_vector(matrix, normal[0], normal[1], normal[2], vector);
      for (int axis = 0; axis < 3; ++axis) {
        moved[axis] += weight * point[axis];
        turned[axis] += weight * vector[axis];
      }
    }
    if (!(sum > 1e-6f)) {
      for (int axis = 0; axis < 3; ++axis)
        (*positions)[v * 3 + static_cast<size_t>(axis)] = source[axis];
      if (has_normal && normals->size() >= (v + 1) * 3)
        for (int axis = 0; axis < 3; ++axis)
          (*normals)[v * 3 + static_cast<size_t>(axis)] = normal[axis];
      continue;
    }
    const float scale = 1.f / sum;
    for (int axis = 0; axis < 3; ++axis)
      (*positions)[v * 3 + static_cast<size_t>(axis)] = moved[axis] * scale;
    if (!has_normal || normals->size() < (v + 1) * 3) continue;
    const float length = std::sqrt(turned[0] * turned[0] + turned[1] * turned[1] +
                                   turned[2] * turned[2]);
    if (length < 1e-8f) continue;
    for (int axis = 0; axis < 3; ++axis)
      (*normals)[v * 3 + static_cast<size_t>(axis)] = turned[axis] * scale / length;
  }
}

}  // namespace slrr
