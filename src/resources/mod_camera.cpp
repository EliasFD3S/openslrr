#include "slrr/mod_camera.hpp"

#include <algorithm>
#include <cmath>

namespace slrr {
namespace {
bool finite3(const float* v) {
  return v && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross(const float* a, const float* b, float* out) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}
bool normalize(float* v) {
  const float length = std::sqrt(dot(v, v));
  if (!(length > 1e-6f)) return false;
  for (int i = 0; i < 3; ++i) v[i] /= length;
  return true;
}
// up minus its component along forward, normalized.
bool perpendicular(const float* forward, const float* up, float* out) {
  const float along = dot(up, forward);
  for (int i = 0; i < 3; ++i) out[i] = up[i] - forward[i] * along;
  return normalize(out);
}
}  // namespace

bool mod_camera_set(ModCameraPose& pose, ModCameraMode mode, const float eye[3],
                    const float at[3], const float up[3], float fov) {
  if (mode == ModCameraMode::None) {
    pose = {};
    return true;
  }
  if (!finite3(eye) || !finite3(at) || !finite3(up) || !std::isfinite(fov)) return false;
  const float d[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  if (!(dot(d, d) > 1e-10f)) return false;
  pose.mode = mode;
  std::copy_n(eye, 3, pose.eye);
  std::copy_n(at, 3, pose.at);
  std::copy_n(up, 3, pose.up);
  pose.fov = fov > 0.f ? std::clamp(fov, 1.f, 170.f) : 0.f;
  return true;
}

bool mod_camera_resolve(const ModCameraPose& pose, const float* car_origin, float eye[3],
                        float at[3], float up[3]) {
  if (pose.mode == ModCameraMode::None) return false;
  float base[3] = {};
  if (pose.mode == ModCameraMode::Relative) {
    if (!finite3(car_origin)) return false;
    std::copy_n(car_origin, 3, base);
  }
  for (int i = 0; i < 3; ++i) {
    eye[i] = base[i] + pose.eye[i];
    at[i] = base[i] + pose.at[i];
  }
  float forward[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  if (!normalize(forward)) return false;
  static const float kWorldUp[3] = {0.f, 1.f, 0.f};
  static const float kWorldBack[3] = {0.f, 0.f, -1.f};
  if (!perpendicular(forward, pose.up, up) && !perpendicular(forward, kWorldUp, up))
    perpendicular(forward, kWorldBack, up);
  return true;
}

void mod_matrix_ypr(const float m[16], float ypr[3]) {
  const double pitch = std::asin(std::clamp(-double(m[9]), -1.0, 1.0));
  ypr[1] = static_cast<float>(pitch);
  if (std::fabs(std::cos(pitch)) > 1e-6) {
    ypr[0] = static_cast<float>(std::atan2(m[8], m[10]));
    ypr[2] = static_cast<float>(std::atan2(m[1], m[5]));
  } else {
    ypr[0] = static_cast<float>(std::atan2(-m[2], m[0]));
    ypr[2] = 0.f;
  }
}

void mod_look_ypr(const float eye[3], const float at[3], float& yaw, float& pitch) {
  const float d[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  yaw = std::atan2(-d[0], -d[2]);
  pitch = std::atan2(d[1], std::hypot(d[0], d[2]));
}

}  // namespace slrr
