#pragma once

// Pure parts of the mod camera API (java.openslrr, #487): the script camera
// pose that CameraControl.setPose / setPoseRelative / setPoseLocal store, how
// it resolves to an eye, focus and up axis, and the Euler angles
// CarState.drawnOri reports.
// No engine state: gameref.cpp and natives_openslrr.cpp supply the inputs.

namespace slrr {

enum class ModCameraMode {
  None = 0,      // stock camera behaviour (clearPose)
  World = 1,     // exact world eye, focus and up (setPose)
  Relative = 2,  // eye and focus are world-space offsets from a car's drawn
                 // origin, added whenever the pose is read (setPoseRelative)
  Local = 3,     // eye, focus and up are in the car's chassis frame (x right,
                 // y up, z back), rotated and placed by the drawn frame
                 // whenever the pose is read (setPoseLocal)
};

struct ModCameraPose {
  ModCameraMode mode = ModCameraMode::None;
  float eye[3] = {};
  float at[3] = {};
  float up[3] = {0.f, 1.f, 0.f};
  float fov = 0.f;  // vertical degrees; 0 keeps the camera's own view angle
};

// Stores a pose. False (and the pose unchanged) when a value is not finite
// or the eye and focus coincide. fov <= 0 means "keep the camera's own"; a
// positive fov is clamped to 1..170 degrees.
bool mod_camera_set(ModCameraPose& pose, ModCameraMode mode, const float eye[3],
                    const float at[3], const float up[3], float fov);

// Resolves a stored pose. car_frame is the car's drawn frame (column-major
// 4x4, axes in [0..2], [4..6], [8..10], origin in [12..14]) and is required
// by Relative (its origin) and Local (all of it); false without it. Local
// offsets and up are rotated by the frame's normalized axes. The up axis is
// made perpendicular to the view direction; a zero or parallel up falls back
// to world up (the car's up for Local), then to world -z. False for None.
bool mod_camera_resolve(const ModCameraPose& pose, const float* car_frame, float eye[3],
                        float at[3], float up[3]);

// Yaw, pitch and roll of a matrix (column-major, axes in m[0..2], m[4..6],
// m[8..10], z pointing back), extracted as dynamics_sync does for
// GameRef.getOri, so CarState.drawnOri agrees with getOri.
void mod_matrix_ypr(const float m[16], float ypr[3]);

// Camera yaw and pitch that make the stock script camera (eye at pos,
// looking along -z rotated by yaw, pitched up by pitch) look from eye to at.
void mod_look_ypr(const float eye[3], const float at[3], float& yaw, float& pitch);

// CameraControl.sweep's arguments: finite endpoints and a finite radius >= 0.
// The native returns 1 (clear) without sweeping otherwise.
bool mod_sweep_args_valid(const float* from, const float* to, float radius);

// Roll of up about the view from eye to at, in the convention of the camera
// `roll` command (gameref.cpp world_camera_pose: rolled up = level up * cos r
// + right * sin r, right = forward x world up). 0 when the view is vertical.
float mod_look_roll(const float eye[3], const float at[3], const float up[3]);

}  // namespace slrr
