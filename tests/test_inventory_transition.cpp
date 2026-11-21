#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/tree_eval.hpp"
using namespace slrr;
void transition() {
 Fixture f;fs::create_directories(f.root/"probe/src");
 std::ofstream(f.root/"probe/src/Flow.java")<<R"JAVA(package probe;
 public class Flow {
  static Stage actual;
  static int parts=0;
  static int saved=-1;
  static int order=0;
  public static void change(Stage state){
   if(actual){actual.exit(state);}
   Stage old=actual;actual=state;
   if(actual){actual.enter(old);}
  }
  public static void leave(Stage state){change(state);saved=parts;}
 }
 interface Stage { public void exit(Stage next); public void enter(Stage old); }
 class Previous implements Stage {
  public void exit(Stage next){Flow.parts=3;Flow.order=Flow.order*10+1;}
  public void enter(Stage old){Flow.order=Flow.order*10+9;}
 }
 class Next implements Stage {
  public void enter(Stage old){Flow.order=Flow.order*10+2;}
  public void exit(Stage next){Flow.order=Flow.order*10+8;}
 }
 )JAVA";
 const auto result=script_compile_all(f.root.string(),"probe");SLRR_REQUIRE(result.failed==0 && result.succeeded==1);
 SLRR_REQUIRE(f.vm.load_class_file((f.root/"probe/Flow.class").string()));
 auto* previous=f.vm.new_object("probe.Previous");auto* next=f.vm.new_object("probe.Next");
 f.vm.set_static("probe.Flow","actual",Value::make_obj(previous));
 f.vm.invoke_by_name("probe.Flow","leave",{Value::make_obj(next)},true);
 const auto saved=f.vm.get_static("probe.Flow","saved");
 const auto order=f.vm.get_static("probe.Flow","order");
 std::printf("saved=%d order=%d\n",saved.v.i,order.v.i);
 SLRR_REQUIRE(saved.tag==ValueTag::Int && saved.v.i==3);
 SLRR_REQUIRE(order.v.i==12);
 SLRR_REQUIRE(f.vm.get_static("probe.Flow","actual").v.o==next);
 std::puts("PASS interface exit/enter runs before subsequent inventory snapshot, exactly once");
}

struct DispatchProbe : TreeHost {
 Object old,next;
 Value actual;
 std::vector<std::string> calls;
 DispatchProbe(){old.class_name="java.game.Garage";next.class_name="java.game.MainMenu";actual=Value::make_obj(&old);}
 Object* tree_new(const std::string&)override{throw std::runtime_error("unexpected allocation");}
 Object* tree_new_string(const std::string&)override{throw std::runtime_error("unexpected string");}
 Value effect(const std::string& owner,const std::string& name,Object* self){
  std::printf("dispatch %s.%s self=%s\n",owner.c_str(),name.c_str(),self?self->class_name.c_str():"null");
  calls.push_back(owner+"."+name);return Value::make_void();
 }
 Value tree_call(const std::string& owner,const std::string& name,const std::string&,Object* self,const std::vector<Value>&,bool)override{return effect(owner,name,self);}
 Value tree_call_by_name(const std::string& owner,const std::string& name,const std::vector<Value>& args,bool is_static)override{return effect(owner,name,!is_static&&!args.empty()&&args[0].tag==ValueTag::Obj?args[0].v.o:nullptr);}
 Value tree_get_static(const std::string&,const std::string& field)override{SLRR_REQUIRE(field=="actualState");return actual;}
 void tree_set_static(const std::string&,const std::string& field,Value value)override{SLRR_REQUIRE(field=="actualState");actual=value;}
 bool tree_instance_of(Object*,const std::string&)override{return false;}
 int tree_field_kind(const std::string&,const std::string& field)override{return field=="actualState"?1:0;}
};
void shipped_transition() {
 std::vector<TufaClass> classes;std::string error;
 SLRR_REQUIRE(tufa_load_file("game/sl/Scripts/game/GameLogic.class",&classes,&error));
 for(const auto& cls:classes)if(cls.name()=="java.game.GameLogic")
  for(const auto& method:cls.static_methods)if(cls.utf8(method.name)=="changeActiveSection"){
   DispatchProbe host;const auto result=tree_eval(&host,cls,method,nullptr,{Value::make_obj(&host.next)});
   SLRR_REQUIRE(result.ok);
   SLRR_REQUIRE(host.calls==std::vector<std::string>({"java.game.Garage.exit","java.game.MainMenu.enter"}));
   SLRR_REQUIRE(host.actual.v.o==&host.next);
   std::puts("PASS shipped GameLogic transition dispatches lifecycle to actual receiver classes");return;
  }
 throw std::runtime_error("changeActiveSection not found");
}

struct InventoryProbe : DispatchProbe {
 Object mechanic,inventory,player,parts;
 int moves=0;
 InventoryProbe(){
  mechanic.class_name="java.game.Mechanic";inventory.class_name="java.game.VisualInventory";
  player.class_name="java.game.Player";parts.class_name="java.game.Inventory";
  mechanic.fields["inventory"]=Value::make_obj(&inventory);mechanic.fields["player"]=Value::make_obj(&player);
  player.fields["parts"]=Value::make_obj(&parts);
 }
 Value tree_call(const std::string& owner,const std::string& name,const std::string&,Object* self,const std::vector<Value>& args,bool)override{return inv_effect(owner,name,self,args);}
 Value tree_call_by_name(const std::string& owner,const std::string& name,const std::vector<Value>& args,bool is_static)override{
  auto values=args;Object* self=nullptr;if(!is_static&&!values.empty()&&values[0].tag==ValueTag::Obj){self=values[0].v.o;values.erase(values.begin());}
  return inv_effect(owner,name,self,values);
 }
 Value inv_effect(const std::string& owner,const std::string& name,Object* self,const std::vector<Value>& args){
  if(name=="size"){SLRR_REQUIRE(self==&inventory);return Value::make_int(3);}
  if(name=="moveToInventory"){
   SLRR_REQUIRE(self==&inventory && args.size()==2 && args[0].tag==ValueTag::Int && args[0].v.i==0 && args[1].tag==ValueTag::Obj && args[1].v.o==&parts);
   ++moves;return Value::make_void();
  }
  throw std::runtime_error("unexpected inventory call "+name);
 }
 int tree_field_kind(const std::string&,const std::string&)override{return 2;}
};
void shipped_inventory_flush() {
 std::vector<TufaClass> classes;std::string error;
 SLRR_REQUIRE(tufa_load_file("game/sl/Scripts/game/Mechanic.class",&classes,&error));
 for(const auto& cls:classes)if(cls.name()=="java.game.Mechanic")
  for(const auto& method:cls.instance_methods)if(cls.utf8(method.name)=="flushInventory"){
   InventoryProbe host;const auto result=tree_eval(&host,cls,method,&host.mechanic,{});
   SLRR_REQUIRE(result.ok && host.moves==3);
   std::puts("PASS actual Mechanic.flushInventory moves every item to player.parts");return;
  }
 throw std::runtime_error("flushInventory not found");
}

struct GarageProbe : InventoryProbe {
 std::vector<TufaClass> definitions;
 Object camera,painter,cans,controller,cursor;
 std::vector<std::unique_ptr<Object>> allocated;
 int saved=-1,exit_calls=0,enter_calls=0;
 // Command probe (#235 review): record transitions, saves and warnings
 // instead of running them, with a controllable clock and car state.
 bool record_only=false;
 std::vector<std::string> events;
 float hour=12;
 Value problem=Value::make_obj(nullptr);
 GarageProbe(const std::string& garage_path="game/sl/Scripts/game/Garage.class"){
  std::string error;
  for(const char* name:{"Garage","GameLogic","Mechanic"}){
   std::vector<TufaClass> values;
   SLRR_REQUIRE(tufa_load_file(std::string(name)=="Garage"?garage_path:std::string("game/sl/Scripts/game/")+name+".class",&values,&error));
   for(auto& cls:values)definitions.push_back(std::move(cls));
  }
  camera.class_name="java.render.Camera";painter.class_name="java.game.Painter";cans.class_name="java.game.PaintCanInventory";
  controller.class_name="java.io.Controller";cursor.class_name="java.io.Cursor";
  old.fields["mechanic"]=Value::make_obj(&mechanic);old.fields["player"]=Value::make_obj(&player);
  old.fields["camera"]=Value::make_obj(&camera);old.fields["painter"]=Value::make_obj(&painter);
  old.fields["parentState"]=Value::make_obj(&next);
  painter.fields["paintCans"]=Value::make_obj(&cans);player.fields["controller"]=Value::make_obj(&controller);
 }
 Value run(const std::string& owner,const std::string& name,Object* self,const std::vector<Value>& args){
  for(const auto& cls:definitions)if(cls.name()==owner){
   const auto& methods=self?cls.instance_methods:cls.static_methods;
   for(const auto& method:methods)if(cls.utf8(method.name)==name){auto result=tree_eval(this,cls,method,self,args);SLRR_REQUIRE(result.ok);return result.value;}
  }
  throw std::runtime_error("missing stock method "+owner+"."+name);
 }
 Object* tree_new(const std::string& name)override{allocated.push_back(std::make_unique<Object>());allocated.back()->class_name=name;return allocated.back().get();}
 Object* tree_new_string(const std::string& value)override{auto* out=tree_new("java.lang.String");out->str=value;return out;}
 Value tree_get_static(const std::string& owner,const std::string& field)override{
  if(field=="actualState")return actual;
  if(field=="cursor")return Value::make_obj(&cursor);
  for(const auto& cls:definitions)if(cls.name()==owner)
   for(const auto& member:cls.static_fields)if(cls.utf8(member.name)==field && member.tree<cls.trees.size())
    for(const auto& node:cls.trees[member.tree])if(node.op==0x0b && node.wide)return Value::make_int(int32_t(node.imm));
  return Value::make_int(0);
 }
 void tree_set_static(const std::string& owner,const std::string& field,Value value)override{
  if(record_only&&field!="actualState")return; // dealer timestamps, car lists
  DispatchProbe::tree_set_static(owner,field,value);
 }
 int tree_field_kind(const std::string& owner,const std::string& field)override{
  for(const auto& cls:definitions)if(cls.name()==owner)
   for(const auto& member:cls.static_fields)if(cls.utf8(member.name)==field)return 1;
  return field=="actualState" || field=="cursor"?1:2;
 }
 Value inv_effect(const std::string& owner,const std::string& name,Object* self,const std::vector<Value>& args){
  if(record_only){
   if(name=="changeActiveSection"){events.push_back("change");return Value::make_void();}
   if(name=="autoSaveQuiet"||name=="autoSave"){events.push_back("save");return Value::make_void();}
   if(name=="giveWarning"){events.push_back("warn");return Value::make_void();}
   if(name=="getTime")return Value::make_float(hour*3600);
   if(name=="isDriveable")return problem;
  }
  if(name=="changeActiveSection")return run("java.game.GameLogic",name,nullptr,args);
  if(name=="exit"){++exit_calls;return run("java.game.Garage",name,&old,args);}
  if(name=="enter"){++enter_calls;return Value::make_void();}
  if(name=="flushInventory")return run("java.game.Mechanic",name,&mechanic,args);
  if(name=="autoSaveQuiet"){saved=moves;return Value::make_void();}
  if(name=="size" || name=="moveToInventory")return InventoryProbe::inv_effect(owner,name,self,args);
  if(name=="getPos")return Value::make_obj(tree_new("java.lang.Vector3"));
  if(name=="getOri")return Value::make_obj(tree_new("java.lang.Ypr"));
  if(name=="currentLine")return Value::make_int(0);
  return Value::make_void(); // Presentation, controller and scene cleanup are isolated.
 }
 Value tree_call(const std::string& owner,const std::string& name,const std::string&,Object* self,const std::vector<Value>& args,bool)override{return inv_effect(owner,name,self,args);}
 Value tree_call_by_name(const std::string& owner,const std::string& name,const std::vector<Value>& args,bool is_static)override{
  auto values=args;Object* self=nullptr;if(!is_static&&!values.empty()&&values[0].tag==ValueTag::Obj){self=values[0].v.o;values.erase(values.begin());}
  return inv_effect(owner,name,self,values);
 }
};
void shipped_garage_save_order() {
 GarageProbe stock;
 const auto stock_command=stock.tree_get_static("java.game.Garage","CMD_MAINMENU");
 stock.run("java.game.Garage","osdCommand",&stock.old,{stock_command});
 // The local corpus may already have the distributed patch installed.
 SLRR_REQUIRE(stock.moves==3 && (stock.saved==0 || stock.saved==3));
 std::printf("Local corpus snapshot=%d (0 reproduces stock ordering; 3 is already patched)\n",stock.saved);
 GarageProbe host("patch/sl/Scripts/game/Garage.class");
 const auto command=host.tree_get_static("java.game.Garage","CMD_MAINMENU");
 SLRR_REQUIRE(command.v.i==101);
 host.run("java.game.Garage","osdCommand",&host.old,{command});
 std::printf("garage snapshot=%d moves=%d exit=%d enter=%d\n",host.saved,host.moves,host.exit_calls,host.enter_calls);
 SLRR_REQUIRE(host.saved==3 && host.moves==3 && host.exit_calls==1 && host.enter_calls==1);
 std::puts("PASS actual Garage.osdCommand -> GameLogic change -> Garage.exit -> Mechanic flush -> autosave ordering");
}
// Garage commands that are refused (dealer closed, no car, undriveable car)
// stay in the garage, so the mechanic strip is not flushed: they must not
// autosave. Accepted commands change section first, then save.
std::string garage_command(const std::string& path,const char* command,bool car,bool driveable,float hour){
 GarageProbe probe(path);
 probe.record_only=true;probe.hour=hour;
 Object vehicle,problem;vehicle.class_name="java.game.Vehicle";problem.class_name="java.lang.String";problem.str="broken";
 probe.player.fields["car"]=Value::make_obj(car?&vehicle:nullptr);
 probe.problem=Value::make_obj(driveable?nullptr:&problem);
 probe.run("java.game.Garage","osdCommand",&probe.old,{probe.tree_get_static("java.game.Garage",command)});
 std::string out;for(const auto& event:probe.events)out+=(out.empty()?"":",")+event;
 return out;
}
void garage_rejected_commands_do_not_save() {
 struct Case{const char* command;bool car,driveable;float hour;const char* expected;};
 const Case cases[]={
  {"CMD_TRACKSELECT",false,true,12,"warn"},{"CMD_TRACKSELECT",true,false,12,"warn"},
  {"CMD_TRACKSELECT",true,true,12,"change,save"},
  {"CMD_BUYCARS",true,true,20,"warn"},{"CMD_BUYCARSUSED",true,true,3,"warn"},
  {"CMD_BUYCARS",true,true,12,"change,save"},{"CMD_BUYCARSUSED",true,true,12,"change,save"},
  {"CMD_CARLOT",true,true,12,"change,save"},
  {"CMD_TESTTRACK",false,true,12,"warn"},{"CMD_TESTTRACK",true,false,12,"warn"}};
 // The #59 class is the behavioural reference when a local copy exists.
 const bool baseline=fs::exists("build/garage-59.class");
 int failures=0;
 for(const auto& c:cases){
  const auto got=garage_command("patch/sl/Scripts/game/Garage.class",c.command,c.car,c.driveable,c.hour);
  std::printf("%s car=%d driveable=%d hour=%.0f patch=%s",c.command,c.car,c.driveable,c.hour,got.c_str());
  if(baseline)std::printf(" #59=%s",garage_command("build/garage-59.class",c.command,c.car,c.driveable,c.hour).c_str());
  std::puts("");
  failures+=got!=c.expected;
 }
 SLRR_REQUIRE(failures==0);
 std::puts("PASS refused Garage commands do not autosave; accepted ones save after the transition");
 // Every other command must save exactly when the #59 class does (where a
 // local copy exists), apart from the order fixed above.
 if(baseline){
  GarageProbe names("patch/sl/Scripts/game/Garage.class");
  int compared=0,differ=0;
  for(const auto& cls:names.definitions)if(cls.name()=="java.game.Garage")
   for(const auto& member:cls.static_fields){
    const auto name=cls.utf8(member.name);
    if(name.rfind("CMD_",0)!=0)continue;
    std::string now,old;
    try{now=garage_command("patch/sl/Scripts/game/Garage.class",name.c_str(),true,true,12);
        old=garage_command("build/garage-59.class",name.c_str(),true,true,12);}
    catch(const std::exception&){continue;}
    ++compared;
    const bool a=now.find("save")!=std::string::npos,b=old.find("save")!=std::string::npos;
    if(a!=b){++differ;std::printf("save differs %s patch=%s #59=%s\n",name.c_str(),now.c_str(),old.c_str());}
   }
  std::printf("compared %d commands with the #59 class, %d differ in saving\n",compared,differ);
  SLRR_REQUIRE(compared>10 && differ==0);
 }
}
int main(){const standalone::Test tests[]={transition,shipped_transition,shipped_inventory_flush,shipped_garage_save_order,garage_rejected_commands_do_not_save};return standalone::run("inventory_transition",tests);}
