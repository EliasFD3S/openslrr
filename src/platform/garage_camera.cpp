#include "slrr/garage_camera.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef SLRR_HAS_LIBBGFX
#include <bx/math.h>
#endif

namespace slrr {
namespace {

GarageCamera g_cam;

#ifndef SLRR_HAS_LIBBGFX
// Minimal lookAt + perspective when libbgfx/bx is off (WGL path).
void mtx_look_at(float* out, float ex, float ey, float ez, float ax, float ay,
                 float az) {
  float fx = ax - ex, fy = ay - ey, fz = az - ez;
  float len = std::sqrt(fx * fx + fy * fy + fz * fz);
  if (len < 1e-6f) len = 1.f;
  fx /= len;
  fy /= len;
  fz /= len;
  float ux = 0.f, uy = 1.f, uz = 0.f;
  float sx = fy * uz - fz * uy;
  float sy = fz * ux - fx * uz;
  float sz = fx * uy - fy * ux;
  float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
  if (sl < 1e-6f) sl = 1.f;
  sx /= sl;
  sy /= sl;
  sz /= sl;
  float ux2 = sy * fz - sz * fy;
  float uy2 = sz * fx - sx * fz;
  float uz2 = sx * fy - sy * fx;
  out[0] = sx;
  out[1] = ux2;
  out[2] = -fx;
  out[3] = 0;
  out[4] = sy;
  out[5] = uy2;
  out[6] = -fy;
  out[7] = 0;
  out[8] = sz;
  out[9] = uz2;
  out[10] = -fz;
  out[11] = 0;
  out[12] = -(sx * ex + sy * ey + sz * ez);
  out[13] = -(ux2 * ex + uy2 * ey + uz2 * ez);
  out[14] = -(-fx * ex - fy * ey - fz * ez);
  out[15] = 1;
}

void mtx_proj(float* out, float fovy_deg, float aspect, float zn, float zf) {
  const float f = 1.f / std::tan(fovy_deg * 0.5f * 3.14159265f / 180.f);
  for (int i = 0; i < 16; ++i) out[i] = 0.f;
  out[0] = f / aspect;
  out[5] = f;
  out[10] = (zf + zn) / (zn - zf);
  out[11] = -1.f;
  out[14] = (2.f * zf * zn) / (zn - zf);
}
#endif

}  // namespace

GarageCamera& garage_camera() { return g_cam; }

// Critically damped chase. `blend` is the stock per-frame fraction at 60 Hz
// (Garage.cameraSetup "smooth 0.05 0.5": position lags, orientation is tighter).
void spring_to(float& cur, float& vel, float goal, float dt, float blend) {
  blend = std::max(0.001f, std::min(blend, 0.95f));
  const float omega = -std::log(1.f - blend) * 60.f;
  const float err = goal - cur;
  vel += (err * omega * omega - 2.f * omega * vel) * dt;
  cur += vel * dt;
}

bool cmd_is(const char* p, const char* word) {
  const size_t n = std::strlen(word);
  if (std::strncmp(p, word, n) != 0) return false;
  return p[n] == '\0' || p[n] == ' ' || p[n] == '\t';
}

int read_floats(const char* p, float* out, int n) {
  int got = 0;
  while (got < n && *p) {
    while (*p && *p != '-' && *p != '+' && *p != '.' &&
           (*p < '0' || *p > '9'))
      ++p;
    if (!*p) break;
    char* end = nullptr;
    out[got] = std::strtof(p, &end);
    if (end == p) break;
    p = end;
    ++got;
  }
  return got;
}

void garage_camera_setup_defaults() {
  g_cam.active = true;
  g_cam.look_x = 0.f;
  g_cam.look_y = 0.f;
  g_cam.look_z = -0.5f;  // Garage.defLookPos
  // Derive orbit from stock cameraPos (-3, 1.5, -2) → look.
  // (cameraOri pitch is look direction, NOT elevation — using it put eye under floor.)
  const float ox = -3.f - g_cam.look_x;
  const float oy = 1.5f - g_cam.look_y;
  const float oz = -2.f - g_cam.look_z;
  g_cam.dist = std::sqrt(ox * ox + oy * oy + oz * oz);
  if (g_cam.dist < 0.1f) g_cam.dist = 3.5f;
  g_cam.dist =
      std::max(g_cam.dist_min, std::min(g_cam.dist_max, g_cam.dist));
  g_cam.yaw = std::atan2(ox, oz);
  g_cam.pitch = std::asin(std::max(-1.f, std::min(1.f, oy / g_cam.dist)));
  g_cam.target_yaw = g_cam.yaw;
  g_cam.target_pitch = g_cam.pitch;
  g_cam.target_dist = g_cam.dist;
  g_cam.yaw_vel = 0.f;
  g_cam.pitch_vel = 0.f;
  g_cam.dist_vel = 0.f;
}

void garage_camera_command(const char* line) {
  if (!line || !line[0]) return;
  const char* p = line;
  while (*p == ' ' || *p == '\t') ++p;
  float v[3];
  if (cmd_is(p, "smooth")) {
    if (read_floats(p, v, 2) >= 2) {
      g_cam.smooth_pos = v[0];
      g_cam.smooth_ori = v[1];
    }
  } else if (cmd_is(p, "dist")) {
    if (read_floats(p, v, 2) >= 2) {
      g_cam.dist_min = v[0];
      g_cam.dist_max = v[1];
      if (g_cam.dist_max < g_cam.dist_min)
        std::swap(g_cam.dist_min, g_cam.dist_max);
      g_cam.target_dist = std::max(
          g_cam.dist_min, std::min(g_cam.dist_max, g_cam.target_dist));
      g_cam.dist =
          std::max(g_cam.dist_min, std::min(g_cam.dist_max, g_cam.dist));
    }
  } else if (cmd_is(p, "zoom")) {
    const int n = read_floats(p, v, 2);
    if (n >= 1 && v[0] > 1.f) g_cam.fov_deg = v[0];
    if (n >= 2) g_cam.zoom_speed = v[1];
  } else if (cmd_is(p, "force")) {
    if (read_floats(p, v, 3) >= 3) {
      g_cam.force = v[0];
      g_cam.damping = v[1];
      g_cam.perp_damping = v[2];
    }
  } else if (cmd_is(p, "torque")) {
    if (read_floats(p, v, 1) >= 1) g_cam.torque = v[0];
  }
}

void garage_camera_tick(float dt, float look_yaw, float look_pitch,
                        float look_zoom) {
  if (!g_cam.active) return;
  if (dt <= 0.f) dt = 1.f / 60.f;
  constexpr float kMouseScale = 2.5f;
  // Mouse moves the target. The eye eases after it (stock position smooth).
  g_cam.target_yaw += look_yaw * kMouseScale;
  g_cam.target_pitch += look_pitch * kMouseScale;
  constexpr float kPitchMin = 0.08f;
  constexpr float kPitchMax = 1.40f;
  g_cam.target_pitch =
      std::max(kPitchMin, std::min(kPitchMax, g_cam.target_pitch));
  g_cam.target_dist -= look_zoom * g_cam.zoom_speed * 0.15f;
  g_cam.target_dist = std::max(
      g_cam.dist_min, std::min(g_cam.dist_max, g_cam.target_dist));
  // Mouse orbit uses the orientation blend (garage "smooth" second value,
  // 0.5). The first value is the chase-cam position lag, used for the dolly.
  const float ori = g_cam.smooth_ori > 0.f ? g_cam.smooth_ori : 0.5f;
  const float pos = g_cam.smooth_pos > 0.f ? g_cam.smooth_pos : 0.05f;
  spring_to(g_cam.yaw, g_cam.yaw_vel, g_cam.target_yaw, dt, ori);
  spring_to(g_cam.pitch, g_cam.pitch_vel, g_cam.target_pitch, dt, ori);
  spring_to(g_cam.dist, g_cam.dist_vel, g_cam.target_dist, dt, pos);
  g_cam.pitch = std::max(kPitchMin, std::min(kPitchMax, g_cam.pitch));
  g_cam.dist =
      std::max(g_cam.dist_min, std::min(g_cam.dist_max, g_cam.dist));
}

void garage_camera_matrices(float* view, float* proj, float aspect,
                            bool homogeneous_depth) {
  const float cy = std::cos(g_cam.yaw);
  const float sy = std::sin(g_cam.yaw);
  const float cp = std::cos(g_cam.pitch);
  const float sp = std::sin(g_cam.pitch);
  // Yaw around Y, pitch around X — eye orbits look_at.
  const float ex =
      g_cam.look_x + g_cam.dist * cp * sy;
  const float ey = g_cam.look_y + g_cam.dist * sp;
  const float ez =
      g_cam.look_z + g_cam.dist * cp * cy;

#ifdef SLRR_HAS_LIBBGFX
  const bx::Vec3 eye = {ex, ey, ez};
  const bx::Vec3 at = {g_cam.look_x, g_cam.look_y, g_cam.look_z};
  const bx::Vec3 up = {0.f, 1.f, 0.f};
  bx::mtxLookAt(view, eye, at, up);
  bx::mtxProj(proj, g_cam.fov_deg, aspect, 0.05f, 200.f, homogeneous_depth);
#else
  (void)homogeneous_depth;
  mtx_look_at(view, ex, ey, ez, g_cam.look_x, g_cam.look_y, g_cam.look_z);
  mtx_proj(proj, g_cam.fov_deg, aspect > 0.f ? aspect : 1.333f, 0.05f, 200.f);
#endif
}

}  // namespace slrr
