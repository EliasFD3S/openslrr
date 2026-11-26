// java.util.resource.ParticleSystem (#454): the stock emitter natives on the
// host's particle pool (script_particles.hpp).
#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/script_particles.hpp"
#include "slrr/surface_effects.hpp"

#include <string>

namespace slrr {
namespace {

constexpr const char *kParticleSystem = "java.util.resource.ParticleSystem";

std::string arg_string(const std::vector<Value> &args, size_t i) {
  Object *text = native_arg_object(args, i);
  return text ? text->str : std::string{};
}

Vec3f arg_vector(const std::vector<Value> &args, size_t i) {
  Vec3f out{};
  Object *v = native_arg_object(args, i);
  if (!v) return out;
  const char *names[] = {"x", "y", "z"};
  for (size_t k = 0; k < 3; ++k) {
    const auto field = v->fields.find(names[k]);
    if (field == v->fields.end()) continue;
    if (field->second.tag == ValueTag::Float && std::isfinite(field->second.v.f))
      out[k] = field->second.v.f;
    else if (field->second.tag == ValueTag::Int)
      out[k] = static_cast<float>(field->second.v.i);
  }
  return out;
}

int32_t type_id_of(Object *type) {
  if (!type) return 0;
  if (GameRefState *state = gameref_get(type); state && state->type_id) return state->type_id;
  const auto it = type->fields.find("id");
  return it != type->fields.end() && it->second.tag == ValueTag::Int ? it->second.v.i : 0;
}

}  // namespace

void register_particle_natives(Jvm &jvm, RpkIndex &rpk) {
  auto &natives = jvm.natives();
  // init(parent, type, alias): the type is the RPK `particle <cfg>` entry.
  // The script object gets its id (ResourceRef.id) but no render type, so
  // the entry's SMOKE.SCX mesh is not drawn as a scene object.
  natives.register_method(
      kParticleSystem, "init",
      "(Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;Ljava.lang.String;)V",
      [&rpk](Object *self, const std::vector<Value> &args) {
        if (!self) return Value::make_void();
        Object *parent = native_arg_object(args, 0);
        const int32_t type = type_id_of(native_arg_object(args, 1));
        self->fields["id"] = Value::make_int(type);
        const uint16_t cloud =
            type ? surface_particle_cloud(rpk, static_cast<uint32_t>(type)) : uint16_t(0xffff);
        script_particles_init(self, parent, cloud);
        return Value::make_void();
      });
  natives.register_method(kParticleSystem, "stop", "()V",
                          [](Object *self, const std::vector<Value> &) {
                            script_particles_stop(self);
                            return Value::make_void();
                          });
  natives.register_method(kParticleSystem, "setFreq", "(F)V",
                          [](Object *self, const std::vector<Value> &args) {
                            script_particles_freq(self, native_arg_number(args, 0, 1.f));
                            return Value::make_void();
                          });
  const auto source = [](bool direct) {
    return [direct](Object *self, const std::vector<Value> &args) {
      // (alias, pos, rmin, rmax, vel, vmin, vmax, freq or num, bone)
      script_particles_source(self, arg_string(args, 0), arg_vector(args, 1),
                              native_arg_number(args, 2), native_arg_number(args, 3),
                              arg_vector(args, 4), native_arg_number(args, 5),
                              native_arg_number(args, 6), native_arg_number(args, 7), direct);
      return Value::make_void();
    };
  };
  constexpr const char *kSourceSignature =
      "(Ljava.lang.String;Ljava.lang.Vector3;FFLjava.lang.Vector3;FFFLjava.lang.String;)V";
  natives.register_method(kParticleSystem, "setDirectSource", kSourceSignature, source(true));
  natives.register_method(kParticleSystem, "setSource", kSourceSignature, source(false));
  natives.register_method(
      kParticleSystem, "setCounter", "(Ljava.lang.String;Ljava.lang.Vector3;F)V",
      [](Object *self, const std::vector<Value> &args) {
        script_particles_counter(self, arg_string(args, 0), arg_vector(args, 1),
                                 native_arg_number(args, 2));
        return Value::make_void();
      });
  natives.register_method(kParticleSystem, "getCounter", "(Ljava.lang.String;)I",
                          [](Object *self, const std::vector<Value> &args) {
                            return Value::make_int(script_particles_count(self, arg_string(args, 0)));
                          });
  natives.register_method(kParticleSystem, "delAction", "(Ljava.lang.String;)V",
                          [](Object *self, const std::vector<Value> &args) {
                            script_particles_delete(self, arg_string(args, 0));
                            return Value::make_void();
                          });
  natives.register_method(kParticleSystem, "modePermanent", "(I)V",
                          [](Object *self, const std::vector<Value> &args) {
                            script_particles_permanent(self, native_arg_integer(args, 0) != 0);
                            return Value::make_void();
                          });
}

}  // namespace slrr
