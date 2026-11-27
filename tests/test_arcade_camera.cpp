#include "standalone_test.hpp"

#include "slrr/arcade_camera.hpp"

#include <cmath>

// Arcade chase camera (#470): the pure pose math gameref.cpp drives.
namespace {

using slrr::ArcadeCameraInput;
using slrr::ArcadeCameraState;
using slrr::arcade_camera_step;
using slrr::arcade_camera_target_yaw;

constexpr float kPi = 3.14159265f;

// A car at the origin heading -Z (its rear, +Z, is the chassis back axis).
ArcadeCameraInput car(float speed_ms = 0.f) {
  ArcadeCameraInput in;
  in.velocity[2] = -speed_ms;
  return in;
}

float wrap(float a) { return std::remainder(a, 2.f * kPi); }

ArcadeCameraState settled(const ArcadeCameraInput& in, int steps = 600) {
  ArcadeCameraState s;
  for (int i = 0; i < steps; ++i) arcade_camera_step(s, in, 1.f / 60.f);
  return s;
}

SLRR_TEST(parked_car_is_seen_from_behind_and_above) {
  ArcadeCameraState s;
  const auto pose = arcade_camera_step(s, car(), 0.f);
  SLRR_REQUIRE(s.init);
  SLRR_REQUIRE(pose.eye[2] > 5.5f && pose.eye[2] < 7.5f);  // behind: +Z
  SLRR_REQUIRE(std::fabs(pose.eye[0]) < 1e-4f);
  SLRR_REQUIRE(pose.eye[1] > 1.5f && pose.eye[1] < 3.f);
  SLRR_REQUIRE(pose.at[2] < 0.f);  // looks past the car
  SLRR_REQUIRE(std::fabs(pose.fov - slrr::kArcadeFovBase) < 1e-3f);
}

SLRR_TEST(read_only_queries_do_not_step) {
  ArcadeCameraState s;
  arcade_camera_step(s, car(), 0.f);
  auto turned = car();
  turned.back[0] = 1.f;  // the car now faces -X
  turned.back[2] = 0.f;
  const float yaw = s.yaw;
  for (int i = 0; i < 10; ++i) arcade_camera_step(s, turned, 0.f);
  SLRR_REQUIRE(s.yaw == yaw);
}

SLRR_TEST(the_eye_trails_a_turn_then_settles_behind) {
  ArcadeCameraState s;
  arcade_camera_step(s, car(), 0.f);
  auto turned = car();
  turned.back[0] = 1.f;  // heading -X: the eye belongs at +X, azimuth pi/2
  turned.back[2] = 0.f;
  arcade_camera_step(s, turned, 1.f / 60.f);
  const float lag = std::fabs(wrap(kPi / 2 - s.yaw));
  SLRR_REQUIRE(lag > .5f && lag <= slrr::kArcadeMaxLag + 1e-4f);  // trails, bounded
  for (int i = 0; i < 300; ++i) arcade_camera_step(s, turned, 1.f / 60.f);
  SLRR_REQUIRE(std::fabs(wrap(kPi / 2 - s.yaw)) < 1e-3f);
}

SLRR_TEST(a_slide_swings_the_view_toward_travel) {
  // Heading -Z, sliding toward -Z and +X (30 degrees of slip) at 90 km/h.
  auto in = car();
  const float slip = kPi / 6;
  in.velocity[0] = 25.f * std::sin(slip);
  in.velocity[2] = -25.f * std::cos(slip);
  const float yaw = arcade_camera_target_yaw(in);
  // The eye moves to the opposite side of travel (-X), part of the angle.
  SLRR_REQUIRE(yaw < -.05f && yaw > -slip);
  // Reversing keeps the heading.
  auto back = car();
  back.velocity[0] = 3.f;
  back.velocity[2] = 10.f;
  SLRR_REQUIRE(std::fabs(arcade_camera_target_yaw(back)) < 1e-6f);
  // A spin follows at most kArcadeMaxSlip of the slide.
  auto spin = car();
  spin.velocity[0] = 25.f;
  spin.velocity[2] = -1.f;
  SLRR_REQUIRE(std::fabs(arcade_camera_target_yaw(spin)) <=
               slrr::kArcadeSlipShare * slrr::kArcadeMaxSlip + 1e-5f);
}

SLRR_TEST(speed_widens_the_view_and_reach) {
  const auto parked = car();
  const auto fast = car(70.f);  // 252 km/h
  ArcadeCameraState slow_state = settled(parked), fast_state = settled(fast);
  const auto slow_pose = arcade_camera_step(slow_state, parked, 0.f);
  const auto fast_pose = arcade_camera_step(fast_state, fast, 0.f);
  SLRR_REQUIRE(fast_pose.fov > slow_pose.fov + 15.f);
  SLRR_REQUIRE(fast_pose.fov <= slrr::kArcadeFovBase + slrr::kArcadeFovSpeed + 1e-3f);
  SLRR_REQUIRE(fast_pose.eye[2] > slow_pose.eye[2] + .5f);
}

SLRR_TEST(seed_eye_and_zoom) {
  ArcadeCameraState s;
  auto in = car();
  const float eye[3] = {-3.f, 2.f, 0.f};  // left of the car
  in.seed_eye = eye;
  arcade_camera_step(s, in, 0.f);
  SLRR_REQUIRE(std::fabs(wrap(s.yaw + kPi / 2)) < 1e-4f);
  ArcadeCameraState a, b;
  auto near = car(), far = car();
  far.zoom = 2.f;
  const auto pa = arcade_camera_step(a, near, 0.f);
  const auto pb = arcade_camera_step(b, far, 0.f);
  SLRR_REQUIRE(std::fabs(pb.eye[2] - 2.f * pa.eye[2]) < 1e-3f);
}

SLRR_TEST(bumps_move_the_car_more_than_the_view) {
  ArcadeCameraState s;
  auto in = car(20.f);
  arcade_camera_step(s, in, 0.f);
  in.position[1] = .3f;
  arcade_camera_step(s, in, 1.f / 60.f);
  SLRR_REQUIRE(s.focus_y > 0.f && s.focus_y < .15f);
  in.position[1] = 5.f;  // a drop or a jump never leaves it far behind
  arcade_camera_step(s, in, 1.f / 60.f);
  SLRR_REQUIRE(s.focus_y >= 4.2f - 1e-4f);
}

}  // namespace

int main() {
  const slrr::standalone::Test tests[] = {
      parked_car_is_seen_from_behind_and_above, read_only_queries_do_not_step,
      the_eye_trails_a_turn_then_settles_behind, a_slide_swings_the_view_toward_travel,
      speed_widens_the_view_and_reach, seed_eye_and_zoom, bumps_move_the_car_more_than_the_view};
  return slrr::standalone::run("arcade_camera", tests);
}
