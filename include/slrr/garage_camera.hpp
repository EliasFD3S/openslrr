#pragma once

#include <cstdint>

namespace slrr {

// Host-side RID_CAMERA stand-in. Feel comes from GameRef.command
// (Garage.cameraSetup: smooth / dist / zoom / force / torque).
struct GarageCamera {
  bool active = false;
  float look_x = 0.f;
  float look_y = 0.f;
  float look_z = -0.5f;  // Garage.defLookPos
  float dist = 3.5f;
  float dist_min = 1.f;   // command "dist 1.0 10.0"
  float dist_max = 10.f;
  // Orbit angles (NOT stock Ypr look orientation). Pitch > 0 = above look_at.
  float yaw = -2.03f;
  float pitch = 0.42f;
  // Targets the mouse integrates. The rendered pose chases them.
  float target_yaw = -2.03f;
  float target_pitch = 0.42f;
  float target_dist = 3.5f;
  float yaw_vel = 0.f;
  float pitch_vel = 0.f;
  float dist_vel = 0.f;
  float fov_deg = 60.f;   // command "zoom 60 5"
  float zoom_speed = 5.f;
  // Per-frame blend at 60 Hz. Position is the heavy garage orbit.
  float smooth_pos = 0.05f;
  float smooth_ori = 0.5f;
  // command "force 0.3 0.5 -0.7" / "torque 0.08" (Config.java defaults).
  float force = 0.3f;
  float damping = 0.5f;
  float perp_damping = -0.7f;
  float torque = 0.08f;
};

GarageCamera& garage_camera();

// Apply stock Garage.cameraSetup defaults (no car → map look).
void garage_camera_setup_defaults();

// GameRef.command lines: smooth, dist, zoom, force, torque.
void garage_camera_command(const char* line);

// Integrate look axes (rad/s style deltas) + zoom for one frame (~dt).
void garage_camera_tick(float dt, float look_yaw, float look_pitch,
                        float look_zoom);

// Fill view (lookAt) + proj matrices. aspect = w/h. homogeneousDepth from bgfx.
void garage_camera_matrices(float* view, float* proj, float aspect,
                            bool homogeneous_depth);

}  // namespace slrr
