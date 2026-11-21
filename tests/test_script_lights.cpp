// java.render.LightSource registry and natives (#235), run through the
// shipped patch classes so constructor chains and inherited natives are real.
#include "natives_internal.hpp"
#include "slrr/gameref.hpp"
#include "slrr/script_lights.hpp"
#include "standalone_test.hpp"

#include <cmath>
#include <limits>

using namespace slrr;
namespace {
struct Fixture {
  NativeRegistry registry;
  Jvm vm{&registry};
  Object parent, other;
  Fixture() {
    gameref_clear();
    register_light_natives(vm);
    for (const char *name :
         {"LightSource", "PointLight", "SpotLight", "RectLight"})
      SLRR_REQUIRE(vm.load_class_file(
          std::string("patch/system/Scripts/render/") + name + ".class"));
    gameref_upsert(&parent, GameRefKind::RenderRef)->empty = false;
    gameref_upsert(&other, GameRefKind::RenderRef)->empty = false;
  }
  ~Fixture() { gameref_clear(); }
  Object *make(const char *cls, Object *owner) {
    Object *light = vm.new_object(std::string("java.render.") + cls);
    SLRR_REQUIRE(light);
    vm.invoke(std::string("java.render.") + cls, "<init>",
              "(Ljava.util.resource.ResourceRef;)V",
              {Value::make_obj(light), Value::make_obj(owner)}, false);
    return light;
  }
  void call(Object *light, const char *method, const char *signature,
            std::vector<Value> args) {
    args.insert(args.begin(), Value::make_obj(light));
    vm.invoke(light->class_name, method, signature, args, false);
  }
  Object *vector(float x, float y, float z) {
    Object *v = vm.new_object("java.lang.Vector3");
    v->fields["x"] = Value::make_float(x);
    v->fields["y"] = Value::make_float(y);
    v->fields["z"] = Value::make_float(z);
    return v;
  }
};

SLRR_TEST(constructors_and_inherited_natives) {
  Fixture f;
  Object *spot = f.make("SpotLight", &f.parent);
  Object *rect = f.make("RectLight", &f.parent);
  Object *point = f.make("PointLight", &f.parent);
  SLRR_REQUIRE(script_lights_count() == 3);
  SLRR_REQUIRE(script_lights_get(spot)->kind == ScriptLightKind::Spot);
  SLRR_REQUIRE(script_lights_get(rect)->kind == ScriptLightKind::Rect);
  SLRR_REQUIRE(script_lights_get(point)->kind == ScriptLightKind::Point);
  SLRR_REQUIRE(script_lights_get(spot)->parent == &f.parent);
  // LightSource natives reached through SpotLight and RectLight receivers.
  f.call(rect, "setPosition", "(Ljava.lang.Vector3;)V",
         {Value::make_obj(f.vector(1, 6.5f, -2))});
  f.call(rect, "setDirection", "(Ljava.lang.Vector3;)V",
         {Value::make_obj(f.vector(0, -4, 0))});
  f.call(rect, "setColor", "(I)V", {Value::make_int(0xFF8000)});
  f.call(rect, "setIntensity", "(F)V", {Value::make_float(12)});
  f.call(rect, "setRange", "(F)V", {Value::make_float(15)});
  f.call(rect, "setShadows", "(I)V", {Value::make_int(0)});
  f.call(rect, "setCone", "(FF)V",
         {Value::make_float(30), Value::make_float(70)});
  f.call(rect, "setSize", "(FF)V",
         {Value::make_float(1.2f), Value::make_float(.3f)});
  const auto *light = script_lights_get(rect);
  SLRR_REQUIRE(light->position == (std::array<float, 3>{1, 6.5f, -2}));
  SLRR_REQUIRE(light->direction == (std::array<float, 3>{0, -1, 0}));
  SLRR_REQUIRE(light->color[0] == 1 &&
               std::abs(light->color[1] - 128 / 255.f) < 1e-6f &&
               light->color[2] == 0);
  SLRR_REQUIRE(light->intensity == 12 && light->range == 15 && !light->shadows);
  SLRR_REQUIRE(light->inner_degrees == 30 && light->outer_degrees == 70);
  SLRR_REQUIRE(light->width == 1.2f && light->height == .3f);
  std::puts(
      "PASS SpotLight/RectLight/PointLight constructors and inherited natives");
}

SLRR_TEST(input_validation) {
  Fixture f;
  Object *spot = f.make("SpotLight", &f.parent);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  f.call(spot, "setPosition", "(Ljava.lang.Vector3;)V",
         {Value::make_obj(f.vector(nan, 1, 1))});
  f.call(spot, "setDirection", "(Ljava.lang.Vector3;)V",
         {Value::make_obj(f.vector(0, 0, 0))});
  f.call(spot, "setIntensity", "(F)V", {Value::make_float(inf)});
  f.call(spot, "setRange", "(F)V", {Value::make_float(nan)});
  f.call(spot, "setPosition", "(Ljava.lang.Vector3;)V",
         {Value::make_obj(nullptr)});
  const auto *light = script_lights_get(spot);
  SLRR_REQUIRE(light->position == (std::array<float, 3>{0, 0, 0}));
  SLRR_REQUIRE(light->direction == (std::array<float, 3>{0, -1, 0}));
  SLRR_REQUIRE(light->intensity == 24 && light->range == 20);
  f.call(spot, "setIntensity", "(F)V", {Value::make_float(-5)});
  f.call(spot, "setRange", "(F)V", {Value::make_float(1e9f)});
  f.call(spot, "setCone", "(FF)V",
         {Value::make_float(120), Value::make_float(400)});
  SLRR_REQUIRE(light->intensity == 0 && light->range == kScriptLightMaxRange);
  SLRR_REQUIRE(light->outer_degrees == 89 && light->inner_degrees == 89);
  // A parent without GameRef state creates nothing.
  Object orphan;
  f.make("SpotLight", &orphan);
  f.make("PointLight", nullptr);
  SLRR_REQUIRE(script_lights_count() == 1);
  std::puts("PASS light natives ignore non-finite input and clamp ranges");
}

SLRR_TEST(lifetime) {
  Fixture f;
  Object *a = f.make("SpotLight", &f.parent);
  Object *b = f.make("PointLight", &f.parent);
  Object *c = f.make("SpotLight", &f.other);
  SLRR_REQUIRE(script_lights_count() == 3);
  // Creation order is kept for a stable renderer budget.
  const auto all = script_lights_all();
  SLRR_REQUIRE(all.size() == 3 && all[0]->key == a && all[1]->key == b &&
               all[2]->key == c);
  // destroy() removes one light; later calls on it are ignored.
  f.call(a, "destroy", "()V", {});
  SLRR_REQUIRE(!script_lights_get(a) && script_lights_count() == 2);
  f.call(a, "setIntensity", "(F)V", {Value::make_float(3)});
  SLRR_REQUIRE(!script_lights_get(a));
  // Removing the parent GameRef removes its lights only.
  gameref_remove(&f.parent);
  SLRR_REQUIRE(!script_lights_get(b) && script_lights_get(c));
  // Registered lights are GC roots until released.
  size_t rooted = 0;
  script_lights_visit_gc_roots([&](const void *key) { rooted += key == c; });
  SLRR_REQUIRE(rooted == 1);
  // A world reset (gameref_clear, or a new VM registering natives) clears all.
  gameref_clear();
  SLRR_REQUIRE(script_lights_count() == 0);
  gameref_upsert(&f.other, GameRefKind::RenderRef)->empty = false;
  f.make("SpotLight", &f.other);
  NativeRegistry fresh;
  Jvm next{&fresh};
  register_light_natives(next);
  SLRR_REQUIRE(script_lights_count() == 0);
  std::puts(
      "PASS light lifetime: destroy, parent removal, GC roots, world reset");
}
} // namespace

int main() {
  const standalone::Test tests[] = {constructors_and_inherited_natives,
                                    input_validation, lifetime};
  return standalone::run("script_lights", tests);
}
