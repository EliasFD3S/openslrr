#include "mod_overlay.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/tufa.hpp"
#include "slrr/utf8_path.hpp"
#include "standalone_test.hpp"
#include <chrono>
#include <algorithm>
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
  fs::copy_file(f.root / "mods/z" / cls, f.root / cls);
  fs::copy_file(f.root / "mods/z" / cls, f.root / "mods/a" / cls);
  std::ostringstream log;
  ModOverlay overlay;
  overlay.scan(f.root, log);
  const auto expected = f.root / "mods/z" / cls;
  SLRR_REQUIRE(overlay.resolve(cls) == expected);
  SLRR_REQUIRE(overlay.resolve(overlay.logical(f.root / "mods/a" / cls)) == expected);
  SLRR_REQUIRE(overlay.resolve("stock/Fallback.class") == f.root / "stock/Fallback.class");
  SLRR_REQUIRE(log.str().find("[mods] java.mods.extra.Marker: mods/z (overrides base; also in mods/a)") != std::string::npos);
}

void cross_root_secondary_providers() {
  Fixture f;
  const fs::path stock = "sl/Scripts/game";
  const fs::path low = fs::path("mods/a") / stock;
  const fs::path high = "mods/z/system/Scripts/game";
  auto compile = [&](const fs::path& dir) {
    const auto report = script_compile_all(path_to_utf8(f.root), path_to_utf8(dir));
    SLRR_REQUIRE(report.attempted == 1 && report.succeeded == 1);
  };
  f.put(stock / "src/Track.java", "package java.game; public class Track { public static int id(){return 0;} } class StockSibling {}");
  compile(stock);
  f.manifest("a", R"("api":1)");
  f.put(low / "src/Track.java", "package java.game; public class Track { public static int id(){return 1;} } class LowSibling {}");
  compile(low);
  f.manifest("z", R"("api":1)");
  f.put("mods/load_order.txt", "z\na\n");
  f.put(high / "src/Bundle.java", "package java.game; public class Bundle {} class Track { public static float id(){return 2.0;} }");
  const fs::path consumer = "probe";
  f.put(consumer / "src/Consumer.java", "package java.game; public class Consumer { public static float read(){return Track.id();} }");
  // Both source-only and compiled dependencies must prefer z's secondary
  // over a named Track.class in the earlier stock classpath root.
  auto check_consumer = [&] {
    compile(consumer);
    std::vector<TufaClass> output;
    std::string why;
    SLRR_REQUIRE(tufa_load_file(path_to_utf8(f.root / consumer / "Consumer.class"), &output, &why));
    const auto& cls = output.at(0);
    bool float_call = false;
    for (uint32_t i = 0; i < cls.constants.size(); ++i) {
      if (cls.constants[i].kind != TufaConstKind::Reference) continue;
      const auto ref = cls.reference(i);
      if (ref.name == "id" && ref.descriptor == "()F") float_call = true;
    }
    SLRR_REQUIRE(float_call);
  };
  check_consumer();
  compile(high);
  check_consumer();

  ModOverlay overlay;
  std::ostringstream log;
  overlay.scan(f.root, log);
  SLRR_REQUIRE(overlay.provider("java.game.Track") == f.root / high / "Bundle.class");
  SLRR_REQUIRE(overlay.provider("java.game.StockSibling") == f.root / stock / "Track.class");
  SLRR_REQUIRE(overlay.provider("java.game.LowSibling") == f.root / low / "Track.class");
  SLRR_REQUIRE(log.str().find("[mods] java.game.Track: mods/z (overrides base; also in mods/a)") != std::string::npos);
  for (const auto& dir : {stock, low}) {
    std::vector<TufaClass> blobs;
    std::string error;
    SLRR_REQUIRE(overlay.load(f.root / dir / "Track.class", blobs, error));
    SLRR_REQUIRE(blobs.size() == 2 && blobs.front().name() == "java.game.Track");
    const auto& track = blobs.front();
    bool winning_method = false;
    for (const auto& m : track.static_methods)
      if (track.utf8(m.name) == "id" && track.utf8(m.descriptor) == "()F") winning_method = true;
    SLRR_REQUIRE(winning_method);
    SLRR_REQUIRE(blobs.back().name() == (dir == stock ? "java.game.StockSibling" : "java.game.LowSibling"));
  }
}

fs::path wide(const std::u8string& text) { return fs::path(text); }

void unicode_folders_and_files() {
  Fixture f;
  const auto jp = wide(u8"日本");        // not representable in the ANSI code page
  const auto off = wide(u8"無効");
  const auto bare = wide(u8"空");
  const auto accent = wide(u8"Ñandú");
  f.put(fs::path("mods") / jp / "mod.json", R"({"name":"Nihon","version":"1","api":1})");
  f.put(fs::path("mods") / accent / "mod.json", R"({"name":"Accent","version":"1","api":1})");
  f.put(fs::path("mods") / off / "mod.json", R"({"name":"Off","version":"1","api":1,"enabled":false})");
  f.put(fs::path("mods") / bare / "readme", "");
  // A real compiled TUFA container under a non-ANSI file name.
  const fs::path dir = "system/Scripts/mods/jp";
  f.put(fs::path("mods") / jp / dir / "src/Marker.java",
        "public class Marker { public static int id() { return 7; } }");
  const auto built = script_compile_all(f.root.string(), dir.string());
  SLRR_REQUIRE(built.succeeded == 1 && built.failed == 0);
  const auto cls = dir / wide(u8"日本.class");
  fs::rename(f.root / "mods" / jp / dir / "Marker.class", f.root / "mods" / jp / cls);
  std::ostringstream log;
  std::vector<ModManifest> mods;
  try { mods = discover_mods(f.root, &log); } catch (const std::exception&) { SLRR_REQUIRE(!"discover_mods threw"); }
  SLRR_REQUIRE(mods.size() == 2);
  const auto nihon = std::find_if(mods.begin(), mods.end(), [](const auto& m) { return m.name == "Nihon"; });
  SLRR_REQUIRE(nihon != mods.end() && nihon->folder == "\xe6\x97\xa5\xe6\x9c\xac");  // UTF-8, not ANSI
  SLRR_REQUIRE(log.str().find("mods/\xe7\x84\xa1\xe5\x8a\xb9: skipped (disabled)") != std::string::npos);
  SLRR_REQUIRE(log.str().find("mods/\xe7\xa9\xba: skipped") != std::string::npos);
  ModOverlay overlay;
  try { overlay.scan(f.root, log); } catch (const std::exception&) { SLRR_REQUIRE(!"scan threw"); }
  const auto physical = f.root / "mods" / jp / cls;
  SLRR_REQUIRE(overlay.resolve(cls) == physical);
  SLRR_REQUIRE(overlay.logical(physical) == cls);
  SLRR_REQUIRE(overlay.provider("java.mods.jp.Marker") == physical);
  std::vector<TufaClass> blobs;
  std::string error;
  SLRR_REQUIRE(overlay.load(physical, blobs, error) && blobs.size() == 1);
  SLRR_REQUIRE(blobs.front().name() == "java.mods.jp.Marker");
  // Windows folder names ignore case in any script.
  SLRR_REQUIRE(overlay.logical(f.root / "mods" / wide(u8"ñANDÚ") / "x.class") == fs::path("x.class"));
}

void invalid_order_line_is_skipped() {
  Fixture f;
  f.manifest("alpha", R"("api":1)");
  f.manifest("beta", R"("api":1)");
  f.put("mods/load_order.txt", "bad\xff\xfe\nbeta\n");
  std::ostringstream log;
  std::vector<ModManifest> mods;
  try { mods = discover_mods(f.root, &log); } catch (const std::exception&) { SLRR_REQUIRE(!"discover_mods threw"); }
  SLRR_REQUIRE(mods.size() == 2 && mods[0].folder == "beta" && mods[1].folder == "alpha");
  SLRR_REQUIRE(log.str().find("load_order.txt line 1: not valid UTF-8") != std::string::npos);
}

void load_order_ignores_case_and_logs_unknown() {
  Fixture f;
  for (auto folder : {"Zed", "alpha", "Beta", "gamma"}) f.manifest(folder, R"("api":1)");
  // Mixed-case entry, an unknown folder and a repeat.
  f.put("mods/load_order.txt", "ZED\nghost\nzed\n");
  std::ostringstream log;
  auto mods = discover_mods(f.root, &log);
  SLRR_REQUIRE(mods.size() == 4 && mods[0].folder == "Zed");
  // Unlisted folders follow case-insensitively: alpha, Beta, gamma.
  SLRR_REQUIRE(mods[1].folder == "alpha" && mods[2].folder == "Beta" && mods[3].folder == "gamma");
  SLRR_REQUIRE(log.str().find("load_order.txt line 2: no mods/ghost folder") != std::string::npos);
  SLRR_REQUIRE(log.str().find("line 3: mods/zed is listed again") != std::string::npos);
  std::ostringstream quiet;
  f.put("mods/load_order.txt", "");
  mods = discover_mods(f.root, &quiet);
  SLRR_REQUIRE(mods[0].folder == "alpha" && mods[1].folder == "Beta" && mods[3].folder == "Zed");
}
}  // namespace

int main() {
  constexpr standalone::Test tests[]{gate_and_order, compiler_and_runtime_choose_same_file, cross_root_secondary_providers,
                                     unicode_folders_and_files, load_order_ignores_case_and_logs_unknown,
                                     invalid_order_line_is_skipped};
  return standalone::run("mod_overlay", tests);
}
