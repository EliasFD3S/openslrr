#pragma once

// Arcade camera (#470), modelled on Need for Speed: Underground 2. Its four
// views, cycled by the change-view key (Track.nextCamera, host option 35):
// BUMPER and HOOD are fixed to the car; NEAR and FAR chase it, low behind,
// swinging toward the direction of travel in a slide so the car's angle
// shows, with a short lag in bends, a wider view angle and a longer reach at
// speed, and a player look that springs back. Pure math: gameref.cpp feeds it
// the chassis pose, velocity and extent and draws the pose it returns. The
// classic (stock) follow camera stays the default; host option 34 selects
// this one. The values approximate NFSU2's views by eye: the game's own
// camera table was not available.
namespace slrr {

enum class ArcadeView : int { Bumper = 0, Hood = 1, Near = 2, Far = 3 };
inline constexpr int kArcadeViews = 4;
inline constexpr ArcadeView kArcadeDefaultView = ArcadeView::Far;

// One view's tuning. Chase views use distance..ahead; every view uses the
// view angle (vertical, degrees): fov_base parked, plus fov_speed by
// 250 km/h, plus up to fov_pull at full acceleration.
struct ArcadeViewTuning {
  bool mounted;       // fixed to the car (bumper, hood)
  float distance;     // eye to focus, parked (chase)
  float height;       // eye above the focus (chase)
  float focus_lift;   // focus above the car origin (chase)
  float ahead;        // look point past the car, parked (chase)
  float fov_base, fov_speed, fov_pull;
};
const ArcadeViewTuning& arcade_view_tuning(ArcadeView view);
ArcadeView arcade_view_from_index(int index);  // out of range: the default

struct ArcadeCameraState {
  bool init = false;
  float yaw = 0.f;      // eye azimuth around the car, radians; atan2(x, z)
  float focus_y = 0.f;  // smoothed height of the car origin, metres
  float speed = 0.f;    // smoothed horizontal speed, m/s
  float pull = 0.f;     // smoothed forward acceleration, m/s^2
  float fov = 0.f;      // vertical view angle, degrees
};

struct ArcadeCameraInput {
  ArcadeView view = kArcadeDefaultView;
  float position[3] = {};  // car origin (world)
  float back[3] = {0.f, 0.f, 1.f};  // chassis +Z axis, the car's rear (world)
  float up[3] = {0.f, 1.f, 0.f};    // chassis +Y axis (world)
  float velocity[3] = {};  // m/s (world)
  float orbit_yaw = 0.f;   // player look offset, radians (chase views)
  float orbit_pitch = 0.f;
  float zoom = 1.f;        // distance scale from the mouse wheel (chase views)
  // Chassis-local extent of the car's meshes (x right, y up, z back), for
  // the mounted views; without it a typical saloon is assumed.
  bool box_valid = false;
  float box_lo[3] = {}, box_hi[3] = {};
  // Where the eye was when this camera takes over (another view, the classic
  // camera); null starts straight behind the car.
  const float* seed_eye = nullptr;
};

struct ArcadeCameraPose {
  float eye[3];
  float at[3];
  float up[3];
  float fov;  // vertical, degrees
};

// The car's heading blended toward its direction of travel, as an eye
// azimuth (the side the eye sits on). Reversing keeps the heading.
float arcade_camera_target_yaw(const ArcadeCameraInput& in);

// Advances the state by dt seconds and returns the pose. dt <= 0 only reads
// the state (several pose queries per frame); the first call seeds it.
ArcadeCameraPose arcade_camera_step(ArcadeCameraState& state, const ArcadeCameraInput& in,
                                    float dt);

// Tuning shared with the tests.
inline constexpr float kArcadeMaxLag = 1.2f;    // radians the eye may trail
inline constexpr float kArcadeSlipShare = .6f;  // of the slide angle shown
inline constexpr float kArcadeMaxSlip = .6f;    // radians of slide followed
inline constexpr float kArcadeBumperHeight = .55f;  // bumper eye above the lowest point

}  // namespace slrr
