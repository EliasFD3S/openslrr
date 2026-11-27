#pragma once

// Arcade chase camera (#470), modelled on the Need for Speed: Underground 2
// chase view: low and close behind the car, swinging toward the direction of
// travel in a slide so the car's angle shows, a short lag in bends, a wider
// view angle and a longer reach at speed, and a player look that springs
// back. Pure math: gameref.cpp feeds it the chassis pose and velocity and
// draws the pose it returns. The classic (stock) follow camera stays the
// default; host option 34 selects this one.
namespace slrr {

struct ArcadeCameraState {
  bool init = false;
  float yaw = 0.f;      // eye azimuth around the car, radians; atan2(x, z)
  float focus_y = 0.f;  // smoothed height of the car origin, metres
  float speed = 0.f;    // smoothed horizontal speed, m/s
  float pull = 0.f;     // smoothed forward acceleration, m/s^2
  float fov = 0.f;      // vertical view angle, degrees
};

struct ArcadeCameraInput {
  float position[3] = {};  // car origin (world)
  float back[3] = {0.f, 0.f, 1.f};  // chassis +Z axis, the car's rear (world)
  float velocity[3] = {};  // m/s (world)
  float orbit_yaw = 0.f;   // player look offset, radians
  float orbit_pitch = 0.f;
  float zoom = 1.f;        // distance scale from the mouse wheel
  // Where the eye was when this camera takes over (another view, the classic
  // camera); null starts straight behind the car.
  const float* seed_eye = nullptr;
};

struct ArcadeCameraPose {
  float eye[3];
  float at[3];
  float fov;  // vertical, degrees
};

// Tuning, shared with the tests.
inline constexpr float kArcadeFovBase = 50.f;   // parked
inline constexpr float kArcadeFovSpeed = 22.f;  // added by 250 km/h
inline constexpr float kArcadeFovPull = 4.f;    // added at full acceleration
inline constexpr float kArcadeDistance = 6.3f;  // eye to focus, parked
inline constexpr float kArcadeHeight = 1.45f;   // eye above the focus
inline constexpr float kArcadeFocusLift = .75f; // focus above the car origin
inline constexpr float kArcadeMaxLag = 1.2f;    // radians the eye may trail
inline constexpr float kArcadeSlipShare = .6f;  // of the slide angle shown
inline constexpr float kArcadeMaxSlip = .6f;    // radians of slide followed

// The car's heading blended toward its direction of travel, as an eye
// azimuth (the side the eye sits on). Reversing keeps the heading.
float arcade_camera_target_yaw(const ArcadeCameraInput& in);

// Advances the state by dt seconds and returns the pose. dt <= 0 only reads
// the state (several pose queries per frame); the first call seeds it.
ArcadeCameraPose arcade_camera_step(ArcadeCameraState& state, const ArcadeCameraInput& in,
                                    float dt);

}  // namespace slrr
