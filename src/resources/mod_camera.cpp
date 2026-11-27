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

bool mod_camera_resolve(const ModCameraPose& pose, const float* car_frame, float eye[3],
                        float at[3], float up[3]) {
  if (pose.mode == ModCameraMode::None) return false;
  float base[3] = {};
  float want_up[3];
  float level_up[3] = {0.f, 1.f, 0.f};
  std::copy_n(pose.up, 3, want_up);
  if (pose.mode == ModCameraMode::Relative || pose.mode == ModCameraMode::Local) {
    if (!car_frame || !finite3(car_frame + 12)) return false;
    std::copy_n(car_frame + 12, 3, base);
  }
  if (pose.mode == ModCameraMode::Local) {
    // The chassis axes (x right, y up, z back), without any scale.
    float axes[3][3];
    for (int a = 0; a < 3; ++a) {
      std::copy_n(car_frame + a * 4, 3, axes[a]);
      if (!finite3(axes[a]) || !normalize(axes[a])) return false;
    }
    const auto rotate = [&axes](const float* v, float* out) {
      for (int i = 0; i < 3; ++i) out[i] = axes[0][i] * v[0] + axes[1][i] * v[1] + axes[2][i] * v[2];
    };
    float e[3], a[3];
    rotate(pose.eye, e);
    rotate(pose.at, a);
    rotate(pose.up, want_up);
    std::copy_n(axes[1], 3, level_up);
    for (int i = 0; i < 3; ++i) {
      eye[i] = base[i] + e[i];
      at[i] = base[i] + a[i];
    }
  } else {
    for (int i = 0; i < 3; ++i) {
      eye[i] = base[i] + pose.eye[i];
      at[i] = base[i] + pose.at[i];
    }
  }
  float forward[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  if (!normalize(forward)) return false;
  static const float kWorldUp[3] = {0.f, 1.f, 0.f};
  static const float kWorldBack[3] = {0.f, 0.f, -1.f};
  if (!perpendicular(forward, want_up, up) && !perpendicular(forward, level_up, up) &&
      !perpendicular(forward, kWorldUp, up))
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

void mod_look_ypr(const float eye[3], const float at[3], float& yaw, float& pitch,
                  const float* up) {
  const float d[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  const float level = std::hypot(d[0], d[2]);
  pitch = std::atan2(d[1], level);
  if (up && finite3(up) && level <= 1e-5f * std::fabs(d[1]) &&
      std::hypot(up[0], up[2]) > 1e-6f) {
    // Gimbal: up = (sin y, 0, cos y) * sin p; sin p is +-1 here.
    const float s = d[1] < 0.f ? -1.f : 1.f;
    yaw = std::atan2(s * up[0], s * up[2]);
    return;
  }
  yaw = std::atan2(-d[0], -d[2]);
}

bool mod_roll_up(const float eye[3], const float at[3], float roll, float up[3]) {
  float forward[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  if (!normalize(forward) || !std::isfinite(roll)) return false;
  static const float kWorldUp[3] = {0.f, 1.f, 0.f};
  float right[3], level[3];
  cross(forward, kWorldUp, right);
  if (!normalize(right)) return false;
  cross(right, forward, level);
  const float c = std::cos(roll), s = std::sin(roll);
  for (int i = 0; i < 3; ++i) up[i] = level[i] * c + right[i] * s;
  return true;
}

bool mod_sweep_args_valid(const float* from, const float* to, float radius) {
  return finite3(from) && finite3(to) && std::isfinite(radius) && radius >= 0.f;
}

float mod_look_roll(const float eye[3], const float at[3], const float up[3]) {
  float forward[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  if (!normalize(forward) || !finite3(up)) return 0.f;
  static const float kWorldUp[3] = {0.f, 1.f, 0.f};
  float right[3], level[3];
  cross(forward, kWorldUp, right);
  if (!normalize(right)) return 0.f;
  cross(right, forward, level);
  const float s = dot(up, right), c = dot(up, level);
  if (!(s * s + c * c > 1e-12f)) return 0.f;
  return std::atan2(s, c);
}

}  // namespace slrr
