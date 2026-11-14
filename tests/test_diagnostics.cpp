#include "test_harness.hpp"

#include "slrr/diagnostics.hpp"
#include "slrr/jvm.hpp"
#include "slrr/native_registry.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class TraceFixture {
 public:
  explicit TraceFixture(std::size_t capacity = 16) {
    static std::atomic_uint64_t sequence{0};
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = std::filesystem::temp_directory_path() /
                 ("slrr-diagnostics-test-" + std::to_string(unique) + "-" +
                  std::to_string(++sequence));
    std::filesystem::create_directories(directory_);
    path_ = directory_ / "trace.jsonl";
    std::string error;
    SLRR_REQUIRE(slrr::diagnostics::start(path_.string(), error, capacity));
    SLRR_REQUIRE(error.empty());
    slrr::diagnostics::clear_context();
  }

  ~TraceFixture() {
    slrr::diagnostics::shutdown();
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path directory_;
  std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

slrr::diagnostics::Event runtime_event(const std::string& name) {
  slrr::diagnostics::Event event;
  event.name = name;
  event.result = "ok";
  return event;
}

}  // namespace

SLRR_TEST(diagnostics_disabled_operation_is_empty) {
  slrr::diagnostics::shutdown();
  SLRR_REQUIRE(!slrr::diagnostics::enabled());
  slrr::diagnostics::set_frame(10);
  slrr::diagnostics::set_tick(20);
  slrr::diagnostics::set_section("Disabled");
  slrr::diagnostics::set_fiber(30);
  slrr::diagnostics::trace(runtime_event("discarded"));
  slrr::diagnostics::flush();
  SLRR_REQUIRE(slrr::diagnostics::snapshot().empty());
  SLRR_REQUIRE(slrr::diagnostics::dropped_events() == 0);
  SLRR_REQUIRE(!slrr::diagnostics::current_fiber_id());
}

SLRR_TEST(diagnostics_ring_capacity_and_event_payload_are_bounded) {
  TraceFixture fixture(3);
  for (int i = 0; i < 5; ++i) {
    auto event = runtime_event("event-" + std::to_string(i));
    if (i == 4) {
      event.error.assign(4096, 'x');
      for (int value = 0; value < 24; ++value)
        event.values.push_back({"value-" + std::to_string(value), value});
    }
    slrr::diagnostics::trace(std::move(event));
  }

  const auto events = slrr::diagnostics::snapshot();
  SLRR_REQUIRE(events.size() == 3);
  SLRR_REQUIRE(events[0].name == "event-2");
  SLRR_REQUIRE(events[1].name == "event-3");
  SLRR_REQUIRE(events[2].name == "event-4");
  SLRR_REQUIRE(events[2].error.size() == 512);
  SLRR_REQUIRE(events[2].values.size() == 16);
  SLRR_REQUIRE(events[2].timestamp_us != 0);
  SLRR_REQUIRE(slrr::diagnostics::dropped_events() == 2);
}

SLRR_TEST(diagnostics_context_is_propagated_and_thread_local) {
  TraceFixture fixture(4);
  slrr::diagnostics::Context context;
  context.frame = 42;
  context.tick = 7;
  context.section = "java.game.Garage";
  context.fiber_id = 11;
  slrr::diagnostics::set_context(context);
  slrr::diagnostics::trace(runtime_event("host"));

  std::thread worker([] {
    slrr::diagnostics::set_frame(99);
    slrr::diagnostics::set_tick(100);
    slrr::diagnostics::set_section("media.callback");
    slrr::diagnostics::set_fiber(12);
    slrr::diagnostics::trace(runtime_event("worker"));
  });
  worker.join();

  const auto current = slrr::diagnostics::current_context();
  SLRR_REQUIRE(current.frame == 42);
  SLRR_REQUIRE(current.tick == 7);
  SLRR_REQUIRE(current.section == "java.game.Garage");
  SLRR_REQUIRE(current.fiber_id == 11);

  const auto events = slrr::diagnostics::snapshot();
  SLRR_REQUIRE(events.size() == 2);
  SLRR_REQUIRE(events[0].frame == 42);
  SLRR_REQUIRE(events[0].tick == 7);
  SLRR_REQUIRE(events[0].section == "java.game.Garage");
  SLRR_REQUIRE(events[0].fiber_id == 11);
  SLRR_REQUIRE(events[1].frame == 99);
  SLRR_REQUIRE(events[1].tick == 100);
  SLRR_REQUIRE(events[1].section == "media.callback");
  SLRR_REQUIRE(events[1].fiber_id == 12);
}

SLRR_TEST(diagnostics_native_success_error_and_missing_preserve_behavior) {
  TraceFixture fixture(8);
  slrr::NativeRegistry registry;
  slrr::Object object;
  registry.register_method("example.Class", "answer", "()I",
                           [](slrr::Object*, const std::vector<slrr::Value>&) {
                             return slrr::Value::make_int(42);
                           });
  registry.register_method("example.Class", "fail", "()V",
                           [](slrr::Object*, const std::vector<slrr::Value>&) -> slrr::Value {
                             throw std::runtime_error("native failure");
                           });

  const auto answer = registry.invoke("example.Class", "answer", "()I", &object, {});
  SLRR_REQUIRE(answer.tag == slrr::ValueTag::Int);
  SLRR_REQUIRE(answer.v.i == 42);

  bool failure_seen = false;
  try {
    registry.invoke("example.Class", "fail", "()V", &object, {});
  } catch (const std::runtime_error& error) {
    failure_seen = std::string(error.what()) == "native failure";
  }
  SLRR_REQUIRE(failure_seen);

  bool missing_seen = false;
  try {
    registry.invoke("example.Class", "missing", "()V", &object, {});
  } catch (const std::runtime_error& error) {
    missing_seen = std::string(error.what()) ==
                   "Missing native: example.Class#missing#()V";
  }
  SLRR_REQUIRE(missing_seen);

  const auto events = slrr::diagnostics::snapshot();
  SLRR_REQUIRE(events.size() == 3);
  SLRR_REQUIRE(events[0].category == slrr::diagnostics::Category::Native);
  SLRR_REQUIRE(events[0].name == "native.invoke");
  SLRR_REQUIRE(events[0].class_name == "example.Class");
  SLRR_REQUIRE(events[0].method == "answer");
  SLRR_REQUIRE(events[0].signature == "()I");
  SLRR_REQUIRE(events[0].object == &object);
  SLRR_REQUIRE(events[0].result == "ok");
  SLRR_REQUIRE(events[0].error.empty());
  SLRR_REQUIRE(events[1].result == "error");
  SLRR_REQUIRE(events[1].error == "native failure");
  SLRR_REQUIRE(events[2].result == "error");
  SLRR_REQUIRE(events[2].error == "Missing native: example.Class#missing#()V");
}

SLRR_TEST(diagnostics_json_escaping_and_real_file_flush_readback) {
  TraceFixture fixture(2);
  auto event = runtime_event("escape\"/line\nnext");
  event.resource = std::string("control:") + char(1);
  event.detail = "quote=\" slash=\\ tab=\t snowman=\xe2\x98\x83";
  event.error = std::string("invalid:") + '\xff';
  event.values = {{"count", 3}};
  slrr::diagnostics::trace(std::move(event));
  slrr::diagnostics::flush();

  const std::string flushed = read_file(fixture.path());
  SLRR_REQUIRE(!flushed.empty());
  SLRR_REQUIRE(flushed.back() == '\n');
  SLRR_REQUIRE(flushed.find('\n') == flushed.size() - 1);
  SLRR_REQUIRE(flushed.find("escape\\\"/line\\nnext") != std::string::npos);
  SLRR_REQUIRE(flushed.find("control:\\u0001") != std::string::npos);
  SLRR_REQUIRE(flushed.find("quote=\\\" slash=\\\\ tab=\\t") != std::string::npos);
  SLRR_REQUIRE(flushed.find("\xe2\x98\x83") != std::string::npos);
  SLRR_REQUIRE(flushed.find("invalid:\\ufffd") != std::string::npos);
  SLRR_REQUIRE(flushed.find("\"values\":{\"count\":3}") != std::string::npos);

  slrr::diagnostics::shutdown();
  SLRR_REQUIRE(read_file(fixture.path()) == flushed);
}

SLRR_TEST(diagnostics_open_failure_does_not_enable_tracing) {
  slrr::diagnostics::shutdown();
  const auto base = std::filesystem::temp_directory_path() /
                    "slrr-diagnostics-missing-directory";
  std::error_code ignored;
  std::filesystem::remove_all(base, ignored);
  std::string error;
  SLRR_REQUIRE(!slrr::diagnostics::start((base / "child" / "trace.jsonl").string(), error));
  SLRR_REQUIRE(!error.empty());
  SLRR_REQUIRE(!slrr::diagnostics::enabled());
}
