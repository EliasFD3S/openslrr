#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/gameref.hpp"

#include <fcntl.h>
#include <initializer_list>
#include <io.h>
#include <iostream>
#include <sstream>
#include <iterator>
#include <sys/stat.h>

using namespace slrr;
fs::path compile(Fixture& f,const char* filename,const char* source) {
 fs::create_directories(f.root/"probe/src");
 std::ofstream(f.root/"probe/src"/filename)<<source;
 const auto result=script_compile_all(f.root.string(),"probe");
 SLRR_REQUIRE(result.failed==0 && result.succeeded>0);
 return f.root/"probe"/fs::path(filename).replace_extension(".class");
}
void write_container(const fs::path& target, std::initializer_list<fs::path> sources) {
  std::ofstream out(target, std::ios::binary);
  for (const auto& path : sources) {
    std::ifstream in(path, std::ios::binary);
    SLRR_REQUIRE(in);
    out << in.rdbuf();
    SLRR_REQUIRE(out);
  }
}
void primary_class() {
  Fixture f;
  const auto helper = compile(f, "Helper.java", "package probe; public class Helper {}");
  const auto main = compile(f, "Main.java", "package probe; public class Main {}");
  // Keep the inputs under probe/ so the requested container determines which
  // class comes first and the case of the file stem.
  const auto folded = f.root / "mAiN.class";
  const auto exact = f.root / "exact" / "Main.class";
  const auto fallback = f.root / "Unrelated.class";
  write_container(folded, {helper, main});
  fs::create_directory(exact.parent_path());
  write_container(exact, {helper, main});
  write_container(fallback, {helper, main});

  std::string primary;
  SLRR_REQUIRE(f.vm.load_class_file(folded.string(), &primary));
  SLRR_REQUIRE(primary == "probe.Main");
  SLRR_REQUIRE(f.vm.load_class_file(exact.string(), &primary));
  SLRR_REQUIRE(primary == "probe.Main");
  SLRR_REQUIRE(f.vm.load_class_file(fallback.string(), &primary));
  SLRR_REQUIRE(primary == "probe.Helper");
  std::puts("PASS resource path primary class: exact and mixed-case match, then first TUFA blob");
}
void stale_resource_path() {
  Fixture f;
  const auto original = compile(f, "ResourceScript.java",
      "package probe; public class ResourceScript { static int marker = 1; }");
  const auto other = compile(f, "OtherContainer.java",
      "package other; public class ResourceScript { static int marker = 2; }");
  const auto stale = f.root / "old" / "ResourceScript.class";
  const auto mod = f.root / "mods" / "resource-test";
  const auto mod_path = mod / "redirected" / "ResourceScript.class";
  fs::create_directories(stale.parent_path());
  fs::create_directories(mod_path.parent_path());
  std::ofstream(mod / "mod.json") << R"({"name":"Resource test","version":"1","api":1})";
  fs::copy_file(other, mod_path);
  SLRR_REQUIRE(f.vm.bootstrap_classpath());
  SLRR_REQUIRE(f.vm.load_class_file(original.string()));
  const auto* loaded = f.vm.find_class("probe.ResourceScript");
  SLRR_REQUIRE(loaded);
  f.vm.set_static("probe.ResourceScript", "marker", Value::make_int(42));
  SLRR_REQUIRE(f.vm.load_resource_class(stale.string()) == loaded);
  SLRR_REQUIRE(f.vm.get_static("probe.ResourceScript", "marker").v.i == 42);
  SLRR_REQUIRE(!f.vm.load_class_file(stale.string()));

  // The mod's file is authoritative at this path, even though the stem also
  // matches the already-loaded class from the unrelated package.
  const auto redirected = f.root / "redirected" / stale.filename();
  const auto* provided = f.vm.load_resource_class(redirected.string());
  SLRR_REQUIRE(provided && provided->name() == "other.ResourceScript");
  SLRR_REQUIRE(!f.vm.load_resource_class(stale.string()));  // now ambiguous

  // The fallback must not have cached a primary name for a file it never read.
  fs::copy_file(other, stale);
  SLRR_REQUIRE(f.vm.load_resource_class(stale.string()) == provided);
  std::puts("PASS stale resource paths reuse only a unique loaded stem and never hide a container");
}
void reentrant_load() {
  Fixture f;
  const auto initializer = compile(f, "Initializer.java", R"JAVA(package probe;
public class Initializer {
  static Initializer instance = new Initializer();
  public static native void reload();
  public Initializer() { reload(); }
}
)JAVA");
  const auto main = compile(f, "Primary.java", "package probe; public class Primary {}");
  const auto container = f.root / "Primary.class";
  write_container(container, {initializer, main});
  std::string nested_primary;
  const TufaClass* nested_class = nullptr;
  bool nested_ok = false;
  int callbacks = 0;
  f.registry.register_method("probe.Initializer", "reload", "()V",
      [&](Object*, const std::vector<Value>&) {
        if (++callbacks == 1) {
          nested_ok = f.vm.load_class_file(container.string(), &nested_primary);
          nested_class = f.vm.find_class(nested_primary);
        }
        return Value::make_void();
      });

  std::string primary;
  SLRR_REQUIRE(f.vm.load_class_file(container.string(), &primary));
  SLRR_REQUIRE(callbacks == 1);
  SLRR_REQUIRE(nested_ok);
  SLRR_REQUIRE(nested_primary == "probe.Primary" && primary == nested_primary);
  SLRR_REQUIRE(nested_class && nested_class == f.vm.find_class(primary));
  SLRR_REQUIRE(f.vm.load_class_file(container.string(), &primary));
  SLRR_REQUIRE(primary == nested_primary && callbacks == 1);
  std::puts("PASS static initializer re-entry returns the registered primary without reinitializing");
}
void class_lifetime() {
 Fixture f;
 const auto path=compile(f,"Container.java",R"JAVA(package probe;
 public class Persistent {
  static int value=7;
  public static native int reload();
  public static int running(){int before=value;reload();return before+value;}
 }
 class Companion { static int value=9; }
 )JAVA");
 SLRR_REQUIRE(f.vm.load_class_file(path.string()));
 const auto* cls=f.vm.find_class("probe.Persistent");SLRR_REQUIRE(cls);
 f.vm.set_static("probe.Persistent","value",Value::make_int(42));
 SLRR_REQUIRE(f.vm.load_class_file(path.string()));
 SLRR_REQUIRE(f.vm.get_static("probe.Persistent","value").v.i==42);
 SLRR_REQUIRE(f.vm.find_class("probe.Persistent")==cls);
 SLRR_REQUIRE(f.vm.load_class((path.parent_path()/"."/path.filename()).string()));
 SLRR_REQUIRE(f.vm.get_static("probe.Persistent","value").v.i==42);
 // Only reached after the repeated-load invariant passes: do not execute an
 // unsafe old-loader use-after-free scenario in the baseline regression.
 f.registry.register_method("probe.Persistent","reload","()I",[&](Object*,const std::vector<Value>&){return Value::make_int(f.vm.load_class_file(path.string()));});
 SLRR_REQUIRE(f.vm.invoke("probe.Persistent","running","()I",{},true).v.i==84);
 // A new container may include an existing class and an unseen sibling.
 const auto novel=compile(f,"Novel.java","package probe; public class Novel { static int value=13; }");
 const auto mixed=f.root/"Mixed.class";
 {std::ofstream out(mixed,std::ios::binary);for(auto file:{path,novel}){std::ifstream in(file,std::ios::binary);out<<in.rdbuf();}}
 SLRR_REQUIRE(f.vm.load_class_file(mixed.string()));
 SLRR_REQUIRE(f.vm.find_class("probe.Persistent")==cls && f.vm.get_static("probe.Persistent","value").v.i==42);
 SLRR_REQUIRE(f.vm.get_static("probe.Novel","value").v.i==13);
 // Already-loaded code remains usable even after its disk file is corrupted.
 std::ofstream(path,std::ios::binary)<<"not TUFA";
 SLRR_REQUIRE(f.vm.load_class_file(path.string()));
 SLRR_REQUIRE(f.vm.invoke("probe.Persistent","running","()I",{},true).v.i==84);
 const auto retry=f.root/"Retry.class";std::ofstream(retry)<<"bad";
 SLRR_REQUIRE(!f.vm.load_class_file(retry.string()));
 fs::copy_file(novel,retry,fs::copy_options::overwrite_existing);
 SLRR_REQUIRE(f.vm.load_class_file(retry.string()));
 std::puts("PASS immutable classes/statics, canonical path reuse, reload during method execution, concatenated containers and failed-load retry");
}
void constructors() {
 Fixture f;
 const auto path=compile(f,"Chain.java",R"JAVA(package probe;
 public class Chain {
  static int creates;
  int flags;
  public Chain(){this(null);}
  public Chain(int flags){this(null,flags);}
  public Chain(Chain parent){creates=creates+1;}
  public Chain(Chain parent,int flags){this(parent);this.flags=flags;}
  public static int pick(int value){return 1;}
  public static int pick(Chain value){return 2;}
  public static int chooseArray(int value){return 3;}
  public static int chooseArray(int[] value){return 4;}
 }
 )JAVA");
 SLRR_REQUIRE(f.vm.load_class_file(path.string()));
 auto construct=[&](std::vector<Value> params,int expected){
   auto* object=f.vm.new_object("probe.Chain");params.insert(params.begin(),Value::make_obj(object));
   f.vm.invoke_by_name("probe.Chain","<init>",params,false);
   const auto count=f.vm.get_static("probe.Chain","creates");
   std::printf("constructor expected=%d actual=%d tag=%d\n",expected,count.tag==ValueTag::Int?count.v.i:-1,int(count.tag));
   SLRR_REQUIRE(count.tag==ValueTag::Int && count.v.i==expected);return object;
 };
 auto* parent=construct({},1);
 construct({Value::make_int(5)},2);
 construct({Value::make_obj(nullptr)},3);
 auto* with_flags=construct({Value::make_obj(parent),Value::make_int(8)},4);
 SLRR_REQUIRE(with_flags->fields.at("flags").v.i==8);
 SLRR_REQUIRE(f.vm.invoke_by_name("probe.Chain","pick",{Value::make_obj(nullptr)},true).v.i==2);
 SLRR_REQUIRE(f.vm.invoke_by_name("probe.Chain","pick",{Value::make_int(0)},true).v.i==1);
 Object array;array.class_name="[I";array.is_array=true;
 SLRR_REQUIRE(f.vm.invoke_by_name("probe.Chain","chooseArray",{Value::make_obj(&array)},true).v.i==4);
 SLRR_REQUIRE(f.vm.invoke_by_name("probe.Chain","chooseArray",{Value::make_obj(nullptr)},true).v.i==4);
 std::puts("PASS general Dummy-shaped constructor chains, null/reference/primitive and array overload selection");
}
void stock_dummy() {
 Fixture f;gameref_clear();
 const auto game=fs::absolute("game").string();
 f.vm.set_game_root(game);f.packs.set_root(game);f.packs.open_lib("system.rpk");rpk_set_active(&f.packs);
 SLRR_REQUIRE(f.vm.bootstrap_classpath());
 register_gameref_natives(f.vm,f.packs,f.sections);
 auto make=[&](std::vector<Value> params){
   auto* object=f.vm.new_object("java.util.resource.Dummy");
   params.insert(params.begin(),Value::make_obj(object));
   f.vm.invoke_by_name("java.util.resource.Dummy","<init>",params,false);
   const auto* node=gameref_get(object);
   SLRR_REQUIRE(node && node->alias=="dummy" && node->live_id>=0x70000000);
   return object;
 };
 auto* parent=make({});
 auto* flagged=make({Value::make_int(128)});
 SLRR_REQUIRE((gameref_get_flags(flagged)&128)!=0);
 auto* child=make({Value::make_obj(parent)});
 SLRR_REQUIRE(gameref_get(child)->parent==parent);
 auto* fourth=make({Value::make_obj(parent),Value::make_int(128)});
 SLRR_REQUIRE(gameref_get(fourth)->parent==parent && (gameref_get_flags(fourth)&128)!=0);
 gameref_clear();rpk_set_active(nullptr);
 std::puts("PASS actual game Dummy.class: all four constructors create native roots and preserve parent/flags");
}

// ---- PR #489 review 3 (VM side) ----
namespace {
void put(const fs::path& p, const std::string& t) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << t;
}
// Run fn with the C stdout (fd 1) redirected to a file; returns what it printed.
template <class F> std::string capture_stdout(const fs::path& file, F&& fn) {
  std::fflush(stdout); std::cout.flush();
  // std::cout (overlay diagnostics) and C stdout (printf) are both captured.
  struct CoutRestore {
    std::ostringstream sink;
    std::streambuf* old = std::cout.rdbuf(sink.rdbuf());
    ~CoutRestore() { std::cout.rdbuf(old); }
  } cout_capture;
  const int saved = _dup(1);
  const int fd = _open(file.string().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IWRITE);
  SLRR_REQUIRE(saved >= 0 && fd >= 0);
  _dup2(fd, 1); _close(fd);
  std::string failure;
  try { fn(); } catch (const std::exception& e) { failure = e.what(); }
  std::fflush(stdout); std::cout.flush();
  _dup2(saved, 1); _close(saved);
  if (!failure.empty()) throw std::runtime_error(failure);
  std::ifstream in(file, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {}) + cout_capture.sink.str();
}
}  // namespace
// #1: stock secondary classes stay loadable when a mod overrides the primary
// from another path or under another file name (compiler/VM must agree).
void stock_secondary_survives_mod_override() {
  const std::string stock = "package java.game; public class Track { public static int id(){return 0;} } class StockSibling { public static int v(){return 5;} }";
  for (const int variant : {0, 1, 2})  // 0 no mod, 1 same name other root, 2 differently named file
  for (const bool sibling_first : {false, true}) {
    Fixture f;
    put(f.root / "sl/Scripts/game/src/Track.java", stock);
    SLRR_REQUIRE(script_compile_all(f.root.string(), "sl/Scripts/game").succeeded == 1);
    if (variant) {
      put(f.root / "mods/z/mod.json", R"({"name":"z","version":"1","api":1})");
      put(f.root / "mods/z/system/Scripts/game/src" / (variant == 1 ? "Track.java" : "Other.java"),
          variant == 1 ? "package java.game; public class Track { public static int id(){return 2;} }"
                       : "package java.game; public class Other {} class Track { public static int id(){return 2;} }");
      SLRR_REQUIRE(script_compile_all(f.root.string(), "mods/z").succeeded == 1);
    }
    SLRR_REQUIRE(f.vm.bootstrap_classpath());
    if (sibling_first) {
      SLRR_REQUIRE(f.vm.load_class("java.game.StockSibling"));
      SLRR_REQUIRE(f.vm.load_class("java.game.Track"));
    } else {
      SLRR_REQUIRE(f.vm.load_class("java.game.Track"));
      SLRR_REQUIRE(f.vm.load_class("java.game.StockSibling"));
    }
    SLRR_REQUIRE(f.vm.invoke("java.game.Track", "id", "()I", {}, true).v.i == (variant ? 2 : 0));
    SLRR_REQUIRE(f.vm.invoke("java.game.StockSibling", "v", "()I", {}, true).v.i == 5);
  }
  std::puts("PASS stock secondary classes survive a mod override from another path or file name");
}
// #2: package lookup selects by FQN, not by relative path. Both load orders.
void same_path_other_fqn_does_not_hide_stock() {
  for (const bool mod_first : {false, true}) {
    Fixture f;
    put(f.root / "system/Scripts/game/src/Foo.java", "package java.game; public class Foo { public static int v(){return 5;} }");
    SLRR_REQUIRE(script_compile_all(f.root.string(), "system/Scripts/game").succeeded == 1);
    put(f.root / "mods/m/mod.json", R"({"name":"m","version":"1","api":1})");
    put(f.root / "mods/m/sl/Scripts/game/src/Foo.java", "package java.mods.m; public class Foo { public static int v(){return 6;} }");
    SLRR_REQUIRE(script_compile_all(f.root.string(), "mods/m").succeeded == 1);
    SLRR_REQUIRE(f.vm.bootstrap_classpath());
    if (mod_first) SLRR_REQUIRE(f.vm.load_class("java.mods.m.Foo"));
    SLRR_REQUIRE(f.vm.load_class("java.game.Foo"));
    SLRR_REQUIRE(f.vm.load_class("java.mods.m.Foo"));
    SLRR_REQUIRE(f.vm.invoke("java.game.Foo", "v", "()I", {}, true).v.i == 5);
    SLRR_REQUIRE(f.vm.invoke("java.mods.m.Foo", "v", "()I", {}, true).v.i == 6);
  }
  std::puts("PASS a mod file at a stock path with another FQN neither hides nor replaces the stock class");
}
// #4: resource-path loads hit the in-memory cache first. A cached path keeps
// resolving after its file is gone and after another same-stem class appears.
void resource_cache_survives_removal_and_ambiguity() {
  Fixture f;
  const auto original = compile(f, "Cached.java", "package probe; public class Cached { static int marker = 1; }");
  put(f.root / "probe2/src/Cached.java", "package other; public class Cached { static int marker = 2; }");
  SLRR_REQUIRE(script_compile_all(f.root.string(), "probe2").succeeded == 1);
  SLRR_REQUIRE(f.vm.bootstrap_classpath());
  const auto* first = f.vm.load_resource_class(original.string());
  SLRR_REQUIRE(first && first->name() == "probe.Cached");
  f.vm.set_static("probe.Cached", "marker", Value::make_int(42));
  fs::remove(original);
  SLRR_REQUIRE(f.vm.load_resource_class(original.string()) == first);
  SLRR_REQUIRE(f.vm.get_static("probe.Cached", "marker").v.i == 42);
  // Now ambiguous by short name: the cache entry must still win, no guessing.
  SLRR_REQUIRE(f.vm.load_class_file((f.root / "probe2/Cached.class").string()));
  SLRR_REQUIRE(f.vm.find_class("other.Cached"));
  SLRR_REQUIRE(f.vm.load_resource_class(original.string()) == first);
  // Equivalent spellings of the cached path hit the same entry.
  SLRR_REQUIRE(f.vm.load_resource_class((original.parent_path() / "." / original.filename()).string()) == first);
  // A path never loaded and now ambiguous is not answered from that cache.
  SLRR_REQUIRE(!f.vm.load_resource_class((f.root / "never" / "Cached.class").string()));
  std::puts("PASS cached resource paths survive file removal and later ambiguity; no false guesses");
}
// #4: the warmed request key, not the physical path/status, decides. A request
// satisfied by a mod container stays bound to that primary even when a base
// file declaring an unrelated FQN later appears at the same request path.
void warmed_request_ignores_later_base_file() {
  Fixture f;
  put(f.root / "modsrc/src/Widget.java", "package other; public class Widget { static int marker = 2; }");
  put(f.root / "basesrc/src/Unrelated.java", "package probe; public class Unrelated {}");
  SLRR_REQUIRE(script_compile_all(f.root.string(), "modsrc").succeeded == 1);
  SLRR_REQUIRE(script_compile_all(f.root.string(), "basesrc").succeeded == 1);
  put(f.root / "mods/r/mod.json", R"({"name":"r","version":"1","api":1})");
  fs::create_directories(f.root / "mods/r/redirected");
  fs::copy_file(f.root / "modsrc/Widget.class", f.root / "mods/r/redirected/Widget.class");
  SLRR_REQUIRE(f.vm.bootstrap_classpath());
  const auto request = f.root / "redirected" / "Widget.class";  // missing in base
  SLRR_REQUIRE(!fs::exists(request));
  const auto* first = f.vm.load_resource_class(request.string());
  SLRR_REQUIRE(first && first->name() == "other.Widget");
  fs::create_directories(request.parent_path());
  fs::copy_file(f.root / "basesrc/Unrelated.class", request);
  const auto* again = f.vm.load_resource_class(request.string());
  SLRR_REQUIRE(again == first);
  SLRR_REQUIRE(!f.vm.find_class("probe.Unrelated"));
  std::puts("PASS a warmed resource request keeps its mod primary when an unrelated base file appears later");
}
// #5: a missing resource script whose stem is ambiguous among loaded classes.
// The contract (docs/scripting/mods.md) refuses; the MWM audit found no stale paths.
void ambiguous_missing_resource_stem() {
  Fixture f;
  const auto a = compile(f, "Dupe.java", "package probe; public class Dupe {}");
  put(f.root / "probe2/src/Dupe.java", "package other; public class Dupe {}");
  SLRR_REQUIRE(script_compile_all(f.root.string(), "probe2").succeeded == 1);
  SLRR_REQUIRE(f.vm.bootstrap_classpath());
  SLRR_REQUIRE(f.vm.load_class_file(a.string()));
  SLRR_REQUIRE(f.vm.load_class_file((f.root / "probe2/Dupe.class").string()));
  const auto missing = f.root / "gone" / "Dupe.class";
  SLRR_REQUIRE(!f.vm.load_resource_class(missing.string()));
  // Not cached: once a real file exists at the path it is authoritative.
  fs::create_directories(missing.parent_path());
  fs::copy_file(f.root / "probe2/Dupe.class", missing);
  const auto* real = f.vm.load_resource_class(missing.string());
  SLRR_REQUIRE(real && real->name() == "other.Dupe");
  std::puts("PASS ambiguous missing resource stem follows the documented policy and is never cached");
}
// #6: one diagnostic per unencodable subtree, with the right wording; encodable
// siblings are still added to the classpath.
void unencodable_subtree_logs_once() {
  Fixture f;
  const std::wstring lone1(1, static_cast<wchar_t>(0xd800)), lone2(1, static_cast<wchar_t>(0xdc00));
  for (const auto& lone : {lone1, lone2})
    for (const wchar_t* tail : {L"a/b/c", L"a/d", L"e"})
      fs::create_directories(f.root / "system/Scripts" / lone / tail);
  put(f.root / "system/Scripts/good/src/Marker.java", "package java.good; public class Marker { public static int id(){return 3;} }");
  SLRR_REQUIRE(script_compile_all(f.root.string(), "system/Scripts/good").succeeded == 1);
  const auto text = capture_stdout(f.root / "boot.log", [&] { SLRR_REQUIRE(f.vm.bootstrap_classpath()); });
  size_t lines = 0, pos = 0;
  while ((pos = text.find("UTF-8", pos)) != std::string::npos) { ++lines; pos += 5; }
  if (lines != 2 || text.find("unreadable directory") != std::string::npos)
    std::fprintf(stderr, "---- captured boot log ----\n%s\n---- end ----\n", text.c_str());
  SLRR_REQUIRE(lines == 2);  // one per unencodable subtree, not per descendant
  SLRR_REQUIRE(text.find("unreadable directory") == std::string::npos);
  SLRR_REQUIRE(f.vm.load_class("java.good.Marker"));
  std::puts("PASS one correctly worded diagnostic per unencodable subtree");
}
int main(){const standalone::Test tests[]={primary_class,stale_resource_path,reentrant_load,class_lifetime,constructors,stock_dummy,
 stock_secondary_survives_mod_override,same_path_other_fqn_does_not_hide_stock,resource_cache_survives_removal_and_ambiguity,
 ambiguous_missing_resource_stem,warmed_request_ignores_later_base_file,unencodable_subtree_logs_once};return standalone::run("class_lifetime",tests);}
