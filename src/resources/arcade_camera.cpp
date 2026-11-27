#include "slrr/arcade_camera.hpp"

#include <algorithm>
#include <cmath>

namespace slrr {
namespace {

constexpr float kTwoPi = 6.2831853f;

// Approximations of NFSU2's views (see the header).
constexpr ArcadeViewTuning kViews[kArcadeViews] = {
    // mounted distance height lift  ahead  fov: base speed pull
    {true, 0.f, 0.f, 0.f, 0.f, 58.f, 20.f, 3.f},         // bumper
    {true, 0.f, 0.f, 0.f, 0.f, 55.f, 20.f, 3.f},         // hood
    {false, 4.4f, 1.1f, .7f, .5f, 54.f, 20.f, 4.f},      // near
    {false, 6.3f, 1.45f, .75f, .6f, 50.f, 22.f, 4.f},    // far
};

float smoothstep01(float t) {
  t = std::clamp(t, 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

float approach(float dt, float rate) { return dt > 0.f ? -std::expm1(-dt * rate) : 0.f; }

// 0 parked or slow, 1 from 250 km/h.
float speed_share(float speed) { return smoothstep01((speed * 3.6f - 20.f) / 230.f); }

float fov_goal(const ArcadeViewTuning& t, float speed, float pull) {
  return t.fov_base + t.fov_speed * speed_share(speed) +
         t.fov_pull * std::clamp(pull / 10.f, 0.f, 1.f);
}

float horizontal_speed(const float* v) { return std::hypot(v[0], v[2]); }

float forward_speed(const ArcadeCameraInput& in) {
  const float back = std::hypot(in.back[0], in.back[2]);
  return back > 1e-6f ? -(in.velocity[0] * in.back[0] + in.velocity[2] * in.back[2]) / back
                      : 0.f;
}

// Chassis-local point to world. x = up x back (right), y = up, z = back.
void local_to_world(const ArcadeCameraInput& in, const float* local, float* world) {
  const float* u = in.up;
  const float* b = in.back;
  const float r[3] = {u[1] * b[2] - u[2] * b[1], u[2] * b[0] - u[0] * b[2],
                      u[0] * b[1] - u[1] * b[0]};
  for (int i = 0; i < 3; ++i)
    world[i] = in.position[i] + r[i] * local[0] + u[i] * local[1] + b[i] * local[2];
}

ArcadeCameraPose mounted_pose(const ArcadeCameraInput& in) {
  // A saloon about 4.4 m long and 1.4 m tall when the meshes are unknown.
  float lo[3] = {-.9f, -.5f, -2.2f}, hi[3] = {.9f, .9f, 2.2f};
  if (in.box_valid && in.box_hi[1] > in.box_lo[1] && in.box_hi[2] > in.box_lo[2]) {
    std::copy_n(in.box_lo, 3, lo);
    std::copy_n(in.box_hi, 3, hi);
  }
  const float length = hi[2] - lo[2], tall = hi[1] - lo[1];
  float eye_local[3], look_local[3];
  if (in.view == ArcadeView::Bumper || in.look_back) {
    // Low, just ahead of the front bumper: the car itself is not seen.
    // Looking back (both mounted views) it is the rear bumper, facing back.
    const float end = in.look_back ? hi[2] + .05f : lo[2] - .05f;
    const float dir = in.look_back ? 1.f : -1.f;
    eye_local[0] = 0.f;
    eye_local[1] = lo[1] + std::min(kArcadeBumperHeight, .6f * tall);
    eye_local[2] = end;
    look_local[0] = 0.f;
    look_local[1] = eye_local[1];
    look_local[2] = eye_local[2] + 10.f * dir;
  } else {
    // Just below roof height at the base of the windscreen, looking down
    // over the bonnet (lower, the eye sits under the bonnet skin).
    eye_local[0] = 0.f;
    eye_local[1] = hi[1] - .06f * tall;
    eye_local[2] = lo[2] + .35f * length;
    look_local[0] = 0.f;
    look_local[1] = eye_local[1] - 1.f;
    look_local[2] = eye_local[2] - 10.f;
  }
  ArcadeCameraPose pose{};
  local_to_world(in, eye_local, pose.eye);
  local_to_world(in, look_local, pose.at);
  std::copy_n(in.up, 3, pose.up);
  return pose;
}

void cross(const float* a, const float* b, float* out) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

bool normalize(float* v) {
  const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len < 1e-6f) return false;
  for (int i = 0; i < 3; ++i) v[i] /= len;
  return true;
}

// Shakes and tilt on a finished pose: eye and look point move together
// (the view jitters rather than the aim), and the up axis turns by roll
// about the view direction.
void apply_motion(ArcadeCameraPose& pose, float shake_x, float shake_y, float roll) {
  float forward[3] = {pose.at[0] - pose.eye[0], pose.at[1] - pose.eye[1],
                      pose.at[2] - pose.eye[2]};
  if (!normalize(forward)) return;
  float right[3];
  cross(forward, pose.up, right);
  if (!normalize(right)) return;
  float up[3];
  cross(right, forward, up);
  for (int i = 0; i < 3; ++i) {
    const float offset = right[i] * shake_x + up[i] * shake_y;
    pose.eye[i] += offset;
    pose.at[i] += offset;
  }
  const float c = std::cos(roll), s = std::sin(roll);
  for (int i = 0; i < 3; ++i) pose.up[i] = up[i] * c + right[i] * s;
}

}  // namespace

const ArcadeViewTuning& arcade_view_tuning(ArcadeView view) {
  const int i = static_cast<int>(view);
  return kViews[i >= 0 && i < kArcadeViews ? i : static_cast<int>(kArcadeDefaultView)];
}

ArcadeView arcade_view_from_index(int index) {
  return index >= 0 && index < kArcadeViews ? static_cast<ArcadeView>(index)
                                            : kArcadeDefaultView;
}

float arcade_camera_target_yaw(const ArcadeCameraInput& in) {
  const float body = std::atan2(in.back[0], in.back[2]);
  const float* v = in.velocity;
  const float speed = horizontal_speed(v);
  // In the air the slide is not shown: the view stays on the heading.
  if (speed < 1.f || forward_speed(in) <= 0.f || in.airborne) return body;
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
  const ArcadeViewTuning& tune = arcade_view_tuning(in.view);
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
    s.fov = fov_goal(tune, speed_now, 0.f);
    s.nitro = in.nitro ? 1.f : 0.f;
    s.impact = 0.f;
    s.roll = 0.f;
    std::copy_n(in.velocity, 3, s.velocity);
    s.init = true;
  }
  if (dt > 0.f) {
    const float previous = s.speed;
    s.speed += (speed_now - previous) * approach(dt, 4.f);
    const float accel = forward_speed(in) >= 0.f ? (s.speed - previous) / dt : 0.f;
    s.pull += (std::clamp(accel, -15.f, 15.f) - s.pull) * approach(dt, 2.5f);
    // A short lag in bends, tighter at speed so the car stays in frame.
    // The mounted views keep it too, so a chase view picked next starts
    // behind the car.
    const float rate = 4.f + 2.f * speed_share(s.speed);
    float lag = std::remainder(target - s.yaw, kTwoPi);
    lag -= lag * approach(dt, rate);
    lag = std::clamp(lag, -kArcadeMaxLag, kArcadeMaxLag);
    s.yaw = std::remainder(target - lag, kTwoPi);
    // Bumps and kerbs move the car more than the view; in the air the view
    // floats behind a jump and settles after the landing.
    s.focus_y += (in.position[1] - s.focus_y) * approach(dt, in.airborne ? 3.f : 10.f);
    s.focus_y = std::clamp(s.focus_y, in.position[1] - kArcadeMaxDrop,
                           in.position[1] + kArcadeMaxDrop);
    // Nitrous: the view widens, drops back and shakes while it burns.
    s.nitro += ((in.nitro ? 1.f : 0.f) - s.nitro) * approach(dt, in.nitro ? 3.f : 1.5f);
    // A crash or a hard landing: a velocity change above 4 g shakes the view.
    const float dv = std::sqrt((in.velocity[0] - s.velocity[0]) * (in.velocity[0] - s.velocity[0]) +
                               (in.velocity[1] - s.velocity[1]) * (in.velocity[1] - s.velocity[1]) +
                               (in.velocity[2] - s.velocity[2]) * (in.velocity[2] - s.velocity[2]));
    std::copy_n(in.velocity, 3, s.velocity);
    const float jolt = dv / dt;
    if (jolt > kArcadeImpactAccel)
      s.impact = std::max(s.impact, std::min((jolt - kArcadeImpactAccel) / 200.f, 1.f));
    s.impact *= std::exp(-dt * 6.f);
    // Tilt with the car's lean in a bend: the lateral acceleration (yaw
    // rate x speed) turns the view toward the outside of the turn.
    const float lateral = in.airborne ? 0.f : in.yaw_rate * s.speed;
    const float tilt = std::clamp(lateral / 9.81f * kArcadeTiltPerG, -kArcadeMaxTilt, kArcadeMaxTilt);
    s.roll += (tilt - s.roll) * approach(dt, 3.f);
    s.clock += dt;
    s.fov += (fov_goal(tune, s.speed, s.pull) + kArcadeNitroFov * s.nitro - s.fov) *
             approach(dt, 2.5f);
  }

  // Road vibration from 150 km/h (stronger on the bumper), plus nitrous and
  // crash shakes: a few incommensurate sines, so it never repeats visibly.
  const float vibration = smoothstep01((s.speed * 3.6f - 150.f) / 150.f);
  const float mount = in.view == ArcadeView::Bumper ? 2.f : tune.mounted ? 1.4f : 1.f;
  const float amplitude = kArcadeSpeedShake * vibration * mount +
                          kArcadeNitroShake * s.nitro + kArcadeImpactShake * s.impact;
  const float t = s.clock;
  const float shake_x = amplitude * (.6f * std::sin(t * 57.f) + .4f * std::sin(t * 89.f + 1.3f));
  const float shake_y = amplitude * (.6f * std::sin(t * 71.f + .7f) + .4f * std::sin(t * 113.f));

  if (tune.mounted) {
    ArcadeCameraPose pose = mounted_pose(in);
    pose.fov = s.fov;
    apply_motion(pose, shake_x, shake_y, 0.f);
    return pose;
  }

  const float share = speed_share(s.speed);
  const float zoom = std::clamp(in.zoom, .3f, 4.f);
  // The eye drops back with speed and while the car pulls, and closes in
  // while it brakes.
  // At speed it also sits a little lower, and nitrous pushes it back.
  const float reach = (tune.distance + tune.distance / 6.3f * share +
                       std::clamp(s.pull * .1f, -.5f, .9f) + kArcadeNitroReach * s.nitro) * zoom;
  const float height = tune.height * (1.f - .12f * share - .08f * s.nitro) * zoom;
  // Looking back (NFSU2's look-back key) is a straight cut to the same chase
  // view from in front of the car, without lag or player look.
  const float azimuth = in.look_back ? std::remainder(std::atan2(in.back[0], in.back[2]) + 3.1415927f, kTwoPi)
                                     : s.yaw + in.orbit_yaw;
  const float elevation = std::clamp(std::atan2(height, reach) + (in.look_back ? 0.f : in.orbit_pitch),
                                     -.3f, 1.3f);
  const float radius = std::hypot(reach, height);
  const float focus[3] = {in.position[0], s.focus_y + tune.focus_lift, in.position[2]};
  ArcadeCameraPose pose{};
  pose.eye[0] = focus[0] + std::sin(azimuth) * std::cos(elevation) * radius;
  pose.eye[1] = focus[1] + std::sin(elevation) * radius;
  pose.eye[2] = focus[2] + std::cos(azimuth) * std::cos(elevation) * radius;
  // Look past the car, more so at speed: the car sits in the lower part of
  // the frame and the road ahead opens up.
  const float ahead = tune.ahead + 1.8f * share;
  pose.at[0] = focus[0] - std::sin(azimuth) * ahead;
  pose.at[1] = focus[1];
  pose.at[2] = focus[2] - std::cos(azimuth) * ahead;
  pose.up[0] = 0.f;
  pose.up[1] = 1.f;
  pose.up[2] = 0.f;
  pose.fov = s.fov;
  apply_motion(pose, shake_x, shake_y, in.look_back ? -s.roll : s.roll);
  return pose;
}

}  // namespace slrr
