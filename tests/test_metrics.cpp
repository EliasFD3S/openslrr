#include "test_harness.hpp"
#include "support/test_fixtures.hpp"

#include "slrr/diagnostics.hpp"
#include "slrr/dyno_model.hpp"
#include "slrr/jvm.hpp"
#include "slrr/metrics.hpp"
#include "slrr/native_registry.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/script_events.hpp"
#include "slrr/script_threads.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

// Keep the focused VM fixture native-only, matching the existing dyno lane.
namespace slrr {
void script_threads_shutdown(Jvm&) {}
void script_events_shutdown(Jvm&) {}
bool script_thread_active(Jvm&) { return false; }
void host_loading_present() {}
void host_input_forget(Object*) {}
uint32_t rpk_resolve_resource(const std::string&, uint32_t local) { return local; }
TreeEvalResult tree_eval(TreeHost*, const TufaClass&, const TufaMember&, Object*,
                         const std::vector<Value>&) {
  return {false, {}, "native-only metrics fixture"};
}
}  // namespace slrr

namespace {

const slrr::metrics::Value* find(const slrr::metrics::Snapshot& snapshot,
                                 const std::string& name) {
  for (const auto& value : snapshot.values)
    if (value.name == name) return &value;
  return nullptr;
}

std::int64_t value_of(const slrr::metrics::Snapshot& snapshot,
                      const std::string& name) {
  const auto* value = find(snapshot, name);
  SLRR_REQUIRE(value != nullptr);
  return value->value;
}

struct Reset {
  Reset() { slrr::metrics::reset(); }
  ~Reset() {
    slrr::diagnostics::shutdown();
    slrr::metrics::reset();
  }
};

}  // namespace

SLRR_TEST(metrics_registration_update_snapshot_and_signed_json) {
  Reset reset;
  using namespace slrr::metrics;
  SLRR_REQUIRE(register_metric("test.gauge", Kind::Gauge));
  SLRR_REQUIRE(register_metric("test.counter", Kind::Counter));
  SLRR_REQUIRE(register_metric("test.gauge", Kind::Gauge));
  SLRR_REQUIRE(!register_metric("test.gauge", Kind::Counter));
  SLRR_REQUIRE(!register_metric("", Kind::Gauge));
  SLRR_REQUIRE(!register_metric("test invalid", Kind::Gauge));
  SLRR_REQUIRE(!register_metric(std::string(97, 'x'), Kind::Gauge));
  SLRR_REQUIRE(!set("test.unregistered", 1));
  SLRR_REQUIRE(!add("test.gauge", 1));
  SLRR_REQUIRE(set("test.gauge", -7));
  SLRR_REQUIRE(add("test.counter", 5));
  SLRR_REQUIRE(add("test.counter", -2));

  const auto current = snapshot();
  SLRR_REQUIRE(value_of(current, "test.gauge") == -7);
  SLRR_REQUIRE(value_of(current, "test.counter") == 3);
  SLRR_REQUIRE(find(current, "test.gauge")->kind == Kind::Gauge);
  SLRR_REQUIRE(find(current, "test.counter")->kind == Kind::Counter);
  SLRR_REQUIRE(!current.frame && !current.tick && current.section.empty());

  const std::string json = to_json(current);
  SLRR_REQUIRE(json.find("\"frame\":null,\"tick\":null") != std::string::npos);
  SLRR_REQUIRE(json.find("\"test.gauge\":-7") != std::string::npos);
  SLRR_REQUIRE(json.find("\"test.counter\":3") != std::string::npos);
  SLRR_REQUIRE(json.find("\"test.gauge\":\"gauge\"") != std::string::npos);
  SLRR_REQUIRE(json.find("\"test.counter\":\"counter\"") != std::string::npos);
  SLRR_REQUIRE(json.find("0x") == std::string::npos);

  SLRR_REQUIRE(set("test.counter", std::numeric_limits<std::int64_t>::max()));
  SLRR_REQUIRE(add("test.counter", 1));
  SLRR_REQUIRE(value_of(snapshot(), "test.counter") ==
               std::numeric_limits<std::int64_t>::max());
  SLRR_REQUIRE(set("test.counter", std::numeric_limits<std::int64_t>::min()));
  SLRR_REQUIRE(add("test.counter", -1));
  SLRR_REQUIRE(value_of(snapshot(), "test.counter") ==
               std::numeric_limits<std::int64_t>::min());
}

SLRR_TEST(metrics_provider_lifecycle_and_bounds) {
  Reset reset;
  using namespace slrr::metrics;
  int live = 4;
  const auto id = register_provider([](void* context, std::vector<Value>& values) {
    values.push_back({"test.provider", *static_cast<int*>(context), Kind::Gauge});
    values.push_back({"bad provider name", 99, Kind::Gauge});
  }, &live);
  SLRR_REQUIRE(id != 0);
  SLRR_REQUIRE(value_of(snapshot(), "test.provider") == 4);
  live = 9;
  SLRR_REQUIRE(value_of(snapshot(), "test.provider") == 9);
  SLRR_REQUIRE(find(snapshot(), "bad provider name") == nullptr);
  unregister_provider(id);
  SLRR_REQUIRE(find(snapshot(), "test.provider") == nullptr);

  for (int i = 0; i < 128; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "test.bound.%03d", i);
    SLRR_REQUIRE(register_metric(name, Kind::Gauge));
  }
  SLRR_REQUIRE(!register_metric("test.bound.overflow", Kind::Gauge));
  SLRR_REQUIRE(snapshot().values.size() == 128);
}

SLRR_TEST(metrics_context_json_and_repeated_section_cycles) {
  Reset reset;
  using namespace slrr::metrics;
  slrr::test::TemporaryDirectory temporary("metrics-context");
  std::string error;
  SLRR_REQUIRE(slrr::diagnostics::start(
      (temporary.path() / "context.jsonl").string(), error));
  SLRR_REQUIRE(register_metric("test.cycle_objects", Kind::Gauge));
  slrr::diagnostics::set_frame(42);
  slrr::diagnostics::set_tick(7);

  set("test.cycle_objects", 10);
  slrr::diagnostics::set_section("java.game.Garage");
  set("test.cycle_objects", 14);
  slrr::diagnostics::set_section("java.game.Track");
  set("test.cycle_objects", 10);
  slrr::diagnostics::set_section("java.game.Garage");
  slrr::diagnostics::set_section("java.game.Garage");

  const auto sections = section_snapshots();
  SLRR_REQUIRE(sections.size() == 3);
  SLRR_REQUIRE(sections[0].section == "java.game.Garage");
  SLRR_REQUIRE(sections[1].section == "java.game.Track");
  SLRR_REQUIRE(sections[2].section == "java.game.Garage");
  SLRR_REQUIRE(value_of(sections[0], "test.cycle_objects") == 10);
  SLRR_REQUIRE(value_of(sections[1], "test.cycle_objects") == 14);
  SLRR_REQUIRE(value_of(sections[2], "test.cycle_objects") == 10);
  SLRR_REQUIRE(sections[0].frame == 42 && sections[0].tick == 7);

  const std::string json = snapshot_json();
  SLRR_REQUIRE(json.find("{\"version\":1") == 0);
  SLRR_REQUIRE(json.find("\"section_snapshots\":[") != std::string::npos);
  SLRR_REQUIRE(json.find("\"frame\":42,\"tick\":7") != std::string::npos);
  SLRR_REQUIRE(json.find("java.game.Garage") != std::string::npos);
  SLRR_REQUIRE(json.find("java.game.Track") != std::string::npos);
}

SLRR_TEST(metrics_section_history_does_not_require_tracing) {
  Reset reset;
  SLRR_REQUIRE(!slrr::diagnostics::enabled());
  SLRR_REQUIRE(slrr::metrics::register_metric("test.objects", slrr::metrics::Kind::Gauge));
  slrr::metrics::set("test.objects", 10);
  slrr::diagnostics::set_section("java.game.Garage");
  slrr::metrics::set("test.objects", 14);
  slrr::diagnostics::set_section("java.game.Track");
  slrr::metrics::set("test.objects", 10);
  slrr::diagnostics::set_section("java.game.Garage");

  const auto sections = slrr::metrics::section_snapshots();
  SLRR_REQUIRE(sections.size() == 3);
  SLRR_REQUIRE(value_of(sections[0], "test.objects") == 10);
  SLRR_REQUIRE(value_of(sections[1], "test.objects") == 14);
  SLRR_REQUIRE(value_of(sections[2], "test.objects") == 10);
}

SLRR_TEST(metrics_final_file_output_and_open_failure) {
  Reset reset;
  using namespace slrr::metrics;
  slrr::test::TemporaryDirectory temporary("metrics-file");
  SLRR_REQUIRE(register_metric("test.file_value", Kind::Gauge));
  SLRR_REQUIRE(set("test.file_value", -1234567890123LL));
  const auto path = temporary.path() / "final.json";
  std::string error;
  SLRR_REQUIRE(write_snapshot(path.string(), error));
  SLRR_REQUIRE(error.empty());
  const std::string contents = slrr::test::read_text_file(path);
  SLRR_REQUIRE(!contents.empty() && contents.back() == '\n');
  SLRR_REQUIRE(contents.find("\"test.file_value\":-1234567890123") !=
               std::string::npos);
  SLRR_REQUIRE(contents.find("\"snapshot\":{") != std::string::npos);
  SLRR_REQUIRE(contents.find("\"section_snapshots\":[]") != std::string::npos);

  SLRR_REQUIRE(!write_snapshot((temporary.path() / "missing" / "final.json").string(),
                               error));
  SLRR_REQUIRE(!error.empty());
}

SLRR_TEST(metrics_real_dyno_and_vm_heap_counters) {
  Reset reset;
  slrr::NativeRegistry natives;
  slrr::Jvm vm(&natives);
  SLRR_REQUIRE(value_of(slrr::metrics::snapshot(), "vm.heap_objects") == 0);
  SLRR_REQUIRE(value_of(slrr::metrics::snapshot(), "dyno.sessions") == 0);
  SLRR_REQUIRE(value_of(slrr::metrics::snapshot(), "dyno.calculations_in_flight") == 0);

  auto* object = vm.new_object("java.lang.Object");
  slrr::DynoStats stats;
  SLRR_REQUIRE(slrr::dyno_calculate(object, {{"maxRPM", 7000.f}}, 4.f, &stats));
  auto current = slrr::metrics::snapshot();
  SLRR_REQUIRE(value_of(current, "vm.heap_objects") == 1);
  SLRR_REQUIRE(value_of(current, "dyno.sessions") == 1);
  SLRR_REQUIRE(value_of(current, "dyno.calculations_in_flight") == 0);

  slrr::dyno_forget(object);
  SLRR_REQUIRE(vm.collect_garbage() == 1);
  current = slrr::metrics::snapshot();
  SLRR_REQUIRE(value_of(current, "vm.heap_objects") == 0);
  SLRR_REQUIRE(value_of(current, "dyno.sessions") == 0);
  SLRR_REQUIRE(value_of(current, "vm.gc_last_freed") == 1);
}
