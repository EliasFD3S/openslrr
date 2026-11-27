// java.render.LightSource and its subclasses (#235). The Java classes are
// shipped in patch/system/Scripts/render; see docs/rendering/contract.md
// "Script light sources" for the contract.
#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/script_lights.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>

namespace slrr {
namespace {
// Vector3 fields, or nothing when the argument is missing or not finite.
std::optional<std::array<float, 3>> vector_arg(const std::vector<Value> &args,
                                               size_t i) {
  Object *object = native_arg_object(args, i);
  if (!object)
    return std::nullopt;
  std::array<float, 3> out{};
  const char *names[] = {"x", "y", "z"};
  for (size_t axis = 0; axis < 3; ++axis) {
    const auto field = object->fields.find(names[axis]);
    if (field == object->fields.end())
      return std::nullopt;
    const auto &value = field->second;
    if (value.tag == ValueTag::Int)
      out[axis] = float(value.v.i);
    else if (value.tag == ValueTag::Float && std::isfinite(value.v.f))
      out[axis] = value.v.f;
    else
      return std::nullopt;
  }
  return out;
}

std::optional<float> float_arg(const std::vector<Value> &args, size_t i) {
  if (i >= args.size())
    return std::nullopt;
  const auto &value = args[i];
  if (value.tag == ValueTag::Int)
    return float(value.v.i);
  if (value.tag == ValueTag::Float && std::isfinite(value.v.f))
    return value.v.f;
  return std::nullopt;
}

std::optional<int32_t> int_arg(const std::vector<Value> &args, size_t i) {
  if (i >= args.size() || args[i].tag != ValueTag::Int)
    return std::nullopt;
  return args[i].v.i;
}

using Setter = void (*)(ScriptLight &, const std::vector<Value> &);

// Calls on a destroyed or never-created light are ignored.
NativeFn setter(Setter apply) {
  return [apply](Object *self, const std::vector<Value> &args) {
    if (auto *light = script_lights_get(self))
      apply(*light, args);
    return Value::make_void();
  };
}
} // namespace

void register_light_natives(Jvm &jvm) {
  // A new VM starts a new world: drop lights of the previous one.
  script_lights_clear();
  jvm.natives().register_method(
      "java.render.LightSource", "create",
      "(Ljava.util.resource.ResourceRef;I)V",
      [](Object *self, const std::vector<Value> &args) {
        const auto kind = int_arg(args, 1);
        if (!self || !kind || *kind < 0 || *kind > 2) {
          std::fprintf(stderr, "[light] LightSource.create: invalid kind\n");
          return Value::make_void();
        }
        if (!script_lights_create(self, native_arg_object(args, 0),
                                  static_cast<ScriptLightKind>(*kind)))
          std::fprintf(stderr,
                       "[light] LightSource.create: parent has no GameRef\n");
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.LightSource", "setPosition", "(Ljava.lang.Vector3;)V",
      setter([](ScriptLight &light, const std::vector<Value> &args) {
        const auto v = vector_arg(args, 0);
        if (!v)
          return;
        for (size_t i = 0; i < 3; ++i)
          light.position[i] = std::clamp((*v)[i], -kScriptLightMaxPosition,
                                         kScriptLightMaxPosition);
      }));
  jvm.natives().register_method(
      "java.render.LightSource", "setDirection", "(Ljava.lang.Vector3;)V",
      setter([](ScriptLight &light, const std::vector<Value> &args) {
        const auto v = vector_arg(args, 0);
        if (!v)
          return;
        const float length = std::sqrt((*v)[0] * (*v)[0] + (*v)[1] * (*v)[1] +
                                       (*v)[2] * (*v)[2]);
        // A zero or overflowing vector keeps the old direction.
        if (!std::isfinite(length) || length < 1e-6f)
          return;
        for (size_t i = 0; i < 3; ++i)
          light.direction[i] = (*v)[i] / length;
      }));
  jvm.natives().register_method(
      "java.render.LightSource", "setColor", "(I)V",
      setter([](ScriptLight &light, const std::vector<Value> &args) {
        const auto rgb = int_arg(args, 0);
        if (!rgb)
          return;
        const auto bits = static_cast<uint32_t>(*rgb);
        light.color = {float((bits >> 16) & 255) / 255.f,
                       float((bits >> 8) & 255) / 255.f,
                       float(bits & 255) / 255.f};
      }));
  jvm.natives().register_method(
      "java.render.LightSource", "setIntensity", "(F)V",
      setter([](ScriptLight &light, const std::vector<Value> &args) {
        if (const auto v = float_arg(args, 0))
          light.intensity = std::clamp(*v, 0.f, kScriptLightMaxIntensity);
      }));
  jvm.natives().register_method(
      "java.render.LightSource", "setRange", "(F)V",
      setter([](ScriptLight &light, const std::vector<Value> &args) {
        if (const auto v = float_arg(args, 0))
          light.range =
              std::clamp(*v, kScriptLightMinRange, kScriptLightMaxRange);
      }));
  jvm.natives().register_method(
      "java.render.LightSource", "setShadows", "(I)V",
      setter([](ScriptLight &light, const std::vector<Value> &args) {
        if (const auto v = int_arg(args, 0))
          light.shadows = *v != 0;
      }));
  jvm.natives().register_method(
      "java.render.LightSource", "setEnabled", "(I)V",
      setter([](ScriptLight &light, const std::vector<Value> &args) {
        if (const auto v = int_arg(args, 0))
          light.enabled = *v != 0;
      }));
  jvm.natives().register_method("java.render.LightSource", "destroy", "()V",
                                [](Object *self, const std::vector<Value> &) {
                                  script_lights_destroy(self);
                                  return Value::make_void();
                                });
  jvm.natives().register_method(
      "java.render.SpotLight", "setCone", "(FF)V",
      setter([](ScriptLight &light, const std::vector<Value> &args) {
        const auto inner = float_arg(args, 0);
        const auto outer = float_arg(args, 1);
        if (!inner || !outer)
          return;
        light.outer_degrees = std::clamp(*outer, 1.f, 89.f);
        light.inner_degrees = std::clamp(*inner, 0.f, light.outer_degrees);
      }));
  jvm.natives().register_method(
      "java.render.RectLight", "setSize", "(FF)V",
      setter([](ScriptLight &light, const std::vector<Value> &args) {
        const auto w = float_arg(args, 0);
        const auto h = float_arg(args, 1);
        if (!w || !h)
          return;
        light.width = std::clamp(*w, 0.f, kScriptLightMaxSize);
        light.height = std::clamp(*h, 0.f, kScriptLightMaxSize);
      }));
}
} // namespace slrr
