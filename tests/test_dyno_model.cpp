#include "test_harness.hpp"
#include "support/test_fixtures.hpp"

#include "slrr/dyno_model.hpp"
#include "slrr/jvm.hpp"
#include "slrr/native_registry.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/script_events.hpp"
#include "slrr/script_threads.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// The GC fixture uses native-only methods. These small stubs avoid linking the
// scheduler, renderer, resource database, and TREE evaluator into this focused
// test; production JVM allocation, invocation, marking, and GC are real.
namespace slrr {
void script_threads_shutdown(Jvm&) {}
void script_events_shutdown(Jvm&) {}
bool script_thread_active(Jvm&) { return false; }
void host_loading_present() {}
void host_input_forget(Object*) {}
uint32_t rpk_resolve_resource(const std::string&, uint32_t local) { return local; }
TreeEvalResult tree_eval(TreeHost*, const TufaClass&, const TufaMember&, Object*,
                         const std::vector<Value>&) {
  return {false, {}, "native-only dyno GC fixture"};
}
}  // namespace slrr

namespace {

constexpr const char* kDynoClass = "test.DynoData";

struct RegistryReset {
  RegistryReset() { slrr::dyno_clear_sessions(); }
  ~RegistryReset() { slrr::dyno_clear_sessions(); }
};

bool near(float a, float b, float tolerance = 0.0001f) {
  return std::fabs(a - b) <= tolerance;
}

void append_u32(slrr::test::Bytes& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 24));
}

void append_tag(slrr::test::Bytes& out, const char (&tag)[5]) {
  out.insert(out.end(), tag, tag + 4);
}

void append_chunk(slrr::test::Bytes& out, const char (&tag)[5],
                  const slrr::test::Bytes& payload) {
  append_tag(out, tag);
  append_u32(out, static_cast<uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
}

void append_utf8(slrr::test::Bytes& out, const std::string& text) {
  append_u32(out, 0);
  append_u32(out, static_cast<uint32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
  out.push_back(0);
}

slrr::test::Bytes native_dyno_class() {
  slrr::test::Bytes cons;
  append_u32(cons, 4);
  append_utf8(cons, kDynoClass);       // 0: class name
  append_u32(cons, 4);                // 1: Class
  append_u32(cons, 0);
  append_utf8(cons, "finalize");       // 2
  append_utf8(cons, "()V");            // 3

  slrr::test::Bytes methods;
  append_u32(methods, 0);              // static methods
  append_u32(methods, 1);              // instance methods
  append_u32(methods, 0x40);           // native
  append_u32(methods, 2);
  append_u32(methods, 3);
  append_u32(methods, slrr::kTufaNone);
  append_u32(methods, 0);

  slrr::test::Bytes clss;
  append_u32(clss, slrr::kTufaNone);
  append_u32(clss, 0);                 // not an interface
  append_u32(clss, 1);                 // name Class(0)
  append_u32(clss, slrr::kTufaNone);   // no superclass
  append_u32(clss, 0);                 // interfaces

  slrr::test::Bytes trees;
  append_u32(trees, 0);

  slrr::test::Bytes bytes;
  append_tag(bytes, "TUFA");
  append_u32(bytes, 4);
  append_u32(bytes, 940);
  append_chunk(bytes, "CONS", cons);
  append_chunk(bytes, "MTHD", methods);
  append_chunk(bytes, "CLSS", clss);
  append_chunk(bytes, "TREE", trees);
  return bytes;
}

class DynoVmFixture {
 public:
  DynoVmFixture() : temporary("dyno-gc"), vm(&natives) {
    slrr::dyno_clear_sessions();
    const auto path = temporary.path() / "DynoData.class";
    slrr::test::write_binary_file(path, native_dyno_class());
    SLRR_REQUIRE(vm.load_class_file(path.string()));
    natives.register_method(
        kDynoClass, "finalize", "()V",
        [this](slrr::Object* self, const std::vector<slrr::Value>&) {
          ++finalized;
          slrr::dyno_forget(self);
          return slrr::Value::make_void();
        });
    vm.add_gc_roots([](const slrr::GcRootVisitor& visit) {
      slrr::dyno_visit_gc_roots(visit);
    });
  }

  ~DynoVmFixture() { slrr::dyno_clear_sessions(); }

  slrr::Object* create() { return vm.new_object(kDynoClass); }

  slrr::test::TemporaryDirectory temporary;
  slrr::NativeRegistry natives;
  slrr::Jvm vm;
  int finalized = 0;
};

}  // namespace

SLRR_TEST(dyno_successful_calc_then_gc_and_live_rooting) {
  DynoVmFixture fixture;
  slrr::Object* object = fixture.create();
  slrr::DynoStats stats;
  SLRR_REQUIRE(slrr::dyno_calculate(object, {{"maxRPM", 7000.f}}, 4.f, &stats));
  SLRR_REQUIRE(slrr::dyno_session_count() == 1);

  {
    // Ordinary VM roots still own reachable DynoData instances.
    slrr::Jvm::RootScope roots(fixture.vm, [&](const slrr::GcRootVisitor& visit) {
      visit(object);
    });
    SLRR_REQUIRE(fixture.vm.collect_garbage() == 0);
    SLRR_REQUIRE(fixture.vm.heap_size() == 1);
    SLRR_REQUIRE(slrr::dyno_session_count() == 1);
  }

  SLRR_REQUIRE(fixture.vm.collect_garbage() == 1);
  SLRR_REQUIRE(fixture.finalized == 1);
  SLRR_REQUIRE(fixture.vm.heap_size() == 0);
  SLRR_REQUIRE(slrr::dyno_session_count() == 0);
}

SLRR_TEST(dyno_failed_first_calc_then_gc_has_no_session_leak) {
  DynoVmFixture fixture;
  slrr::Object* object = fixture.create();
  slrr::DynoStats stats;
  SLRR_REQUIRE(!slrr::dyno_calculate(object, {{"stroke", 1.f}}, 4.f, &stats));
  SLRR_REQUIRE(!stats.ok);
  SLRR_REQUIRE(slrr::dyno_session_count() == 0);

  SLRR_REQUIRE(fixture.vm.collect_garbage() == 1);
  SLRR_REQUIRE(fixture.finalized == 1);
  SLRR_REQUIRE(fixture.vm.heap_size() == 0);
  SLRR_REQUIRE(slrr::dyno_session_count() == 0);
}

SLRR_TEST(dyno_repeated_create_calc_finalize_is_bounded) {
  DynoVmFixture fixture;
  for (int i = 0; i < 6; ++i) {
    slrr::Object* object = fixture.create();
    SLRR_REQUIRE(slrr::dyno_calculate(object, {{"maxRPM", 7000.f}}, 4.f, nullptr));
    SLRR_REQUIRE(slrr::dyno_session_count() == 1);
    SLRR_REQUIRE(fixture.vm.collect_garbage() == 1);
    SLRR_REQUIRE(fixture.vm.heap_size() == 0);
    SLRR_REQUIRE(slrr::dyno_session_count() == 0);
  }
  SLRR_REQUIRE(fixture.finalized == 6);
}

SLRR_TEST(dyno_world_cleanup_releases_all_sessions) {
  RegistryReset reset;
  int first = 0;
  int second = 0;
  SLRR_REQUIRE(slrr::dyno_calculate(&first, {{"maxRPM", 7000.f}}, 4.f, nullptr));
  SLRR_REQUIRE(slrr::dyno_calculate(&second, {{"maxRPM", 7000.f}}, 4.f, nullptr));
  SLRR_REQUIRE(slrr::dyno_session_count() == 2);
  slrr::dyno_clear_sessions();
  SLRR_REQUIRE(slrr::dyno_session_count() == 0);
  SLRR_REQUIRE(slrr::dyno_sample(&first, 1000.f, 0.f, false) == 0.f);
  SLRR_REQUIRE(slrr::dyno_sample(&second, 1000.f, 0.f, false) == 0.f);
}

SLRR_TEST(dyno_highest_sample_interpolation_and_out_of_range) {
  RegistryReset reset;
  int key = 0;
  slrr::DynoStats stats;
  SLRR_REQUIRE(slrr::dyno_calculate(&key, {{"maxRPM", 7000.f}}, 16.f, &stats));
  SLRR_REQUIRE(stats.rpm_step == 437.f);
  const float highest = stats.rpm_step * 15.f;
  const float previous = highest - stats.rpm_step;
  const float last_value = slrr::dyno_sample(&key, highest, 0.f, false);
  const float previous_value = slrr::dyno_sample(&key, previous, 0.f, false);
  const float midpoint = slrr::dyno_sample(
      &key, previous + stats.rpm_step * 0.5f, 0.f, false);
  SLRR_REQUIRE(last_value > 0.f);
  SLRR_REQUIRE(near(midpoint, (last_value + previous_value) * 0.5f, 0.002f));
  SLRR_REQUIRE(slrr::dyno_sample(&key, highest + 0.01f, 0.f, false) == 0.f);
  SLRR_REQUIRE(slrr::dyno_sample(&key, -1.f, 0.f, false) == 0.f);
  SLRR_REQUIRE(slrr::dyno_sample(
      &key, std::numeric_limits<float>::max(), 0.f, false) == 0.f);
}

SLRR_TEST(dyno_table_and_max_rpm_changes_regenerate_nitro) {
  RegistryReset reset;
  int key = 0;
  const std::unordered_map<std::string, float> fields{
      {"maxRPM", 7000.f}, {"nitro_H", 1.2f}, {"nitro_cooling", 0.9f}};
  slrr::DynoStats stats;
  SLRR_REQUIRE(slrr::dyno_calculate(&key, fields, 16.f, &stats));
  SLRR_REQUIRE(slrr::dyno_sample(&key, 6555.f, 1.f, false) > 0.f);

  SLRR_REQUIRE(slrr::dyno_calculate(&key, fields, 28.f, &stats));
  SLRR_REQUIRE(stats.rpm_step == 250.f);
  SLRR_REQUIRE(slrr::dyno_sample(&key, 6750.f, 1.f, false) > 0.f);
  SLRR_REQUIRE(slrr::dyno_sample(&key, 6751.f, 1.f, false) == 0.f);

  auto lower_rpm = fields;
  lower_rpm["maxRPM"] = 3500.f;
  SLRR_REQUIRE(slrr::dyno_calculate(&key, lower_rpm, 16.f, &stats));
  SLRR_REQUIRE(stats.rpm_step == 218.f);
  SLRR_REQUIRE(slrr::dyno_sample(&key, 3270.f, 1.f, false) > 0.f);
  SLRR_REQUIRE(slrr::dyno_sample(&key, 6750.f, 1.f, false) == 0.f);
}

SLRR_TEST(dyno_geometry_and_configuration_changes_invalidate_stale_nitro) {
  RegistryReset reset;
  int key = 0;
  std::unordered_map<std::string, float> fields{
      {"maxRPM", 7000.f}, {"nitro_H", 1.2f}, {"nitro_cooling", 0.9f}};
  slrr::DynoStats stats;
  SLRR_REQUIRE(slrr::dyno_calculate(&key, fields, 16.f, &stats));
  const float original_displacement = stats.displacement;
  const float original_nitro = slrr::dyno_sample(&key, 3000.f, 1.f, false);

  fields["bore"] = 0.1f;
  SLRR_REQUIRE(slrr::dyno_calculate(&key, fields, 16.f, &stats));
  SLRR_REQUIRE(stats.displacement > original_displacement);
  const float regenerated_nitro = slrr::dyno_sample(&key, 3000.f, 1.f, false);
  SLRR_REQUIRE(std::fabs(regenerated_nitro - original_nitro) > 0.01f);

  // Removing the nitro configuration must remove the secondary bank instead
  // of blending samples from the previous geometry/configuration.
  fields.erase("nitro_H");
  fields.erase("nitro_cooling");
  SLRR_REQUIRE(slrr::dyno_calculate(&key, fields, 16.f, &stats));
  const float primary = slrr::dyno_sample(&key, 3000.f, 0.f, false);
  SLRR_REQUIRE(slrr::dyno_sample(&key, 3000.f, 1.f, false) == primary);
}
