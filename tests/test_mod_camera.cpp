#include "standalone_test.hpp"

#include "slrr/mod_api.hpp"
#include "slrr/mod_camera.hpp"

#include <cmath>
#include <limits>

namespace {

using slrr::ModCameraMode;
using slrr::ModCameraPose;
using slrr::mod_camera_resolve;
using slrr::mod_camera_set;
using slrr::mod_look_ypr;
using slrr::mod_matrix_ypr;

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) < eps; }
bool near3(const float* a, float x, float y, float z, float eps = 1e-5f) {
  return near(a[0], x, eps) && near(a[1], y, eps) && near(a[2], z, eps);
}
float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

SLRR_TEST(api_version_is_one) { SLRR_REQUIRE(slrr::kModApiVersion == 1); }

// setPose is exact: no smoothing, the pose read back is the pose given.
SLRR_TEST(world_pose_is_exact) {
  ModCameraPose pose;
  const float eye[3] = {10.f, 2.f, -5.f}, at[3] = {10.f, 1.f, -15.f}, up[3] = {0.f, 1.f, 0.f};
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::World, eye, at, up, 55.f));
  float e[3], a[3], u[3];
  SLRR_REQUIRE(mod_camera_resolve(pose, nullptr, e, a, u));
  SLRR_REQUIRE(near3(e, 10.f, 2.f, -5.f) && near3(a, 10.f, 1.f, -15.f));
  SLRR_REQUIRE(pose.fov == 55.f);
  // Up comes back perpendicular to the view and on the same side.
  const float f[3] = {a[0] - e[0], a[1] - e[1], a[2] - e[2]};
  SLRR_REQUIRE(near(dot(f, u), 0.f) && u[1] > .99f && near(dot(u, u), 1.f));
  // The car origin is ignored by a world pose.
  const float origin[3] = {100.f, 100.f, 100.f};
  SLRR_REQUIRE(mod_camera_resolve(pose, origin, e, a, u) && near3(e, 10.f, 2.f, -5.f));
}

// setPoseRelative adds the car's drawn origin every time it is read, so a
// moving car carries the camera with it without a script update.
SLRR_TEST(relative_pose_follows_the_origin) {
  ModCameraPose pose;
  const float eye[3] = {0.f, 1.5f, 5.f}, at[3] = {0.f, .8f, -2.f}, up[3] = {0.f, 1.f, 0.f};
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::Relative, eye, at, up, 0.f));
  float e[3], a[3], u[3];
  SLRR_REQUIRE(!mod_camera_resolve(pose, nullptr, e, a, u));  // needs the car
  const float o1[3] = {100.f, 3.f, 50.f}, o2[3] = {101.25f, 3.f, 48.5f};
  SLRR_REQUIRE(mod_camera_resolve(pose, o1, e, a, u));
  SLRR_REQUIRE(near3(e, 100.f, 4.5f, 55.f) && near3(a, 100.f, 3.8f, 48.f));
  SLRR_REQUIRE(mod_camera_resolve(pose, o2, e, a, u));
  SLRR_REQUIRE(near3(e, 101.25f, 4.5f, 53.5f) && near3(a, 101.25f, 3.8f, 46.5f));
  const float bad[3] = {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f};
  SLRR_REQUIRE(!mod_camera_resolve(pose, bad, e, a, u));
}

// Roll: a tilted up axis survives; a degenerate one falls back.
SLRR_TEST(up_axis_roll_and_fallbacks) {
  ModCameraPose pose;
  const float eye[3] = {0.f, 0.f, 0.f}, at[3] = {0.f, 0.f, -10.f};
  const float s = std::sin(.3f), c = std::cos(.3f);
  const float tilted[3] = {s, c, 0.f};
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::World, eye, at, tilted, 0.f));
  float e[3], a[3], u[3];
  SLRR_REQUIRE(mod_camera_resolve(pose, nullptr, e, a, u) && near3(u, s, c, 0.f));
  // Zero up: world up.
  const float zero[3] = {0.f, 0.f, 0.f};
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::World, eye, at, zero, 0.f));
  SLRR_REQUIRE(mod_camera_resolve(pose, nullptr, e, a, u) && near3(u, 0.f, 1.f, 0.f));
  // Looking straight down with world up: falls back to world -z.
  const float below[3] = {0.f, -10.f, 0.f}, wup[3] = {0.f, 1.f, 0.f};
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::World, eye, below, wup, 0.f));
  SLRR_REQUIRE(mod_camera_resolve(pose, nullptr, e, a, u) && near3(u, 0.f, 0.f, -1.f));
}

// Invalid input keeps the previous pose; fov is clamped; None clears.
SLRR_TEST(invalid_input_keeps_the_pose) {
  ModCameraPose pose;
  const float eye[3] = {1.f, 2.f, 3.f}, at[3] = {1.f, 2.f, 0.f}, up[3] = {0.f, 1.f, 0.f};
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::World, eye, at, up, 400.f));
  SLRR_REQUIRE(pose.fov == 170.f);
  SLRR_REQUIRE(!mod_camera_set(pose, ModCameraMode::Relative, eye, eye, up, 50.f));
  const float nan3[3] = {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f};
  SLRR_REQUIRE(!mod_camera_set(pose, ModCameraMode::Relative, nan3, at, up, 50.f));
  SLRR_REQUIRE(!mod_camera_set(pose, ModCameraMode::Relative, eye, at, up,
                               std::numeric_limits<float>::infinity()));
  SLRR_REQUIRE(pose.mode == ModCameraMode::World && pose.fov == 170.f && near3(pose.at, 1.f, 2.f, 0.f));
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::World, eye, at, up, .2f) && pose.fov == 1.f);
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::World, eye, at, up, -1.f) && pose.fov == 0.f);
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::None, nullptr, nullptr, nullptr, 0.f));
  float e[3], a[3], u[3];
  SLRR_REQUIRE(pose.mode == ModCameraMode::None && !mod_camera_resolve(pose, nullptr, e, a, u));
}

// drawnOri uses getOri's extraction: a car yawed by y, pitched by p, rolled
// by r reads back y, p, r.
SLRR_TEST(matrix_ypr_round_trip) {
  for (const float y : {0.f, .7f, -2.5f, 3.f})
    for (const float p : {0.f, .2f, -.4f})
      for (const float r : {0.f, .15f, -.3f}) {
        // Rows of R = Ry(y) * Rx(p) * Rz(r) as the engine composes a pose:
        // the z axis (back) is (sin y cos p, -sin p, cos y cos p).
        const float cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
        const float cr = std::cos(r), sr = std::sin(r);
        float m[16] = {};
        // x axis
        m[0] = cy * cr + sy * sp * sr; m[1] = cp * sr; m[2] = -sy * cr + cy * sp * sr;
        // y axis
        m[4] = -cy * sr + sy * sp * cr; m[5] = cp * cr; m[6] = sy * sr + cy * sp * cr;
        // z axis
        m[8] = sy * cp; m[9] = -sp; m[10] = cy * cp;
        m[15] = 1.f;
        float ypr[3];
        mod_matrix_ypr(m, ypr);
        SLRR_REQUIRE(near(ypr[0], y, 1e-4f) && near(ypr[1], p, 1e-4f) && near(ypr[2], r, 1e-4f));
      }
}

// The yaw and pitch written back to the camera make the stock script camera
// (forward = (-sin y cos p, sin p, -cos y cos p)) look at the same point.
SLRR_TEST(look_ypr_matches_the_stock_camera) {
  const float eye[3] = {3.f, 2.f, 1.f};
  for (const float x : {-4.f, 0.f, 5.f})
    for (const float z : {-6.f, 2.f})
      for (const float h : {-1.f, 0.f, 3.f}) {
        const float at[3] = {eye[0] + x, eye[1] + h, eye[2] + z};
        float yaw, pitch;
        mod_look_ypr(eye, at, yaw, pitch);
        const float len = std::sqrt(x * x + h * h + z * z);
        const float f[3] = {-std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                            -std::cos(yaw) * std::cos(pitch)};
        SLRR_REQUIRE(near3(f, x / len, h / len, z / len, 1e-4f));
      }
}

}  // namespace

int main() {
  const slrr::standalone::Test tests[] = {
      api_version_is_one,          world_pose_is_exact,
      relative_pose_follows_the_origin, up_axis_roll_and_fallbacks,
      invalid_input_keeps_the_pose, matrix_ypr_round_trip,
      look_ypr_matches_the_stock_camera,
  };
  return slrr::standalone::run("mod_camera", tests);
}
