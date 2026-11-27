#include "slrr/arcade_camera.hpp"

#include <algorithm>
#include <cmath>

namespace slrr {
namespace {

constexpr float kTwoPi = 6.2831853f;

float smoothstep01(float t) {
  t = std::clamp(t, 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

float approach(float dt, float rate) { return dt > 0.f ? -std::expm1(-dt * rate) : 0.f; }

// 0 parked or slow, 1 from 250 km/h.
float speed_share(float speed) { return smoothstep01((speed * 3.6f - 20.f) / 230.f); }

float fov_goal(float speed, float pull) {
  return kArcadeFovBase + kArcadeFovSpeed * speed_share(speed) +
         kArcadeFovPull * std::clamp(pull / 10.f, 0.f, 1.f);
}

float horizontal_speed(const float* v) { return std::hypot(v[0], v[2]); }

}  // namespace

float arcade_camera_target_yaw(const ArcadeCameraInput& in) {
  const float body = std::atan2(in.back[0], in.back[2]);
  const float back = std::hypot(in.back[0], in.back[2]);
  const float* v = in.velocity;
  const float speed = horizontal_speed(v);
  const float forward = back > 1e-6f ? -(v[0] * in.back[0] + v[2] * in.back[2]) / back : 0.f;
  if (speed < 1.f || forward <= 0.f) return body;
  // The eye sits opposite the velocity: azimuth of -v.
  const float travel = std::atan2(-v[0], -v[2]);
  const float slip = std::clamp(std::remainder(travel - body, kTwoPi), -kArcadeMaxSlip,
                                kArcadeMaxSlip);
  // Below walking pace the heading rules; a slide shows from about 40 km/h.
  const float share = kArcadeSlipShare * smoothstep01((speed - 1.f) / 11.f);
  return std::remainder(body + slip * share, kTwoPi);
}

ArcadeCameraPose arcade_camera_step(ArcadeCameraState& s, const ArcadeCameraInput& in,
                                    float dt) {
  const float target = arcade_camera_target_yaw(in);
  const float speed_now = horizontal_speed(in.velocity);
  if (!s.init) {
    s.yaw = target;
    if (in.seed_eye) {
      const float dx = in.seed_eye[0] - in.position[0], dz = in.seed_eye[2] - in.position[2];
      if (dx * dx + dz * dz > 1e-4f) s.yaw = std::atan2(dx, dz);
    }
    s.focus_y = in.position[1];
    s.speed = speed_now;
    s.pull = 0.f;
    s.fov = fov_goal(speed_now, 0.f);
    s.init = true;
  }
  if (dt > 0.f) {
    const float back = std::hypot(in.back[0], in.back[2]);
    const float forward = back > 1e-6f
        ? -(in.velocity[0] * in.back[0] + in.velocity[2] * in.back[2]) / back : 0.f;
    const float previous = s.speed;
    s.speed += (speed_now - previous) * approach(dt, 4.f);
    const float accel = forward >= 0.f ? (s.speed - previous) / dt : 0.f;
    s.pull += (std::clamp(accel, -15.f, 15.f) - s.pull) * approach(dt, 2.5f);
    // A short lag in bends, tighter at speed so the car stays in frame.
    const float rate = 4.f + 2.f * speed_share(s.speed);
    float lag = std::remainder(target - s.yaw, kTwoPi);
    lag -= lag * approach(dt, rate);
    lag = std::clamp(lag, -kArcadeMaxLag, kArcadeMaxLag);
    s.yaw = std::remainder(target - lag, kTwoPi);
    // Bumps and kerbs move the car more than the view.
    s.focus_y += (in.position[1] - s.focus_y) * approach(dt, 10.f);
    s.focus_y = std::clamp(s.focus_y, in.position[1] - .8f, in.position[1] + .8f);
    s.fov += (fov_goal(s.speed, s.pull) - s.fov) * approach(dt, 2.5f);
  }

  const float share = speed_share(s.speed);
  const float zoom = std::clamp(in.zoom, .3f, 4.f);
  // The eye drops back with speed and while the car pulls, and closes in
  // while it brakes.
  const float reach = (kArcadeDistance + 1.f * share +
                       std::clamp(s.pull * .1f, -.5f, .9f)) * zoom;
  const float height = (kArcadeHeight + .15f * share) * zoom;
  const float azimuth = s.yaw + in.orbit_yaw;
  const float elevation = std::clamp(std::atan2(height, reach) + in.orbit_pitch, -.3f, 1.3f);
  const float radius = std::hypot(reach, height);
  const float focus[3] = {in.position[0], s.focus_y + kArcadeFocusLift, in.position[2]};
  ArcadeCameraPose pose{};
  pose.eye[0] = focus[0] + std::sin(azimuth) * std::cos(elevation) * radius;
  pose.eye[1] = focus[1] + std::sin(elevation) * radius;
  pose.eye[2] = focus[2] + std::cos(azimuth) * std::cos(elevation) * radius;
  // Look past the car, more so at speed: the car sits in the lower part of
  // the frame and the road ahead opens up.
  const float ahead = .6f + 1.8f * share;
  pose.at[0] = focus[0] - std::sin(azimuth) * ahead;
  pose.at[1] = focus[1];
  pose.at[2] = focus[2] - std::cos(azimuth) * ahead;
  pose.fov = s.fov;
  return pose;
}

}  // namespace slrr
