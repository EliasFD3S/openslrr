#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/gameref.hpp"
using namespace slrr;
fs::path compile(Fixture& f,const char* filename,const char* source) {
 fs::create_directories(f.root/"probe/src");
 std::ofstream(f.root/"probe/src"/filename)<<source;
 const auto result=script_compile_all(f.root.string(),"probe");
 SLRR_REQUIRE(result.failed==0 && result.succeeded>0);
 return f.root/"probe"/fs::path(filename).replace_extension(".class");
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
 std::puts("PASS immutable classes/statics, canonical path reuse, reentrant load, concatenated containers and failed-load retry");
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
int main(){const standalone::Test tests[]={class_lifetime,constructors,stock_dummy};return standalone::run("class_lifetime",tests);}
