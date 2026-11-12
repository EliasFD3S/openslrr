#include "test_harness.hpp"

#include "slrr/jvm.hpp"
#include "slrr/native_registry.hpp"
#include "slrr/script_events.hpp"
#include "slrr/script_threads.hpp"
#include "slrr/tree_eval.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

// Focused TREE host: allocation and constructors are real test objects, while
// unrelated scheduler, OSD and resource services stay outside this VM test.
namespace slrr {
void script_threads_shutdown(Jvm&) {}
void script_events_shutdown(Jvm&) {}
bool script_thread_active(Jvm&) { return false; }
bool script_thread_checkpoint(Jvm&) { return false; }
void host_loading_present() {}
void host_input_forget(Object*) {}
void osd_on_field_write(Object*, const std::string&) {}
uint32_t rpk_resolve_resource(const std::string&, uint32_t local) { return local; }
}

namespace {

class TestTreeHost final : public slrr::TreeHost {
 public:
  slrr::Object* tree_new(const std::string& class_fqn) override {
    auto object = std::make_unique<slrr::Object>();
    object->class_name = class_fqn;
    objects.push_back(std::move(object));
    return objects.back().get();
  }

  slrr::Value tree_call(const std::string&, const std::string&, const std::string&,
                        slrr::Object*, const std::vector<slrr::Value>&, bool) override {
    return slrr::Value::make_void();
  }

  slrr::Value tree_call_by_name(const std::string&, const std::string&,
                                const std::vector<slrr::Value>&, bool) override {
    return slrr::Value::make_void();
  }

  slrr::Value tree_get_static(const std::string&, const std::string&) override {
    return slrr::Value::make_void();
  }

  void tree_set_static(const std::string&, const std::string&, slrr::Value) override {}

  slrr::Object* tree_new_string(const std::string& utf8) override {
    auto* object = tree_new("java.lang.String");
    object->str = utf8;
    return object;
  }

  bool tree_instance_of(slrr::Object* object, const std::string& type) override {
    return object && object->class_name == type;
  }

  int tree_field_kind(const std::string&, const std::string&) override { return 2; }

  std::vector<std::unique_ptr<slrr::Object>> objects;
};

slrr::TufaNode node(uint8_t op) { return slrr::TufaNode{op, 0, false, 0}; }
slrr::TufaNode node(uint8_t op, uint32_t imm) {
  return slrr::TufaNode{op, 0, true, imm};
}

struct CloneFixture {
  CloneFixture() {
    const uint32_t name = utf8(class_name);
    cls.name_index = constant({slrr::TufaConstKind::Class, "", name, 0});
    cls.trees.emplace_back();
    cls.trees.emplace_back();
  }

  uint32_t constant(slrr::TufaConstant value) {
    cls.constants.push_back(std::move(value));
    return static_cast<uint32_t>(cls.constants.size() - 1);
  }

  uint32_t utf8(const char* text) {
    return constant({slrr::TufaConstKind::Utf8, text, 0, 0});
  }

  uint32_t field(const char* name, const char* descriptor) {
    const uint32_t n = utf8(name);
    const uint32_t d = utf8(descriptor);
    return constant({slrr::TufaConstKind::NameAndType, "", n, d});
  }

  void append_clone_assignment(uint32_t field_ref) {
    auto& tree = cls.trees[0];
    // this.field, then the explicit local receiver and target field. This is
    // the stock DynoData.clone() postfix shape: local1.field = this.field.
    tree.push_back(node(0x1b, field_ref));
    tree.push_back(node(0x11, 2));
    tree.push_back(node(0x01, 1));
    tree.push_back(node(0x1b, field_ref));
    tree.push_back(node(0x08, 35));
  }

  void begin_clone() {
    auto& tree = cls.trees[0];
    tree.push_back(node(0x02, 1));
    tree.push_back(node(0x06, utf8("Ltest.DynoData;")));
    tree.push_back(node(0x2c));
    tree.push_back(node(0x07, 30));
    tree.push_back(node(0x06, utf8("Ltest.DynoData;")));
    tree.push_back(node(0x27, 0));
    tree.push_back(node(0x21));
    tree.push_back(node(0x2d));
  }

  void finish_clone() {
    auto& tree = cls.trees[0];
    tree.push_back(node(0x01, 1));
    tree.push_back(node(0x2b));

    slrr::TufaMember method;
    method.name = utf8("clone");
    method.descriptor = utf8("()Ltest.DynoData;");
    method.tree = 0;
    method.locals = 2;
    cls.instance_methods.push_back(method);
  }

  void append_self_assignment(uint32_t field_ref) {
    auto& tree = cls.trees[1];
    // No explicit receiver: this.fuelType = this.fuelType. An object value on
    // an otherwise empty stack must not be mistaken for a target receiver.
    tree.push_back(node(0x1b, field_ref));
    tree.push_back(node(0x1b, field_ref));
    tree.push_back(node(0x08, 35));
    tree.push_back(node(0x2a));

    slrr::TufaMember method;
    method.name = utf8("assignSelf");
    method.descriptor = utf8("()V");
    method.tree = 1;
    method.locals = 1;
    cls.instance_methods.push_back(method);
  }

  slrr::TufaClass cls;
  const char* class_name = "test.DynoData";
};

slrr::TreeEvalResult evaluate(CloneFixture& fixture, TestTreeHost& host,
                              slrr::Object* self, std::size_t tree_index) {
  slrr::TufaMember method = fixture.cls.instance_methods.front();
  method.tree = static_cast<uint32_t>(tree_index);
  return slrr::tree_eval(&host, fixture.cls, method, self, {});
}

void require_float_field(const slrr::Object& object, const char* name, float expected) {
  const auto it = object.fields.find(name);
  SLRR_REQUIRE(it != object.fields.end());
  SLRR_REQUIRE(it->second.tag == slrr::ValueTag::Float);
  SLRR_REQUIRE(it->second.v.f == expected);
}

void require_string_field(const slrr::Object& object, const char* name,
                          const std::string& expected) {
  const auto it = object.fields.find(name);
  SLRR_REQUIRE(it != object.fields.end());
  SLRR_REQUIRE(it->second.tag == slrr::ValueTag::Obj);
  SLRR_REQUIRE(it->second.v.o != nullptr);
  SLRR_REQUIRE(it->second.v.o->str == expected);
}

}  // namespace

SLRR_TEST(tree_same_class_explicit_receiver_copies_dyno_fields_without_mutating_source) {
  CloneFixture fixture;
  const uint32_t min = fixture.field("minCompression", "F");
  const uint32_t max = fixture.field("maxCompression", "F");
  const uint32_t mixture = fixture.field("mixture_H", "F");
  const uint32_t fuel = fixture.field("fuelType", "Ljava.lang.String;");
  fixture.begin_clone();
  fixture.append_clone_assignment(min);
  fixture.append_clone_assignment(max);
  fixture.append_clone_assignment(mixture);
  fixture.append_clone_assignment(fuel);
  fixture.finish_clone();

  TestTreeHost host;
  slrr::Object source;
  source.class_name = fixture.class_name;
  source.fields["minCompression"] = slrr::Value::make_float(6.25f);
  source.fields["maxCompression"] = slrr::Value::make_float(15.5f);
  source.fields["mixture_H"] = slrr::Value::make_float(1550000.f);
  source.fields["fuelType"] = slrr::Value::make_obj(host.tree_new_string("95 octane"));

  const slrr::TreeEvalResult result = evaluate(fixture, host, &source, 0);
  SLRR_REQUIRE(result.ok);
  SLRR_REQUIRE(result.value.tag == slrr::ValueTag::Obj);
  slrr::Object* clone = result.value.v.o;
  SLRR_REQUIRE(clone != nullptr);
  SLRR_REQUIRE(clone != &source);

  require_float_field(*clone, "minCompression", 6.25f);
  require_float_field(*clone, "maxCompression", 15.5f);
  require_float_field(*clone, "mixture_H", 1550000.f);
  require_string_field(*clone, "fuelType", "95 octane");

  // The stock failure replaced each source value with the clone itself, so
  // maxCompression later formatted as 0.0:1 and rejected the stock engine.
  require_float_field(source, "minCompression", 6.25f);
  require_float_field(source, "maxCompression", 15.5f);
  require_float_field(source, "mixture_H", 1550000.f);
  require_string_field(source, "fuelType", "95 octane");
}

SLRR_TEST(tree_same_class_implicit_receiver_does_not_steal_object_value) {
  CloneFixture fixture;
  const uint32_t fuel = fixture.field("fuelType", "Ljava.lang.String;");
  fixture.append_self_assignment(fuel);

  TestTreeHost host;
  slrr::Object source;
  source.class_name = fixture.class_name;
  source.fields["fuelType"] = slrr::Value::make_obj(host.tree_new_string("98 octane"));

  const slrr::TreeEvalResult result = evaluate(fixture, host, &source, 1);
  SLRR_REQUIRE(result.ok);
  require_string_field(source, "fuelType", "98 octane");
}
