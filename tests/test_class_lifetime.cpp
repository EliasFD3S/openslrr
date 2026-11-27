#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/gameref.hpp"

#include <initializer_list>

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
int main(){const standalone::Test tests[]={primary_class,stale_resource_path,reentrant_load,class_lifetime,constructors,stock_dummy};return standalone::run("class_lifetime",tests);}
