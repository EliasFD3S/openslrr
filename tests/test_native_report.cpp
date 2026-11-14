#include "test_harness.hpp"
#include "support/test_fixtures.hpp"

#include "slrr/diagnostics.hpp"
#include "slrr/metrics.hpp"
#include "slrr/native_registry.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Reset {
  Reset() {
    slrr::diagnostics::shutdown();
    slrr::diagnostics::clear_context();
    slrr::metrics::reset();
  }
  ~Reset() {
    slrr::diagnostics::shutdown();
    slrr::diagnostics::clear_context();
    slrr::metrics::reset();
  }
};

const slrr::NativeRuntimeStats& find_record(const slrr::NativeReport& report,
                                            const std::string& method) {
  for (const auto& record : report.signatures)
    if (record.method == method) return record;
  throw std::runtime_error("missing test record");
}

std::int64_t metric(const slrr::metrics::Snapshot& snapshot,
                    const std::string& name) {
  for (const auto& value : snapshot.values)
    if (value.name == name) return value.value;
  throw std::runtime_error("missing test metric");
}

void expect_missing(slrr::NativeRegistry& registry, const std::string& method,
                    const std::string& signature = "()V") {
  bool seen = false;
  try {
    registry.invoke("example.Class", method, signature, nullptr, {});
  } catch (const std::runtime_error& error) {
    seen = std::string(error.what()) ==
           "Missing native: example.Class#" + method + "#" + signature;
  }
  SLRR_REQUIRE(seen);
}

}  // namespace

SLRR_TEST(native_report_metadata_noop_success_alias_duration_and_section) {
  Reset reset;
  slrr::NativeRegistry registry;
  registry.register_method(
      "example.Class", "answer", "()I",
      [](slrr::Object*, const std::vector<slrr::Value>& args) {
        SLRR_REQUIRE(args.size() == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return slrr::Value::make_int(42);
      });
  registry.register_method(
      "example.Class", "noop", "()V",
      [](slrr::Object*, const std::vector<slrr::Value>&) {
        return slrr::Value::make_void();
      },
      slrr::NativeImplementation::ExplicitNoop);

  SLRR_REQUIRE(!slrr::diagnostics::enabled());
  slrr::diagnostics::set_section("java.game.Garage");
  const auto answer = registry.invoke("example.Class", "answer", "()I", nullptr,
                                      {slrr::Value::make_int(123456)});
  SLRR_REQUIRE(answer.tag == slrr::ValueTag::Int && answer.v.i == 42);
  registry.invoke("example.Class", "noop", "()", nullptr, {});
  slrr::diagnostics::set_section(std::string(300, 's'));
  registry.invoke("example.Class", "answer", "()I", nullptr,
                  {slrr::Value::make_int(7)});

  const auto report = registry.report();
  SLRR_REQUIRE(report.registered == 2);
  SLRR_REQUIRE(report.called == 2);
  SLRR_REQUIRE(report.success == 3);
  SLRR_REQUIRE(report.error == 0);
  SLRR_REQUIRE(report.missing == 0);
  SLRR_REQUIRE(report.noop == 1);
  SLRR_REQUIRE(report.signatures.size() == 2);
  SLRR_REQUIRE(report.signatures[0].method == "answer");
  SLRR_REQUIRE(report.signatures[1].method == "noop");
  const auto& success = find_record(report, "answer");
  SLRR_REQUIRE(success.registered);
  SLRR_REQUIRE(success.implementation == slrr::NativeImplementation::Body);
  SLRR_REQUIRE(success.invocation_count == 2);
  SLRR_REQUIRE(success.success_count == 2);
  SLRR_REQUIRE(success.total_duration_ns >= 1000000);
  SLRR_REQUIRE(success.last_section && success.last_section->size() == 256);
  const auto& noop = find_record(report, "noop");
  SLRR_REQUIRE(noop.implementation == slrr::NativeImplementation::ExplicitNoop);
  SLRR_REQUIRE(noop.signature == "()V");
  SLRR_REQUIRE(noop.invocation_count == 1);
  SLRR_REQUIRE(noop.success_count == 1);
  SLRR_REQUIRE(noop.last_section && *noop.last_section == "java.game.Garage");

  const std::string json = registry.report_json();
  SLRR_REQUIRE(json.find("\"registered\":2,\"called\":2,\"success\":3") !=
               std::string::npos);
  SLRR_REQUIRE(json.find("\"implementation\":\"body\"") != std::string::npos);
  SLRR_REQUIRE(json.find("\"implementation\":\"explicit_noop\"") !=
               std::string::npos);
  SLRR_REQUIRE(json.find("behaviorally_verified") == std::string::npos);
  SLRR_REQUIRE(json.find("123456") == std::string::npos);
}

SLRR_TEST(native_report_error_missing_and_nonstandard_exceptions_are_distinct) {
  Reset reset;
  slrr::NativeRegistry registry;
  registry.register_method("example.Class", "fail", "()V",
                           [](slrr::Object*, const std::vector<slrr::Value>&)
                               -> slrr::Value { throw std::runtime_error("native failure"); });
  registry.register_method("example.Class", "nonstandard", "()V",
                           [](slrr::Object*, const std::vector<slrr::Value>&)
                               -> slrr::Value { throw 7; });
  for (const char* method : {"fail", "nonstandard"}) {
    bool seen = false;
    try {
      registry.invoke("example.Class", method, "()V", nullptr, {});
    } catch (...) {
      seen = true;
    }
    SLRR_REQUIRE(seen);
  }
  expect_missing(registry, "absent");
  expect_missing(registry, "absent");

  const auto report = registry.report();
  SLRR_REQUIRE(report.registered == 2);
  SLRR_REQUIRE(report.called == 3);
  SLRR_REQUIRE(report.success == 0);
  SLRR_REQUIRE(report.error == 2);
  SLRR_REQUIRE(report.missing == 2);
  SLRR_REQUIRE(report.noop == 0);
  SLRR_REQUIRE(find_record(report, "fail").error_count == 1);
  SLRR_REQUIRE(find_record(report, "nonstandard").error_count == 1);
  const auto& missing = find_record(report, "absent");
  SLRR_REQUIRE(!missing.registered);
  SLRR_REQUIRE(missing.invocation_count == 2);
  SLRR_REQUIRE(missing.missing_count == 2);
  SLRR_REQUIRE(missing.error_count == 0);
  SLRR_REQUIRE(report.signatures.size() == 3);
}

SLRR_TEST(native_report_metrics_follow_registry_lifecycle) {
  Reset reset;
  {
    slrr::NativeRegistry registry;
    registry.register_method("example.Class", "noop", "()V",
                             [](slrr::Object*, const std::vector<slrr::Value>&) {
                               return slrr::Value::make_void();
                             },
                             slrr::NativeImplementation::ExplicitNoop);
    auto snapshot = slrr::metrics::snapshot();
    SLRR_REQUIRE(metric(snapshot, "natives.registered") == 1);
    SLRR_REQUIRE(metric(snapshot, "natives.called") == 0);
    SLRR_REQUIRE(metric(snapshot, "natives.missing") == 0);
    SLRR_REQUIRE(metric(snapshot, "natives.noop") == 1);
    registry.invoke("example.Class", "noop", "()V", nullptr, {});
    expect_missing(registry, "absent");
    snapshot = slrr::metrics::snapshot();
    SLRR_REQUIRE(metric(snapshot, "natives.called") == 2);
    SLRR_REQUIRE(metric(snapshot, "natives.missing") == 1);
  }
  const auto snapshot = slrr::metrics::snapshot();
  for (const auto& value : snapshot.values)
    SLRR_REQUIRE(value.name.find("natives.") != 0);
}

SLRR_TEST(native_report_json_file_escaping_and_open_failure) {
  Reset reset;
  slrr::test::TemporaryDirectory temporary("native-report-json");
  slrr::NativeRegistry registry;
  const std::string owner = std::string("example.\\\"/line\n") + char(1) +
                            " snowman=\xe2\x98\x83 invalid:\xff";
  registry.register_method(owner, "quote\"\\", "()V",
                           [](slrr::Object*, const std::vector<slrr::Value>&) {
                             return slrr::Value::make_void();
                           });
  const auto path = temporary.path() / "native.json";
  std::string error;
  SLRR_REQUIRE(registry.write_report(path.string(), error));
  SLRR_REQUIRE(error.empty());
  const std::string json = slrr::test::read_text_file(path);
  SLRR_REQUIRE(!json.empty() && json.back() == '\n');
  SLRR_REQUIRE(json.find("{\"version\":1,\"summary\":{") == 0);
  SLRR_REQUIRE(json.find("quote\\\"\\\\") != std::string::npos);
  SLRR_REQUIRE(json.find("/line\\n") != std::string::npos);
  SLRR_REQUIRE(json.find("\\u0001") != std::string::npos);
  SLRR_REQUIRE(json.find("\xe2\x98\x83") != std::string::npos);
  SLRR_REQUIRE(json.find("invalid:\\ufffd") != std::string::npos);
  SLRR_REQUIRE(!registry.write_report(
      (temporary.path() / "absent" / "native.json").string(), error));
  SLRR_REQUIRE(!error.empty());
}

SLRR_TEST(native_report_missing_records_and_components_are_bounded) {
  Reset reset;
  slrr::NativeRegistry registry;
  for (std::size_t i = 0; i < slrr::NativeRegistry::kMaximumMissingSignatures + 1;
       ++i) {
    expect_missing(registry, "missing" + std::to_string(i));
  }
  expect_missing(registry, "missing" +
                 std::to_string(slrr::NativeRegistry::kMaximumMissingSignatures));
  expect_missing(registry, std::string(
      slrr::NativeRegistry::kMaximumMissingComponentBytes + 1, 'x'));

  const auto report = registry.report();
  SLRR_REQUIRE(report.registered == 0);
  SLRR_REQUIRE(report.called == slrr::NativeRegistry::kMaximumMissingSignatures);
  SLRR_REQUIRE(report.missing == slrr::NativeRegistry::kMaximumMissingSignatures + 3);
  SLRR_REQUIRE(report.untracked_missing_invocations == 3);
  SLRR_REQUIRE(report.signatures.size() ==
               slrr::NativeRegistry::kMaximumMissingSignatures);
  for (const auto& record : report.signatures) {
    SLRR_REQUIRE(!record.registered);
    SLRR_REQUIRE(record.class_name.size() <=
                 slrr::NativeRegistry::kMaximumMissingComponentBytes);
    SLRR_REQUIRE(record.method.size() <=
                 slrr::NativeRegistry::kMaximumMissingComponentBytes);
    SLRR_REQUIRE(record.signature.size() <=
                 slrr::NativeRegistry::kMaximumMissingComponentBytes);
  }
  const std::string json = registry.report_json();
  SLRR_REQUIRE(json.find("\"untracked_missing_invocations\":3") !=
               std::string::npos);
}
