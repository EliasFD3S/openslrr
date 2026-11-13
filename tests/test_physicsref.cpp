#include "standalone_test.hpp"

#include "natives_internal.hpp"
#include "slrr/gameref.hpp"
#include "slrr/script_events.hpp"
#include "slrr/script_threads.hpp"

#include <cmath>
#include <unordered_map>

// Native-only fixture: the PhysicsRef bindings use the production JVM and
// registry, while this small GameRef map avoids pulling in scene/render code.
namespace slrr {
namespace {
std::unordered_map<void*, GameRefState> states;
}

GameRefState* gameref_upsert(void* key, GameRefKind kind) {
  if (!key) return nullptr;
  auto [it, added] = states.try_emplace(key);
  if (added) {
    it->second.key = key;
    it->second.kind = kind;
    it->second.empty = false;
  } else if (kind != GameRefKind::Generic) {
    it->second.kind = kind;
  }
  return &it->second;
}

GameRefState* gameref_get(void* key) {
  const auto found = states.find(key);
  return found == states.end() ? nullptr : &found->second;
}

void gameref_set_parent(void* key, void* parent) {
  if (auto* state = gameref_upsert(key)) state->parent = parent;
}

void script_threads_shutdown(Jvm&) {}
void script_events_shutdown(Jvm&) {}
bool script_thread_active(Jvm&) { return false; }
void host_loading_present() {}
void host_input_forget(Object*) {}
uint32_t rpk_resolve_resource(const std::string&, uint32_t local) { return local; }
TreeEvalResult tree_eval(TreeHost*, const TufaClass&, const TufaMember&, Object*,
                         const std::vector<Value>&) {
  return {false, {}, "native-only PhysicsRef fixture"};
}
}  // namespace slrr

namespace {

constexpr const char* kPhysicsRef = "java.util.resource.PhysicsRef";

struct Fixture {
  slrr::NativeRegistry registry;
  slrr::Jvm jvm{&registry};
  slrr::Object self;
  slrr::Object parent;

  Fixture() {
    slrr::states.clear();
    self.class_name = kPhysicsRef;
    slrr::register_physicsref_natives(jvm);
  }
  ~Fixture() { slrr::states.clear(); }

  slrr::GameRefState& state() {
    auto* value = slrr::gameref_get(&self);
    SLRR_REQUIRE(value != nullptr);
    return *value;
  }

  slrr::Value invoke(const char* name, const char* descriptor,
                     const std::vector<slrr::Value>& args = {}) {
    return registry.invoke(kPhysicsRef, name, descriptor, &self, args);
  }
};

float field(slrr::Object* object, const char* name) {
  SLRR_REQUIRE(object != nullptr);
  const auto found = object->fields.find(name);
  SLRR_REQUIRE(found != object->fields.end());
  SLRR_REQUIRE(found->second.tag == slrr::ValueTag::Float);
  return found->second.v.f;
}

slrr::Object* object(const slrr::Value& value) {
  SLRR_REQUIRE(value.tag == slrr::ValueTag::Obj);
  return value.v.o;
}

slrr::Object* vector(slrr::Jvm& jvm, float x, float y, float z) {
  auto* result = jvm.new_object("java.lang.Vector3");
  result->fields["x"] = slrr::Value::make_float(x);
  result->fields["y"] = slrr::Value::make_float(y);
  result->fields["z"] = slrr::Value::make_float(z);
  return result;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

}  // namespace

SLRR_TEST(physicsref_registers_all_remaining_stock_descriptors) {
  Fixture fixture;
  SLRR_REQUIRE(fixture.registry.has(kPhysicsRef, "create",
      "(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V"));
  SLRR_REQUIRE(fixture.registry.has(kPhysicsRef, "createSphere",
      "(Ljava.util.resource.ResourceRef;FLjava.lang.String;)V"));
  SLRR_REQUIRE(fixture.registry.has(kPhysicsRef, "setStatic", "(I)V"));
  SLRR_REQUIRE(fixture.registry.has(kPhysicsRef, "getVel", "()Ljava.lang.Vector3;"));
  SLRR_REQUIRE(fixture.registry.has(kPhysicsRef, "getVel",
      "(Ljava.lang.Vector3;)Ljava.lang.Vector3;"));
  SLRR_REQUIRE(fixture.registry.has(kPhysicsRef, "getAngVel", "()Ljava.lang.Vector3;"));
  SLRR_REQUIRE(fixture.registry.has(kPhysicsRef, "getOri", "()Ljava.lang.Ypr;"));
}

SLRR_TEST(physicsref_world_box_and_sphere_create_real_shapes) {
  Fixture fixture;
  slrr::physicsref_create_box(&fixture.self, &fixture.parent, 2, 4, 6, "tree");
  auto& box = fixture.state();
  SLRR_REQUIRE(box.kind == slrr::GameRefKind::PhysicsRef);
  SLRR_REQUIRE(box.physics_shape == slrr::PhysicsShape::Box);
  SLRR_REQUIRE(box.parent == &fixture.parent);
  SLRR_REQUIRE(box.collision_active);
  SLRR_REQUIRE(near(box.physics_size[0], 1));
  SLRR_REQUIRE(near(box.physics_size[1], 2));
  SLRR_REQUIRE(near(box.physics_size[2], 3));

  fixture.invoke("createSphere", "(Ljava.util.resource.ResourceRef;FLjava.lang.String;)V",
                 {slrr::Value::make_obj(&fixture.parent), slrr::Value::make_float(.75f),
                  slrr::Value::make_obj(fixture.jvm.new_string("sphere"))});
  SLRR_REQUIRE(fixture.state().physics_shape == slrr::PhysicsShape::Sphere);
  SLRR_REQUIRE(near(fixture.state().physics_size[0], .75f));

  fixture.invoke("create", "(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V",
                 {slrr::Value::make_obj(&fixture.parent), slrr::Value::make_int(0x12345),
                  slrr::Value::make_obj(fixture.jvm.new_string("resource"))});
  SLRR_REQUIRE(fixture.state().physics_shape == slrr::PhysicsShape::Resource);
  SLRR_REQUIRE(fixture.state().type_id == 0x12345);
}

SLRR_TEST(physicsref_matrix_orientation_and_velocity_getters_use_body_state) {
  Fixture fixture;
  slrr::physicsref_create_box(&fixture.self, &fixture.parent, 2, 2, 2, "box");
  auto* position = vector(fixture.jvm, 1, 2, 3);
  auto* orientation = fixture.jvm.new_object("java.lang.Ypr");
  orientation->fields["y"] = slrr::Value::make_float(.1f);
  orientation->fields["p"] = slrr::Value::make_float(.2f);
  orientation->fields["r"] = slrr::Value::make_float(.3f);
  slrr::physicsref_set_matrix(&fixture.self, position, orientation);

  auto* ori = object(fixture.invoke("getOri", "()Ljava.lang.Ypr;"));
  SLRR_REQUIRE(near(field(ori, "y"), .1f));
  SLRR_REQUIRE(near(field(ori, "p"), .2f));
  SLRR_REQUIRE(near(field(ori, "r"), .3f));

  auto& state = fixture.state();
  state.vel[0] = 10;
  state.vel[1] = 20;
  state.vel[2] = 30;
  state.angular_vel[0] = .5f;
  state.angular_vel[1] = 1;
  state.angular_vel[2] = 2;

  auto* velocity = object(fixture.invoke("getVel", "()Ljava.lang.Vector3;"));
  SLRR_REQUIRE(near(field(velocity, "x"), 10));
  SLRR_REQUIRE(near(field(velocity, "y"), 20));
  SLRR_REQUIRE(near(field(velocity, "z"), 30));

  auto* point = vector(fixture.jvm, 4, 6, 8);
  auto* point_velocity = object(fixture.invoke(
      "getVel", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;",
      {slrr::Value::make_obj(point)}));
  SLRR_REQUIRE(near(field(point_velocity, "x"), 7));
  SLRR_REQUIRE(near(field(point_velocity, "y"), 23.5f));
  SLRR_REQUIRE(near(field(point_velocity, "z"), 29));

  auto* angular = object(fixture.invoke("getAngVel", "()Ljava.lang.Vector3;"));
  SLRR_REQUIRE(near(field(angular, "x"), .5f));
  SLRR_REQUIRE(near(field(angular, "y"), 1));
  SLRR_REQUIRE(near(field(angular, "z"), 2));

  fixture.invoke("setStatic", "(I)V", {slrr::Value::make_int(1)});
  SLRR_REQUIRE(fixture.state().physics_static);
  velocity = object(fixture.invoke("getVel", "()Ljava.lang.Vector3;"));
  SLRR_REQUIRE(near(field(velocity, "x"), 0));
  angular = object(fixture.invoke("getAngVel", "()Ljava.lang.Vector3;"));
  SLRR_REQUIRE(near(field(angular, "z"), 0));
}

SLRR_TEST(collision_activation_is_idempotent_and_commits_at_physics_boundary) {
  slrr::GameRefState state;
  slrr::gameref_queue_collision_state(state);
  SLRR_REQUIRE(state.collision_pending);
  SLRR_REQUIRE(!state.collision_active);
  SLRR_REQUIRE((state.flags & slrr::kCollisionQueued) != 0);

  slrr::gameref_queue_collision_state(state);
  SLRR_REQUIRE(state.collision_pending);
  slrr::gameref_activate_collision_state(state);
  SLRR_REQUIRE(!state.collision_pending);
  SLRR_REQUIRE(state.collision_active);
  SLRR_REQUIRE((state.flags & slrr::kCollisionQueued) == 0);

  slrr::gameref_queue_collision_state(state);
  SLRR_REQUIRE(!state.collision_pending);
  SLRR_REQUIRE((state.flags & slrr::kCollisionQueued) == 0);

  state = slrr::GameRefState{};
  SLRR_REQUIRE(!state.collision_pending);
  SLRR_REQUIRE(!state.collision_active);
  SLRR_REQUIRE((state.flags & slrr::kCollisionQueued) == 0);
}

int main() {
  const slrr::standalone::Test tests[] = {
      physicsref_registers_all_remaining_stock_descriptors,
      physicsref_world_box_and_sphere_create_real_shapes,
      physicsref_matrix_orientation_and_velocity_getters_use_body_state,
      collision_activation_is_idempotent_and_commits_at_physics_boundary,
  };
  return slrr::standalone::run("physicsref", tests);
}
