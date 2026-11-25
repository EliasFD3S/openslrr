#include "standalone_test.hpp"
#include "slrr/script_compiler.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// #420: compile_game_sources picks, per unit, the float conversion that
// reproduces the class it replaces; System.compileAll stays on B.
namespace fs = std::filesystem;
using namespace slrr;

namespace {
// 1.90 and 2.65 as the stock MC swaybars hold them (D) and as the exe's
// conversion makes them (B); 0.527 tells A (0x3F06E978) from B, C and D.
constexpr uint32_t D_190 = 0x3FF33333, B_190 = 0x3FF33334;
constexpr uint32_t D_265 = 0x4029999A, B_265 = 0x40299999;
constexpr uint32_t A_0527 = 0x3F06E978, B_0527 = 0x3F06E979;

std::string le(uint32_t bits) {
  return {char(bits), char(bits >> 8), char(bits >> 16), char(bits >> 24)};
}
bool holds(const std::string& bytes, uint32_t bits) { return bytes.find(le(bits)) != std::string::npos; }
std::string read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Root {
  fs::path root;
  Root() {
    static int serial = 0;
    root = fs::temp_directory_path() / ("slrr_float_mode_" + std::to_string(++serial));
    fs::remove_all(root);
    fs::create_directories(root / "probe/src");
    std::ofstream(root / "probe/src/P.java") <<
        "package probe;\n"
        "public class P {\n"
        " float a, b, c;\n"
        " public P() { a = 1.90; b = 2.65; c = 0.527; }\n"
        "}\n";
  }
  ~Root() { std::error_code ec; fs::remove_all(root, ec); }
  fs::path cls() const { return root / "probe/P.class"; }
  // Any bytes do as a reference: the choice only looks for the literal bits.
  void reference(const std::string& bytes) const {
    std::ofstream(cls(), std::ios::binary) << "REF" << bytes;
    fs::remove(fs::path(cls()) += ".openslrr.bak");
  }
  std::string compile(FloatLiteralMode mode) const {
    ScriptCompileOptions options;
    options.float_mode = mode;
    const auto report = script_compile_all(root.string(), "probe", options);
    SLRR_REQUIRE(report.failed == 0 && report.succeeded == 1);
    return read(cls());
  }
};

void auto_without_reference_is_b() {
  Root r;
  const auto out = r.compile(FloatLiteralMode::Auto);
  SLRR_REQUIRE(holds(out, B_190) && holds(out, B_265) && !holds(out, D_190));
}

void auto_follows_a_stock_data_class() {
  Root r;
  r.reference(le(D_190) + le(D_265) + le(B_0527));
  const auto out = r.compile(FloatLiteralMode::Auto);
  SLRR_REQUIRE(holds(out, D_190) && holds(out, D_265) && !holds(out, B_190) && !holds(out, B_265));
  // The backup of the first original stays the reference of later runs.
  const auto again = r.compile(FloatLiteralMode::Auto);
  SLRR_REQUIRE(again == out);
}

void auto_follows_a_core_script() {
  Root r;
  r.reference(le(B_190) + le(A_0527));
  const auto out = r.compile(FloatLiteralMode::Auto);
  SLRR_REQUIRE(holds(out, A_0527) && holds(out, B_190) && !holds(out, B_0527));
}

void explicit_mode_ignores_the_reference() {
  Root r;
  r.reference(le(D_190) + le(D_265));
  const auto b = r.compile(FloatLiteralMode::B);
  SLRR_REQUIRE(holds(b, B_190) && holds(b, B_265));
  const auto d = r.compile(FloatLiteralMode::D);
  SLRR_REQUIRE(holds(d, D_190) && holds(d, D_265));
}

void default_options_stay_b() {
  Root r;
  r.reference(le(D_190) + le(D_265));
  const auto report = script_compile_all(r.root.string(), "probe");
  SLRR_REQUIRE(report.failed == 0 && report.succeeded == 1);
  const auto out = read(r.cls());
  SLRR_REQUIRE(holds(out, B_190) && holds(out, B_265));
}
}  // namespace

int main() {
  static constexpr standalone::Test tests[] = {
      auto_without_reference_is_b, auto_follows_a_stock_data_class, auto_follows_a_core_script,
      explicit_mode_ignores_the_reference, default_options_stay_b};
  return standalone::run("float_literal_mode", tests);
}
