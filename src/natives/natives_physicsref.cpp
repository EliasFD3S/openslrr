#include "natives_internal.hpp"

#include "slrr/gameref.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace slrr {
namespace {

constexpr const char* kPhysicsRef = "java.util.resource.PhysicsRef";

float field(Object* object, const char* name) {
  if (!object) return 0.f;
  const auto found = object->fields.find(name);
  if (found == object->fields.end()) return 0.f;
  if (found->second.tag == ValueTag::Float) return found->second.v.f;
  if (found->second.tag == ValueTag::Int) return static_cast<float>(found->second.v.i);
  return 0.f;
}

float number(const std::vector<Value>& args, size_t index) {
  if (index >= args.size()) return 0.f;
  if (args[index].tag == ValueTag::Float) return args[index].v.f;
  if (args[index].tag == ValueTag::Int) return static_cast<float>(args[index].v.i);
  return 0.f;
}

Object* object_arg(const std::vector<Value>& args, size_t index) {
  return index < args.size() && args[index].tag == ValueTag::Obj ? args[index].v.o : nullptr;
}

std::string string_arg(const std::vector<Value>& args, size_t index,
                       const char* fallback) {
  Object* value = object_arg(args, index);
  return value ? value->str : fallback;
}

bool finite3(const float values[3]) {
  return std::isfinite(values[0]) && std::isfinite(values[1]) &&
         std::isfinite(values[2]);
}

GameRefState* create_body(Object* self, Object* parent, int32_t type,
                          PhysicsShape shape, const float size[3],
                          const std::string& alias) {
  if (!self || !size || !finite3(size)) return nullptr;
  GameRefState* state = gameref_upsert(self, GameRefKind::PhysicsRef);
  if (!state) return nullptr;
  gameref_set_parent(self, parent);
  state->script = self;
  state->class_name = self->class_name;
  state->type_id = type;
  state->alias = alias;
  state->empty = false;
  state->physics_shape = shape;
  std::copy(size, size + 3, state->physics_size);
  state->physics_static = false;
  state->collision_pending = false;
  state->collision_active = true;
  state->flags &= ~kCollisionQueued;
  std::fill(std::begin(state->pos), std::end(state->pos), 0.f);
  std::fill(std::begin(state->ypr), std::end(state->ypr), 0.f);
  std::fill(std::begin(state->vel), std::end(state->vel), 0.f);
  std::fill(std::begin(state->angular_vel), std::end(state->angular_vel), 0.f);
  return state;
}

Object* vector3(Jvm& jvm, float x, float y, float z) {
  Object* result = jvm.new_object("java.lang.Vector3");
  if (result) {
    result->fields["x"] = Value::make_float(x);
    result->fields["y"] = Value::make_float(y);
    result->fields["z"] = Value::make_float(z);
  }
  return result;
}

const GameRefState* body(Object* self) {
  GameRefState* state = gameref_get(self);
  return state && state->physics_shape != PhysicsShape::None ? state : nullptr;
}

}  // namespace

void physicsref_create_box(Object* self, Object* parent, float x, float y,
                           float z, const std::string& alias) {
  const float size[3] = {x * .5f, y * .5f, z * .5f};
  if (!(size[0] > 0.f) || !(size[1] > 0.f) || !(size[2] > 0.f)) return;
  create_body(self, parent, 0, PhysicsShape::Box, size, alias);
}

void physicsref_set_matrix(Object* self, Object* position, Object* orientation) {
  GameRefState* state = gameref_get(self);
  if (!state || state->physics_shape == PhysicsShape::None) return;
  state->pos[0] = field(position, "x");
  state->pos[1] = field(position, "y");
  state->pos[2] = field(position, "z");
  state->ypr[0] = field(orientation, "y");
  state->ypr[1] = field(orientation, "p");
  state->ypr[2] = field(orientation, "r");
  state->matrix_override = false;
  if (!finite3(state->pos) || !finite3(state->ypr)) {
    std::fill(std::begin(state->pos), std::end(state->pos), 0.f);
    std::fill(std::begin(state->ypr), std::end(state->ypr), 0.f);
  }
  if (state->physics_static) {
    std::fill(std::begin(state->vel), std::end(state->vel), 0.f);
    std::fill(std::begin(state->angular_vel), std::end(state->angular_vel), 0.f);
  }
}

void register_physicsref_natives(Jvm& jvm) {
  jvm.natives().register_method(
      kPhysicsRef, "create",
      "(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V",
      [](Object* self, const std::vector<Value>& args) {
        const float size[3] = {};
        const int32_t type = args.size() > 1 && args[1].tag == ValueTag::Int
                                 ? args[1].v.i
                                 : 0;
        create_body(self, object_arg(args, 0), type, PhysicsShape::Resource, size,
                    string_arg(args, 2, "physics"));
        return Value::make_void();
      });
  jvm.natives().register_method(
      kPhysicsRef, "createSphere",
      "(Ljava.util.resource.ResourceRef;FLjava.lang.String;)V",
      [](Object* self, const std::vector<Value>& args) {
        const float radius = number(args, 1);
        const float size[3] = {radius, 0.f, 0.f};
        if (radius > 0.f)
          create_body(self, object_arg(args, 0), 0, PhysicsShape::Sphere, size,
                      string_arg(args, 2, "physics_sphere"));
        return Value::make_void();
      });
  jvm.natives().register_method(
      kPhysicsRef, "setStatic", "(I)V",
      [](Object* self, const std::vector<Value>& args) {
        GameRefState* state = gameref_get(self);
        if (!state || state->physics_shape == PhysicsShape::None)
          return Value::make_void();
        state->physics_static = !args.empty() && args[0].tag == ValueTag::Int &&
                                args[0].v.i != 0;
        if (state->physics_static) {
          std::fill(std::begin(state->vel), std::end(state->vel), 0.f);
          std::fill(std::begin(state->angular_vel), std::end(state->angular_vel), 0.f);
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      kPhysicsRef, "getVel", "()Ljava.lang.Vector3;",
      [&jvm](Object* self, const std::vector<Value>&) {
        const GameRefState* state = body(self);
        if (!state || state->physics_static) return Value::make_obj(vector3(jvm, 0, 0, 0));
        return Value::make_obj(vector3(jvm, state->vel[0], state->vel[1], state->vel[2]));
      });
  jvm.natives().register_method(
      kPhysicsRef, "getVel", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;",
      [&jvm](Object* self, const std::vector<Value>& args) {
        const GameRefState* state = body(self);
        if (!state || state->physics_static) return Value::make_obj(vector3(jvm, 0, 0, 0));
        Object* point = object_arg(args, 0);
        if (!point) return Value::make_obj(nullptr);
        const float rx = field(point, "x") - state->pos[0];
        const float ry = field(point, "y") - state->pos[1];
        const float rz = field(point, "z") - state->pos[2];
        const float* w = state->angular_vel;
        return Value::make_obj(vector3(
            jvm, state->vel[0] + w[1] * rz - w[2] * ry,
            state->vel[1] + w[2] * rx - w[0] * rz,
            state->vel[2] + w[0] * ry - w[1] * rx));
      });
  jvm.natives().register_method(
      kPhysicsRef, "getAngVel", "()Ljava.lang.Vector3;",
      [&jvm](Object* self, const std::vector<Value>&) {
        const GameRefState* state = body(self);
        if (!state || state->physics_static) return Value::make_obj(vector3(jvm, 0, 0, 0));
        return Value::make_obj(vector3(jvm, state->angular_vel[0],
                                       state->angular_vel[1], state->angular_vel[2]));
      });
  jvm.natives().register_method(
      kPhysicsRef, "getOri", "()Ljava.lang.Ypr;",
      [&jvm](Object* self, const std::vector<Value>&) {
        Object* result = jvm.new_object("java.lang.Ypr");
        const GameRefState* state = body(self);
        if (result) {
          result->fields["y"] = Value::make_float(state ? state->ypr[0] : 0.f);
          result->fields["p"] = Value::make_float(state ? state->ypr[1] : 0.f);
          result->fields["r"] = Value::make_float(state ? state->ypr[2] : 0.f);
        }
        return Value::make_obj(result);
      });
}

}  // namespace slrr
