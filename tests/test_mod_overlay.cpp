#include "mod_overlay.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/tufa.hpp"
#include "standalone_test.hpp"
#include <chrono>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace slrr;
namespace {
struct Fixture {
  fs::path root = fs::temp_directory_path() / ("slrr_overlay_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
  void put(const fs::path& rel, const std::string& text) {
    fs::create_directories((root / rel).parent_path());
    std::ofstream(root / rel, std::ios::binary) << text;
  }
  void manifest(const std::string& folder, const std::string& fields) {
    put("mods/" + folder + "/mod.json", "{\"name\":\"Mód\",\"version\":\"1.2\"," + fields + "}");
  }
};

void gate_and_order() {
  Fixture f;
  f.manifest("alpha", R"("api":0)");
  f.manifest("beta", R"("api":1)");
  f.manifest("winner", R"("api":1,"entry":"java.mods.extra.Mod")");
  f.manifest("future", R"("api":2)");
  f.manifest("off", R"("api":1,"enabled":false)");
  f.manifest("malformed", R"("api":01)");
  f.manifest("wrongtype", R"("api":"1")");
  f.manifest("null", R"("api":1,"entry":null)");
  f.manifest("dup", R"("api":2,"api":1)");
  f.put("mods/nomanifest/readme", "");
  f.put("mods/load_order.txt", "# priority\r\n off\r\n winner # inline\r\nunknown\nwinner\n");
  std::ostringstream log;
  const auto mods = discover_mods(f.root, &log);
  SLRR_REQUIRE(mods.size() == 3 && mods[0].folder == "winner");
  SLRR_REQUIRE(mods[1].folder == "alpha" && mods[2].folder == "beta");
  SLRR_REQUIRE(mods[0].entry == "java.mods.extra.Mod" && mods[1].entry.empty());
  publish_mod_list(mods);
  SLRR_REQUIRE(mods_loaded().front().name == "Mód" && mods_loaded().front().version == "1.2");
  for (auto folder : {"future", "off", "malformed", "wrongtype", "null", "dup", "nomanifest"})
    SLRR_REQUIRE(log.str().find(std::string("mods/") + folder + ": skipped") != std::string::npos);
}

void compiler_and_runtime_choose_same_file() {
  Fixture f;
  const fs::path dir = "system/Scripts/mods/extra";
  const auto source = dir / "src/Marker.java";
  const auto cls = dir / "Marker.class";
  const std::string valid = "public class Marker { public static int id() { return 7; } }";
  // No package statement: infer it from the logical directory under the mod.
  f.manifest("a", R"("api":1)");
  f.manifest("z", R"("api":1)");
  f.manifest("off", R"("api":1,"enabled":false)");
  f.put("mods/load_order.txt", "z\na\n");
  f.put("mods/z" / source, valid);
  f.put("mods/a" / source, "not Java");
  f.put("mods/off" / source, "not Java");
  f.put(source, "not Java");
  auto compiled = script_compile_all(f.root.string(), dir.string());
  SLRR_REQUIRE(compiled.succeeded == 1 && compiled.attempted == 1);
  SLRR_REQUIRE(!fs::exists(f.root / cls) && !fs::exists(f.root / "mods/a" / cls));
  f.put(cls, "base");
  f.put("mods/a" / cls, "shadowed");
  std::ostringstream log;
  ModOverlay overlay;
  overlay.scan(f.root, log);
  const auto expected = f.root / "mods/z" / cls;
  SLRR_REQUIRE(overlay.resolve(cls) == expected);
  SLRR_REQUIRE(overlay.resolve(overlay.logical(f.root / "mods/a" / cls)) == expected);
  SLRR_REQUIRE(overlay.resolve("stock/Fallback.class") == f.root / "stock/Fallback.class");
  SLRR_REQUIRE(log.str().find("[mods] java.mods.extra.Marker: mods/z (overrides base; also in mods/a)") != std::string::npos);
}
}  // namespace

int main() {
  constexpr standalone::Test tests[]{gate_and_order, compiler_and_runtime_choose_same_file};
  return standalone::run("mod_overlay", tests);
}
