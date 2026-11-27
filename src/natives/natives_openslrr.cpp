// java.openslrr natives (#487): the versioned mod API that camera mods use
// instead of replacing java.game.Track. The declaring classes ship in the
// patch (patch/system/Scripts/openslrr); see docs/scripting/mod-api.md.
#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/mod_api.hpp"
#include "slrr/mod_camera.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/scene_instances.hpp"

#include <algorithm>
#include <cmath>

namespace slrr {
namespace {
constexpr const char* kOpenSlrr = "java.openslrr.OpenSLRR";
constexpr const char* kCarState = "java.openslrr.CarState";
constexpr const char* kCameraControl = "java.openslrr.CameraControl";

// A Vector3 / Ypr argument's three float fields; false when absent or not finite.
bool read3(Object* o, const char* a, const char* b, const char* c, float* out) {
  if (!o) return false;
  const char* names[3] = {a, b, c};
  for (int i = 0; i < 3; ++i) {
    const auto it = o->fields.find(names[i]);
    if (it == o->fields.end()) return false;
    if (it->second.tag == ValueTag::Float) out[i] = it->second.v.f;
    else if (it->second.tag == ValueTag::Int) out[i] = static_cast<float>(it->second.v.i);
    else return false;
    if (!std::isfinite(out[i])) return false;
  }
  return true;
}
bool read_vector(const std::vector<Value>& args, size_t i, float* out) {
  return read3(native_arg_object(args, i), "x", "y", "z", out);
}
// The up argument: null is world up; any other value must be a finite
// Vector3, else the whole pose is rejected (a zero vector is still world up,
// resolved by mod_camera_resolve).
bool read_up(const std::vector<Value>& args, size_t i, float* out) {
  if (!native_arg_object(args, i)) {
    out[0] = 0.f, out[1] = 1.f, out[2] = 0.f;
    return true;
  }
  return read_vector(args, i, out);
}
// setPose / setPoseRelative / setPoseLocal: cam, [car,] eye, at, up, fov.
// A non-finite or missing eye, focus, up or fov leaves the previous pose.
void set_pose(const std::vector<Value>& args, ModCameraMode mode) {
  const size_t first = mode == ModCameraMode::World ? 1 : 2;
  float eye[3], at[3], up[3];
  if (!read_vector(args, first, eye) || !read_vector(args, first + 1, at) ||
      !read_up(args, first + 2, up))
    return;
  const float fov = native_arg_number(args, first + 3, 0.f);
  if (!std::isfinite(fov)) return;
  gameref_mod_camera_set(native_arg_object(args, 0), mode, eye, at, up, fov,
                         mode == ModCameraMode::World ? nullptr : native_arg_object(args, 1));
}
Value new_triple(Jvm& jvm, const char* cls, const char* a, const char* b, const char* c,
                 const float* v) {
  Object* o = jvm.new_object(cls);
  if (!o) return Value::make_obj(nullptr);
  o->fields[a] = Value::make_float(v[0]);
  o->fields[b] = Value::make_float(v[1]);
  o->fields[c] = Value::make_float(v[2]);
  return Value::make_obj(o);
}
Value new_vector(Jvm& jvm, const float* v) { return new_triple(jvm, "java.lang.Vector3", "x", "y", "z", v); }
// Scripts get null when the GameRef is not a live car.
Value null_object() { return Value::make_obj(nullptr); }

// The pose drawn this frame: the car between its last two physics steps,
// the same frame the chase camera follows (part_camera_pose).
bool drawn_frame(Object* car, float* frame) { return car && part_camera_pose(car, -1, frame); }
}  // namespace

void register_openslrr_natives(Jvm& jvm) {
  auto& natives = jvm.natives();
  natives.register_method(kOpenSlrr, "apiVersion", "()I",
      [](Object*, const std::vector<Value>&) { return Value::make_int(kModApiVersion); });

  // --- CarState: read-only car state for mods (units: m, m/s, rad/s) ---
  natives.register_method(kCarState, "drawnPos", "(Ljava.util.resource.GameRef;)Ljava.lang.Vector3;",
      [&jvm](Object*, const std::vector<Value>& args) {
        float frame[16];
        if (!drawn_frame(native_arg_object(args, 0), frame)) return null_object();
        return new_vector(jvm, frame + 12);
      });
  natives.register_method(kCarState, "drawnOri", "(Ljava.util.resource.GameRef;)Ljava.lang.Ypr;",
      [&jvm](Object*, const std::vector<Value>& args) {
        float frame[16], ypr[3];
        if (!drawn_frame(native_arg_object(args, 0), frame)) return null_object();
        mod_matrix_ypr(frame, ypr);
        return new_triple(jvm, "java.lang.Ypr", "y", "p", "r", ypr);
      });
  natives.register_method(kCarState, "vel", "(Ljava.util.resource.GameRef;)Ljava.lang.Vector3;",
      [&jvm](Object*, const std::vector<Value>& args) {
        float v[3];
        if (!part_car_velocity(native_arg_object(args, 0), v)) return null_object();
        return new_vector(jvm, v);
      });
  natives.register_method(kCarState, "angVel", "(Ljava.util.resource.GameRef;)Ljava.lang.Vector3;",
      [&jvm](Object*, const std::vector<Value>& args) {
        float w[3];
        if (!part_car_angular_velocity(native_arg_object(args, 0), w)) return null_object();
        return new_vector(jvm, w);
      });
  natives.register_method(kCarState, "speed", "(Ljava.util.resource.GameRef;)F",
      [](Object*, const std::vector<Value>& args) {
        float v[3];
        if (!part_car_velocity(native_arg_object(args, 0), v)) return Value::make_float(0.f);
        return Value::make_float(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
      });
  natives.register_method(kCarState, "nitro", "(Ljava.util.resource.GameRef;)I",
      [](Object*, const std::vector<Value>& args) {
        PartPhysicsState state;
        Object* car = native_arg_object(args, 0);
        return Value::make_int(car && part_physics_state(car, state) && state.nitro_active ? 1 : 0);
      });
  natives.register_method(kCarState, "wheelsOnGround", "(Ljava.util.resource.GameRef;)I",
      [](Object*, const std::vector<Value>& args) {
        PartPhysicsState state;
        Object* car = native_arg_object(args, 0);
        if (!car || !part_physics_state(car, state) || !state.active) return Value::make_int(0);
        return Value::make_int(static_cast<int32_t>(
            std::count(state.contact.begin(), state.contact.end(), true)));
      });
  natives.register_method(kCarState, "boxMin", "(Ljava.util.resource.GameRef;)Ljava.lang.Vector3;",
      [&jvm](Object*, const std::vector<Value>& args) {
        float lo[3], hi[3];
        if (!part_car_local_box(native_arg_object(args, 0), lo, hi)) return null_object();
        return new_vector(jvm, lo);
      });
  natives.register_method(kCarState, "boxMax", "(Ljava.util.resource.GameRef;)Ljava.lang.Vector3;",
      [&jvm](Object*, const std::vector<Value>& args) {
        float lo[3], hi[3];
        if (!part_car_local_box(native_arg_object(args, 0), lo, hi)) return null_object();
        return new_vector(jvm, hi);
      });

  // --- CameraControl: exact poses on a script camera (RID_CAMERA) ---
  natives.register_method(kCameraControl, "setPose",
      "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;Ljava.lang.Vector3;Ljava.lang.Vector3;F)V",
      [](Object*, const std::vector<Value>& args) {
        set_pose(args, ModCameraMode::World);
        return Value::make_void();
      });
  natives.register_method(kCameraControl, "setPoseRelative",
      "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;Ljava.lang.Vector3;"
      "Ljava.lang.Vector3;Ljava.lang.Vector3;F)V",
      [](Object*, const std::vector<Value>& args) {
        set_pose(args, ModCameraMode::Relative);
        return Value::make_void();
      });
  natives.register_method(kCameraControl, "setPoseLocal",
      "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;Ljava.lang.Vector3;"
      "Ljava.lang.Vector3;Ljava.lang.Vector3;F)V",
      [](Object*, const std::vector<Value>& args) {
        set_pose(args, ModCameraMode::Local);
        return Value::make_void();
      });
  natives.register_method(kCameraControl, "clearPose", "(Ljava.util.resource.GameRef;)V",
      [](Object*, const std::vector<Value>& args) {
        gameref_mod_camera_clear(native_arg_object(args, 0));
        return Value::make_void();
      });
  natives.register_method(kCameraControl, "sweep", "(Ljava.lang.Vector3;Ljava.lang.Vector3;F)F",
      [](Object*, const std::vector<Value>& args) {
        float from[3], to[3];
        const float radius = native_arg_number(args, 2, .15f);
        if (!read_vector(args, 0, from) || !read_vector(args, 1, to) || !(radius >= 0.f))
          return Value::make_float(1.f);
        const float delta[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
        SceneSphereHit hit;
        if (!scene_instances_sweep_sphere(from, delta, std::min(radius, 2.f), hit))
          return Value::make_float(1.f);
        return Value::make_float(std::clamp(hit.fraction, 0.f, 1.f));
      });
}

}  // namespace slrr
