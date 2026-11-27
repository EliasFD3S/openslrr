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
using slrr::mod_look_roll;
using slrr::mod_look_ypr;
using slrr::mod_matrix_ypr;

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) < eps; }
bool near3(const float* a, float x, float y, float z, float eps = 1e-5f) {
  return near(a[0], x, eps) && near(a[1], y, eps) && near(a[2], z, eps);
}
float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A drawn car frame as the engine composes a pose: R = Ry(y) * Rx(p) * Rz(r),
// axes x right, y up, z back in m[0..2], m[4..6], m[8..10], origin in m[12..14].
void car_frame(float y, float p, float r, const float* origin, float* m) {
  const float cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
  const float cr = std::cos(r), sr = std::sin(r);
  for (int i = 0; i < 16; ++i) m[i] = 0.f;
  m[0] = cy * cr + sy * sp * sr; m[1] = cp * sr; m[2] = -sy * cr + cy * sp * sr;
  m[4] = -cy * sr + sy * sp * cr; m[5] = cp * cr; m[6] = sy * sr + cy * sp * cr;
  m[8] = sy * cp; m[9] = -sp; m[10] = cy * cp;
  m[12] = origin[0]; m[13] = origin[1]; m[14] = origin[2]; m[15] = 1.f;
}
// m's rotation applied to v, plus its origin.
void place(const float* m, const float* v, float* out) {
  for (int i = 0; i < 3; ++i) out[i] = m[12 + i] + m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2];
}

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
  // The car frame is ignored by a world pose.
  float frame[16] = {};
  frame[12] = frame[13] = frame[14] = 100.f;
  SLRR_REQUIRE(mod_camera_resolve(pose, frame, e, a, u) && near3(e, 10.f, 2.f, -5.f));
}

// setPoseRelative adds the car's drawn origin every time it is read, so a
// moving car carries the camera with it without a script update.
SLRR_TEST(relative_pose_follows_the_origin) {
  ModCameraPose pose;
  const float eye[3] = {0.f, 1.5f, 5.f}, at[3] = {0.f, .8f, -2.f}, up[3] = {0.f, 1.f, 0.f};
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::Relative, eye, at, up, 0.f));
  float e[3], a[3], u[3];
  SLRR_REQUIRE(!mod_camera_resolve(pose, nullptr, e, a, u));  // needs the car
  float o1[16] = {}, o2[16] = {};
  o1[12] = 100.f, o1[13] = 3.f, o1[14] = 50.f;
  o2[12] = 101.25f, o2[13] = 3.f, o2[14] = 48.5f;
  SLRR_REQUIRE(mod_camera_resolve(pose, o1, e, a, u));
  SLRR_REQUIRE(near3(e, 100.f, 4.5f, 55.f) && near3(a, 100.f, 3.8f, 48.f));
  SLRR_REQUIRE(mod_camera_resolve(pose, o2, e, a, u));
  SLRR_REQUIRE(near3(e, 101.25f, 4.5f, 53.5f) && near3(a, 101.25f, 3.8f, 46.5f));
  float bad[16] = {};
  bad[12] = std::numeric_limits<float>::quiet_NaN();
  SLRR_REQUIRE(!mod_camera_resolve(pose, bad, e, a, u));
}

// setPoseLocal: the offsets and up turn with the drawn chassis frame, so a
// hood camera stays on the body whatever its yaw, pitch and bank.
SLRR_TEST(local_pose_turns_and_banks_with_the_car) {
  ModCameraPose pose;
  const float eye[3] = {0.f, 1.2f, -.4f}, at[3] = {0.f, 1.f, -10.f}, up[3] = {0.f, 1.f, 0.f};
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::Local, eye, at, up, 70.f));
  float e[3], a[3], u[3];
  SLRR_REQUIRE(!mod_camera_resolve(pose, nullptr, e, a, u));  // needs the car
  const float origin[3] = {40.f, 2.f, -30.f};
  for (const float y : {0.f, 1.5707963f, -2.4f})
    for (const float p : {0.f, .25f})
      for (const float r : {0.f, .35f, -.5f}) {
        float m[16], we[3], wa[3];
        car_frame(y, p, r, origin, m);
        SLRR_REQUIRE(mod_camera_resolve(pose, m, e, a, u));
        place(m, eye, we);
        place(m, at, wa);
        SLRR_REQUIRE(near3(e, we[0], we[1], we[2], 1e-4f) && near3(a, wa[0], wa[1], wa[2], 1e-4f));
        // Banked: the camera's up leans with the body's up axis (minus the
        // slight downward tilt of the view).
        const float f[3] = {a[0] - e[0], a[1] - e[1], a[2] - e[2]};
        SLRR_REQUIRE(near(dot(f, u), 0.f, 1e-4f) && near(dot(u, u), 1.f, 1e-4f));
        SLRR_REQUIRE(dot(u, m + 4) > .99f);
        // and stays square to the body's x axis.
        SLRR_REQUIRE(near(dot(u, m), 0.f, 1e-4f));
      }
  // Turning the car by 90 degrees moves a nose offset to the side.
  float m[16];
  const float zero[3] = {0.f, 0.f, 0.f};
  car_frame(1.5707963f, 0.f, 0.f, zero, m);
  SLRR_REQUIRE(mod_camera_resolve(pose, m, e, a, u));
  SLRR_REQUIRE(near3(e, -.4f, 1.2f, 0.f, 1e-4f) && near3(a, -10.f, 1.f, 0.f, 1e-4f));
  // A scaled frame does not stretch the offsets.
  for (int i = 0; i < 11; ++i) m[i] *= 2.f;
  SLRR_REQUIRE(mod_camera_resolve(pose, m, e, a, u) && near3(e, -.4f, 1.2f, 0.f, 1e-4f));
  // A zero local up is the car's up; a degenerate frame is refused.
  const float zup[3] = {0.f, 0.f, 0.f};
  SLRR_REQUIRE(mod_camera_set(pose, ModCameraMode::Local, eye, at, zup, 0.f));
  car_frame(0.f, 0.f, .4f, zero, m);
  SLRR_REQUIRE(mod_camera_resolve(pose, m, e, a, u) && dot(u, m + 4) > .99f);
  float flat[16] = {};
  flat[15] = 1.f;
  SLRR_REQUIRE(!mod_camera_resolve(pose, flat, e, a, u));
}

// The roll written back to the camera (getOri().r, camera_roll) rebuilds the
// same up with the stock roll formula, so clearPose keeps the horizon.
SLRR_TEST(roll_write_back_matches_the_roll_command) {
  const float eye[3] = {1.f, 2.f, 3.f};
  for (const float yaw : {0.f, .8f, -2.f})
    for (const float pitch : {0.f, .3f, -.4f})
      for (const float roll : {0.f, .2f, -.6f, 1.2f}) {
        const float cp = std::cos(pitch);
        const float f[3] = {-std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};
        const float at[3] = {eye[0] + f[0] * 5.f, eye[1] + f[1] * 5.f, eye[2] + f[2] * 5.f};
        // right = forward x world up, level = right x forward (gameref.cpp).
        float right[3] = {-f[2], 0.f, f[0]};
        const float rl = std::sqrt(dot(right, right));
        for (float& v : right) v /= rl;
        const float level[3] = {right[1] * f[2] - right[2] * f[1], right[2] * f[0] - right[0] * f[2],
                                right[0] * f[1] - right[1] * f[0]};
        float up[3];
        for (int i = 0; i < 3; ++i) up[i] = level[i] * std::cos(roll) + right[i] * std::sin(roll);
        SLRR_REQUIRE(near(mod_look_roll(eye, at, up), roll, 1e-4f));
      }
  // A level up is roll 0; a vertical view has no defined roll.
  const float at[3] = {1.f, 2.f, -7.f}, wup[3] = {0.f, 1.f, 0.f}, below[3] = {1.f, -5.f, 3.f};
  const float back[3] = {0.f, 0.f, -1.f};
  SLRR_REQUIRE(near(mod_look_roll(eye, at, wup), 0.f));
  SLRR_REQUIRE(mod_look_roll(eye, below, back) == 0.f);
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
  // A NaN or infinite up is rejected too, not taken as world up.
  const float inf_up[3] = {0.f, std::numeric_limits<float>::infinity(), 0.f};
  SLRR_REQUIRE(!mod_camera_set(pose, ModCameraMode::World, eye, at, nan3, 50.f));
  SLRR_REQUIRE(!mod_camera_set(pose, ModCameraMode::Local, eye, at, inf_up, 50.f));
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
      look_ypr_matches_the_stock_camera, local_pose_turns_and_banks_with_the_car,
      roll_write_back_matches_the_roll_command,
  };
  return slrr::standalone::run("mod_camera", tests);
}
