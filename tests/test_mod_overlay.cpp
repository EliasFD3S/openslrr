#include "mod_overlay.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/tufa.hpp"
#include "slrr/utf8_path.hpp"
#include "standalone_test.hpp"
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

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
  // The definition (winner) is chosen by FQN priority: mods/z beats mods/a and base.
  SLRR_REQUIRE(overlay.provider("java.mods.extra.Marker") == expected);
  // A path request selects the valid base container when present (copied above)...
  const auto* base = overlay.container(cls);
  SLRR_REQUIRE(base && base->path == f.root / cls);
  const auto* via_mod_path = overlay.container(overlay.logical(f.root / "mods/a" / cls));
  SLRR_REQUIRE(via_mod_path && via_mod_path->path == f.root / cls);
  SLRR_REQUIRE(overlay.container("stock/Fallback.class") == nullptr);
  // ...else the highest-priority mod container.
  fs::remove(f.root / cls);
  ModOverlay no_base;
  no_base.scan(f.root, log);
  const auto* top = no_base.container(cls);
  SLRR_REQUIRE(top && top->path == expected);
  SLRR_REQUIRE(log.str().find("[mods] java.mods.extra.Marker: mods/z (overrides base; also in mods/a)") != std::string::npos);
}

void unusable_names_do_not_hide_valid_files() {
  Fixture f;
  f.manifest("m", R"("api":1)");
  const fs::path dir = "mods/m/system/Scripts/mods/m";
  f.put(dir / "src/Good.java", "package java.mods.m; public class Good {}");
  const std::wstring lone(1, static_cast<wchar_t>(0xd800));
  // Native paths intentionally cannot round-trip through UTF-8.
  for (const auto& path : {dir / (lone + L".class"), fs::path("base") / (L"bad." + lone),
                           fs::path("system/Scripts") / lone / "Nested.class"}) {
    f.put(path, "ignored");
    SLRR_REQUIRE(fs::is_regular_file(f.root / path));
  }
  const auto built = script_compile_all(path_to_utf8(f.root), path_to_utf8(dir));
  SLRR_REQUIRE(built.succeeded == 1 && built.failed == 0);
  ModOverlay overlay;
  std::ostringstream log;
  overlay.scan(f.root, log);
  SLRR_REQUIRE(overlay.provider("java.mods.m.Good") == f.root / dir / "Good.class");
  const auto text = log.str();
  for (const auto& parent : {dir, fs::path("base"), fs::path("system/Scripts")})
    SLRR_REQUIRE(text.find("skipped a name not representable in UTF-8 under " + path_to_utf8(f.root / parent)) != text.npos);
}

void invalid_containers_fall_through() {
  Fixture f;
  const fs::path dir = "system/Scripts/game";
  const auto cls = dir / "Stock.class";
  f.put(dir / "src/Stock.java", "package java.game; public class Stock {}");
  SLRR_REQUIRE(script_compile_all(path_to_utf8(f.root), path_to_utf8(dir)).succeeded == 1);
  f.manifest("a", R"("api":1)");
  f.manifest("b", R"("api":1)");
  f.put(fs::path("mods/a") / cls, "not TUFA");
  f.put(fs::path("mods/b") / cls, "");
  ModOverlay overlay;
  std::ostringstream log;
  overlay.scan(f.root, log);
  // Corrupt/empty mod containers provide nothing; the valid base file stays selected.
  const auto* base = overlay.container(cls);
  SLRR_REQUIRE(base && base->path == f.root / cls);
  SLRR_REQUIRE(overlay.provider("java.game.Stock") == f.root / cls);
  SLRR_REQUIRE(log.str().find("mods/a/system/Scripts/game/Stock.class:") != std::string::npos);
  // A valid mod container overrides the definition by FQN even though base also exists.
  fs::copy_file(f.root / cls, f.root / "mods/b" / cls, fs::copy_options::overwrite_existing);
  overlay.scan(f.root, log);
  SLRR_REQUIRE(overlay.provider("java.game.Stock") == f.root / "mods/b" / cls);
  base = overlay.container(cls);
  SLRR_REQUIRE(base && base->path == f.root / cls);
  // Without a valid base file the highest-priority VALID mod container is chosen;
  // corrupt mods/a is never selected.
  fs::remove(f.root / cls);
  overlay.scan(f.root, log);
  const auto* mod = overlay.container(cls);
  SLRR_REQUIRE(mod && mod->path == f.root / "mods/b" / cls);
  SLRR_REQUIRE(overlay.provider("java.game.Stock") == f.root / "mods/b" / cls);
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
  const auto* container = overlay.container(cls);
  SLRR_REQUIRE(container && container->path == physical);
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
// Duplicates inside one mod: the compiler and the runtime must pick the same
// file, by the shared order (lower-cased directory, then lower-cased name),
// not by the case-sensitive fs::path order. Each class is declared twice with
// different signatures: id() returns I in the file that must lose and F in
// the one that must win.
void duplicates_in_one_mod_use_compiler_order() {
  struct Source {
    fs::path rel;       // below mods/m
    std::string name;   // the primary class of the file
    const char* type;
  };
  auto run = [](const std::vector<Source>& sources, const fs::path& winner) {
    Fixture f;
    f.manifest("m", R"("api":1)");
    const fs::path consumer = "probe";
    f.put(consumer / "src/Consumer.java", "package java.game; public class Consumer { public static float read(){return Dup.id();} }");
    std::vector<fs::path> dirs;
    for (const auto& src : sources) {
      f.put("mods/m" / src.rel, std::string("package java.game; class ") + src.name + " {} class Dup { public static " +
                                    src.type + " id(){return 0;} }");
      const auto dir = (f.root / "mods/m" / src.rel).parent_path().parent_path();
      if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
    }
    for (const auto& dir : dirs) {
      const auto report = script_compile_all(path_to_utf8(f.root), path_to_utf8(fs::relative(dir, f.root)));
      SLRR_REQUIRE(report.attempted == report.succeeded && report.succeeded >= 1);
    }
    auto consumer_descriptor = [&] {
      const auto report = script_compile_all(path_to_utf8(f.root), path_to_utf8(consumer));
      SLRR_REQUIRE(report.attempted == 1 && report.succeeded == 1);
      std::vector<TufaClass> output;
      std::string why;
      SLRR_REQUIRE(tufa_load_file(path_to_utf8(f.root / consumer / "Consumer.class"), &output, &why));
      const auto& cls = output.at(0);
      for (uint32_t i = 0; i < cls.constants.size(); ++i) {
        if (cls.constants[i].kind != TufaConstKind::Reference) continue;
        const auto ref = cls.reference(i);
        if (ref.name == "id") return ref.descriptor;
      }
      return std::string();
    };
    SLRR_REQUIRE(consumer_descriptor() == "()F");
    ModOverlay overlay;
    std::ostringstream log;
    overlay.scan(f.root, log);
    const auto expected = f.root / "mods/m" / winner;
    SLRR_REQUIRE(overlay.provider("java.game.Dup") == expected);
    std::vector<TufaClass> blobs;
    std::string error;
    SLRR_REQUIRE(overlay.load(expected, blobs, error));
    bool float_id = false;
    for (const auto& blob : blobs)
      if (blob.name() == "java.game.Dup")
        for (const auto& m : blob.static_methods)
          if (blob.utf8(m.name) == "id" && blob.utf8(m.descriptor) == "()F") float_id = true;
    SLRR_REQUIRE(float_id);
  };
  // Directories: "Zeta" sorts before "alpha" case-sensitively, after it ignoring case.
  run({{"system/Scripts/Zeta/src/Holder.java", "Holder", "int"},
       {"system/Scripts/alpha/src/Holder.java", "Holder", "float"}},
      "system/Scripts/alpha/Holder.class");
  // File names in one directory: "B2" before "a2" case-sensitively, after it ignoring case.
  run({{"system/Scripts/game/src/B2.java", "B2", "int"}, {"system/Scripts/game/src/a2.java", "a2", "float"}},
      "system/Scripts/game/a2.class");
  // Exact filename preference beats the first container in sorted order.
  run({{"system/Scripts/game/src/A.java", "A", "int"}, {"system/Scripts/game/src/Dup.java", "Aux", "float"}},
      "system/Scripts/game/Dup.class");
}

// ---- PR #489 review 3 (findings 1-3): identity is by declared FQN only ----
void put_class_dir(Fixture& f, const fs::path& dir, const std::string& file, const std::string& text) {
  f.put(dir / "src" / file, text);
}

// #1: a mod's primary replaces stock Track from ANOTHER path (docs' own example)
// or under another file name; the stock secondary classes of that container
// must stay reachable through the overlay.
void secondary_survives_override_from_other_path() {
  for (const bool renamed : {false, true}) {
    Fixture f;
    const fs::path stock = "sl/Scripts/game";
    put_class_dir(f, stock, "Track.java",
        "package java.game; public class Track { public static int id(){return 0;} } class StockSibling { public static int v(){return 5;} }");
    SLRR_REQUIRE(script_compile_all(path_to_utf8(f.root), path_to_utf8(stock)).succeeded == 1);
    f.manifest("z", R"("api":1)");
    const fs::path mod = "mods/z/system/Scripts/game";
    if (renamed)  // differently named file declaring the same FQN
      put_class_dir(f, mod, "Other.java", "package java.game; public class Other {} class Track { public static int id(){return 2;} }");
    else
      put_class_dir(f, mod, "Track.java", "package java.game; public class Track { public static int id(){return 2;} }");
    SLRR_REQUIRE(script_compile_all(path_to_utf8(f.root), "mods/z").succeeded == 1);
    ModOverlay overlay;
    std::ostringstream log;
    overlay.scan(f.root, log);
    SLRR_REQUIRE(overlay.provider("java.game.Track") == f.root / mod / (renamed ? "Other.class" : "Track.class"));
    SLRR_REQUIRE(overlay.provider("java.game.StockSibling") == f.root / stock / "Track.class");
    SLRR_REQUIRE(log.str().find("[mods] java.game.Track: mods/z (overrides base") != std::string::npos);
    // The container that supplies the sibling must still load it.
    std::vector<TufaClass> blobs;
    std::string error;
    SLRR_REQUIRE(overlay.load(f.root / stock / "Track.class", blobs, error));
    bool sibling = false;
    for (const auto& blob : blobs) sibling |= blob.name() == "java.game.StockSibling";
    SLRR_REQUIRE(sibling);
  }
}

// #2: a mod file at the same relative path as a stock class but declaring a
// different FQN must not hide the stock class, in either direction.
void same_path_different_fqn_does_not_shadow() {
  Fixture f;
  put_class_dir(f, "system/Scripts/game", "Foo.java", "package java.game; public class Foo { public static int v(){return 5;} }");
  SLRR_REQUIRE(script_compile_all(path_to_utf8(f.root), "system/Scripts/game").succeeded == 1);
  f.manifest("m", R"("api":1)");
  const fs::path mod = "mods/m/sl/Scripts/game";
  put_class_dir(f, mod, "Foo.java", "package java.mods.m; public class Foo { public static int v(){return 6;} }");
  SLRR_REQUIRE(script_compile_all(path_to_utf8(f.root), "mods/m").succeeded == 1);
  ModOverlay overlay;
  std::ostringstream log;
  overlay.scan(f.root, log);
  SLRR_REQUIRE(overlay.provider("java.mods.m.Foo") == f.root / mod / "Foo.class");
  // Nobody in the mod declares java.game.Foo: the stock container provides it.
  SLRR_REQUIRE(overlay.provider("java.game.Foo") == f.root / "system/Scripts/game/Foo.class");
  SLRR_REQUIRE(log.str().find("java.game.Foo:") == std::string::npos);
}

// #3: directory junctions and symlinked directories are followed by the scan
// and by the compiler alike, with a cycle guard.
bool make_junction(const fs::path& link, const fs::path& target) {
  fs::create_directories(link.parent_path());
  const std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>nul";
  return std::system(cmd.c_str()) == 0 && fs::is_directory(link);
}
bool make_symlink(const fs::path& link, const fs::path& target) {
  fs::create_directories(link.parent_path());
  std::error_code ec;
  fs::create_directory_symlink(target, link, ec);  // may need privilege: caller skips
  return !ec && fs::is_directory(link);
}
// Removes the links themselves (never their targets) even when a check throws.
struct LinkGuard {
  std::vector<fs::path> links;
  void add(const fs::path& p) { links.push_back(p); }
  ~LinkGuard() {
    for (const auto& p : links) {
      std::error_code ec;
      if (fs::is_symlink(fs::symlink_status(p, ec)) || fs::exists(fs::symlink_status(p, ec)))
        fs::remove(p, ec);  // removes the junction/symlink entry only; never recurses
    }
  }
};
void linked_directories_are_followed() {
  for (const bool junction : {true, false}) {
    Fixture f;
    LinkGuard links;  // declared after f: unlinks before f's remove_all runs
    f.manifest("m", R"("api":1)");
    const fs::path pkg = "mods/m/system/Scripts/mods/m";
    f.put("ext/linked/src/J.java", "package java.mods.m.linked; public class J { public static int id(){return 1;} }");
    const fs::path link = f.root / pkg / "linked";
    links.add(link);
    links.add(f.root / pkg / "loop");
    if (!(junction ? make_junction(link, f.root / "ext/linked") : make_symlink(link, f.root / "ext/linked"))) {
      SLRR_REQUIRE(!junction);  // junctions need no privilege; symlinks may be unavailable
      std::puts("SKIP directory symlinks unavailable (no privilege)");
      continue;
    }
    // A cycle back to an ancestor, reachable from the same tree.
    SLRR_REQUIRE(junction ? make_junction(f.root / pkg / "loop", f.root / pkg)
                          : make_symlink(f.root / pkg / "loop", f.root / pkg));
    // The compiler sees the linked source once and terminates on the cycle.
    const auto built = script_compile_all(path_to_utf8(f.root), path_to_utf8(pkg));
    SLRR_REQUIRE(built.attempted == 1 && built.succeeded == 1 && built.failed == 0);
    SLRR_REQUIRE(fs::exists(f.root / "ext/linked/J.class"));
    ModOverlay overlay;
    std::ostringstream log;
    overlay.scan(f.root, log);
    const auto provider = overlay.provider("java.mods.m.linked.J");
    SLRR_REQUIRE(!provider.empty() && fs::equivalent(provider, f.root / "ext/linked/J.class"));
    // The same container reached through the loop is not a competing provider.
    SLRR_REQUIRE(log.str().find("also in") == std::string::npos);
    SLRR_REQUIRE(overlay.provider("java.mods.m.linked.J") == provider);
  }
}
}  // namespace

int main() {
  constexpr standalone::Test tests[]{gate_and_order, unusable_names_do_not_hide_valid_files, invalid_containers_fall_through,
                                     compiler_and_runtime_choose_same_file, cross_root_secondary_providers,
                                     unicode_folders_and_files, load_order_ignores_case_and_logs_unknown,
                                     invalid_order_line_is_skipped,
                                     duplicates_in_one_mod_use_compiler_order,
                                     secondary_survives_override_from_other_path,
                                     same_path_different_fqn_does_not_shadow, linked_directories_are_followed};
  return standalone::run("mod_overlay", tests);
}
