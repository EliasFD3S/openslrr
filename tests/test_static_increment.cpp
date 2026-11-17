#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/tree_eval.hpp"
using namespace slrr;
void compiled_increments() {
 Fixture f;fs::create_directories(f.root/"probe/src");
 std::ofstream(f.root/"probe/src/Counter.java")<<R"JAVA(package probe;
 public class Counter {
  static int day=1;
  static float time=86200.0;
  static int count=1;
  static float amount=1.5;
  static int uninitialized;
  int member=1;
  public static void spend(float dt){time+=dt;while(time>86400){time-=86400;day++;}}
  public static int statement(){count++;return count;}
  public static int post(){return count++;}
  public static int pre(){return ++count;}
  public static int down(){return --count;}
  public static int postDown(){return count--;}
  public static float floatStep(){amount++;return amount;}
  public static int foreign(){Other.count++;return Other.count;}
  public static int fresh(){uninitialized++;return uninitialized;}
  public static int twice(){return count++ + count++;}
  public static float floatPost(){return amount++;}
  public static float floatDown(){return --amount;}
  public int memberStep(){member++;return member;}
  public static int localStep(){int a=1;a++;return a;}
 }
 class Other {static int count=5;}
 )JAVA";
 auto report=script_compile_all(f.root.string(),"probe");SLRR_REQUIRE(report.failed==0 && report.succeeded==1);
 SLRR_REQUIRE(f.vm.load_class_file((f.root/"probe/Counter.class").string()));
 auto call=[&](const char* method){return f.vm.invoke_by_name("probe.Counter",method,{},true);};
 f.vm.invoke("probe.Counter","spend","(F)V",{Value::make_float(1000)},true);
 SLRR_REQUIRE(f.vm.get_static("probe.Counter","day").v.i==2);
 SLRR_REQUIRE(f.vm.get_static("probe.Counter","time").v.f==800);
 SLRR_REQUIRE(call("statement").v.i==2);
 SLRR_REQUIRE(call("post").v.i==2 && f.vm.get_static("probe.Counter","count").v.i==3);
 SLRR_REQUIRE(call("pre").v.i==4);
 SLRR_REQUIRE(call("down").v.i==3);
 SLRR_REQUIRE(call("postDown").v.i==3 && f.vm.get_static("probe.Counter","count").v.i==2);
 SLRR_REQUIRE(call("floatStep").v.f==2.5f);
 SLRR_REQUIRE(call("foreign").v.i==6);
 SLRR_REQUIRE(call("fresh").v.i==1);
 SLRR_REQUIRE(call("twice").v.i==5 && f.vm.get_static("probe.Counter","count").v.i==4);
 SLRR_REQUIRE(call("floatPost").v.f==2.5f && f.vm.get_static("probe.Counter","amount").v.f==3.5f);
 SLRR_REQUIRE(call("floatDown").v.f==2.5f);
 // Both field opcode forms must preserve the static lvalue, regardless of
 // which encoding the compiler or a shipped container uses.
 for(uint8_t opcode:{uint8_t(0x1b),uint8_t(0x1c)}) {
   auto cls=*f.vm.find_class("probe.Counter");
   for(const auto& method:cls.static_methods)if(cls.utf8(method.name)=="statement") {
     for(auto& n:cls.trees[method.tree])if(n.op==0x1b || n.op==0x1c)n.op=opcode;
     f.vm.set_static("probe.Counter","count",Value::make_int(10));
     const auto result=tree_eval(&f.vm,cls,method,nullptr,{});
     SLRR_REQUIRE(result.ok && result.value.v.i==11 && f.vm.get_static("probe.Counter","count").v.i==11);
   }
 }
 auto* object=f.vm.new_object("probe.Counter");
 SLRR_REQUIRE(f.vm.invoke_by_name("probe.Counter","memberStep",{Value::make_obj(object)},false).v.i==2);
 SLRR_REQUIRE(call("localStep").v.i==2);
 f.vm.set_static("probe.Counter","count",Value::make_int(INT32_MAX));
 SLRR_REQUIRE(call("pre").v.i==INT32_MIN);
 SLRR_REQUIRE(call("down").v.i==INT32_MAX);
 std::puts("PASS static pre/post ++/--, statement, float, foreign owner, local/instance regression and int wrap");
}
struct ClockHost : TreeHost {
 Object refresher;
 std::unordered_map<std::string,Value> fields;
 int synced=0,notified=0;float synced_time=-1;
 ClockHost(){refresher.class_name="java.lang.Thread";fields["day"]=Value::make_int(1);fields["time"]=Value::make_float(86200);fields["timeRefresher"]=Value::make_obj(&refresher);}
 Object* tree_new(const std::string&)override{throw std::runtime_error("unexpected allocation");}
 Object* tree_new_string(const std::string&)override{throw std::runtime_error("unexpected string");}
 Value tree_call(const std::string&,const std::string& method,const std::string&,Object*,const std::vector<Value>& args,bool)override{return effect(method,args);}
 Value tree_call_by_name(const std::string&,const std::string& method,const std::vector<Value>& args,bool)override{return effect(method,args);}
 Value effect(const std::string& method,const std::vector<Value>& args){
  if(method=="notify"){++notified;return Value::make_void();}
  if(method=="syncGameTime"){++synced;SLRR_REQUIRE(!args.empty() && args.back().tag==ValueTag::Float);synced_time=args.back().v.f;return Value::make_void();}
  throw std::runtime_error("unexpected call "+method);
 }
 Value tree_get_static(const std::string&,const std::string& field)override{return fields.at(field);}
 void tree_set_static(const std::string&,const std::string& field,Value v)override{fields[field]=v;}
 bool tree_instance_of(Object*,const std::string&)override{return false;}
 int tree_field_kind(const std::string&,const std::string& field)override{return fields.contains(field)?1:0;}
};
void shipped_spend_time() {
 std::vector<TufaClass> classes;std::string error;
 SLRR_REQUIRE(tufa_load_file("game/sl/Scripts/game/GameLogic.class",&classes,&error));
 for(const auto& cls:classes)if(cls.name()=="java.game.GameLogic")
  for(const auto& method:cls.static_methods)if(cls.utf8(method.name)=="spendTime"){
   ClockHost host;
   auto spend=[&](float dt){auto result=tree_eval(&host,cls,method,nullptr,{Value::make_float(dt)});SLRR_REQUIRE(result.ok);};
   spend(200); // Preserve stock's strict > midnight boundary, not >=.
   SLRR_REQUIRE(host.fields["day"].v.i==1 && host.fields["time"].v.f==86400);
   spend(1);SLRR_REQUIRE(host.fields["day"].v.i==2 && host.fields["time"].v.f==1);
   spend(3*86400+100);SLRR_REQUIRE(host.fields["day"].v.i==5 && host.fields["time"].v.f==101);
   SLRR_REQUIRE(host.notified==3 && host.synced==3 && host.synced_time==101);
   std::puts("PASS actual GameLogic.spendTime TREE: midnight boundary, single/multiple days, notification and clock sync");return;
  }
 throw std::runtime_error("GameLogic.spendTime not found");
}
int main(){const standalone::Test tests[]={compiled_increments,shipped_spend_time};return standalone::run("static_increment",tests);}
