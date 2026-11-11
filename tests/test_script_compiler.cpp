#include "test_harness.hpp"
#include "support/test_fixtures.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/resource_loading.hpp"
#include "slrr/tufa.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
void write(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary); out << text;
  SLRR_REQUIRE(bool(out));
}
std::string read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
}
SLRR_TEST(compiler_preserves_multiclass_units_and_walks_subdirectories) {
  slrr::test::TemporaryDirectory temp("compile-units");
  write(temp.path()/"scripts/src/Bundle.java",
      "package test; public class Bundle { public int value(){ return 3; } } "
      "public class Helper { public int value(){ return 7; } }");
  write(temp.path()/"scripts/sub/src/Other.java",
      "package test; public class Other { public float value(){ return 1.25; } }");
  write(temp.path()/"outside/src/Untouched.java", "not valid Java");
  unsigned checkpoints = 0;
  const auto report = slrr::script_compile_all(temp.path().string(), "scripts", [&]{++checkpoints;});
  SLRR_REQUIRE(report.attempted == 2);
  SLRR_REQUIRE(report.succeeded == 2 && report.failed == 0);
  SLRR_REQUIRE(report.classes == 3 && checkpoints == 2);
  std::vector<slrr::TufaClass> classes; std::string error;
  SLRR_REQUIRE(slrr::tufa_load_file((temp.path()/"scripts/Bundle.class").string(), &classes, &error));
  SLRR_REQUIRE(classes.size() == 2);
  SLRR_REQUIRE(classes[0].name() == "test.Bundle" && classes[1].name() == "test.Helper");
  SLRR_REQUIRE(!std::filesystem::exists(temp.path()/"scripts/Helper.class"));
  SLRR_REQUIRE(!std::filesystem::exists(temp.path()/"outside/Untouched.class"));
}
SLRR_TEST(compiler_failure_keeps_previous_output_and_backup) {
  slrr::test::TemporaryDirectory temp("compile-failure");
  const auto source = temp.path()/"scripts/src/Part.java";
  const auto output = temp.path()/"scripts/Part.class";
  write(source, "package test; public class Part { public int value(){return 1;} }");
  write(output, "original");
  SLRR_REQUIRE(slrr::script_compile_all(temp.path().string(), ".").succeeded == 1);
  const auto valid = read(output);
  SLRR_REQUIRE(read(output.string()+".openslrr.bak") == "original");
  write(source, "package test; public class Part { broken syntax");
  const auto report = slrr::script_compile_all(temp.path().string(), ".");
  SLRR_REQUIRE(report.attempted == 1 && report.failed == 1);
  SLRR_REQUIRE(read(output) == valid);
  SLRR_REQUIRE(read(output.string()+".openslrr.bak") == "original");
  SLRR_REQUIRE(!std::filesystem::exists(output.string()+".openslrr.tmp"));
  SLRR_REQUIRE(slrr::script_compile_all(temp.path().string(), "..").attempted == 0);
}
SLRR_TEST(compiler_system_multiclass_source_uses_engine_namespace) {
  slrr::test::TemporaryDirectory temp("compile-system");
  write(temp.path()/"system/Scripts/util/resource/src/Bundle.java",
      "public class Bundle { public int value(){return 2;} } public class Helper {} ");
  SLRR_REQUIRE(slrr::script_compile_all(temp.path().string(), ".").succeeded == 1);
  std::vector<slrr::TufaClass> classes; std::string error;
  SLRR_REQUIRE(slrr::tufa_load_file((temp.path()/"system/Scripts/util/resource/Bundle.class").string(),
                                   &classes, &error));
  SLRR_REQUIRE(classes.size() == 2);
  SLRR_REQUIRE(classes[0].name() == "java.util.resource.Bundle");
  SLRR_REQUIRE(classes[1].name() == "java.util.resource.Helper");
}
SLRR_TEST(loading_history_reset_preserves_work_and_drains_without_fake_delay) {
  slrr::ResourceLoadingHistory history;
  history.reset();
  SLRR_REQUIRE(!history.busy());
  history.record(12);
  history.reset();
  SLRR_REQUIRE(history.busy());
  // Reads do not consume resource history.
  for (int i=0; i<100; ++i) SLRR_REQUIRE(history.busy());
  history.pump();
  for (int i=0; i<32; ++i) history.pump();
  SLRR_REQUIRE(!history.busy());
  history.reset();
  history.record(std::numeric_limits<double>::quiet_NaN());
  SLRR_REQUIRE(!history.busy());
}
