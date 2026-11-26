#include "save_io_fixture.hpp"
#include "slrr/script_compiler.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/gameref.hpp"
#include "slrr/ui_time.hpp"
#include <cmath>
#include <limits>
using namespace slrr;
int main() {
  Fixture f;
  auto I=[](int32_t n){return Value::make_int(n);};
  auto F=[](float n){return Value::make_float(n);};
  Object object;
  const auto nan=F(std::numeric_limits<float>::quiet_NaN());
  SLRR_REQUIRE(native_arg_integer({},0,7)==7);
  SLRR_REQUIRE(native_arg_integer({Value::make_obj(&object)},0,7)==7);
  SLRR_REQUIRE(native_arg_integer({nan},0,7)==7);
  SLRR_REQUIRE(native_arg_integer({F(3.75f)},0)==3);
  SLRR_REQUIRE(native_arg_integer({F(1e30f)},0)==INT32_MAX);
  SLRR_REQUIRE(native_arg_object({I(1)},0)==nullptr);
  register_script_natives(f.vm,f.packs,f.sections);
  register_osd_natives(f.vm,f.packs,f.sections);
  register_lang_natives(f.vm,f.packs,f.sections);
  register_renderref_natives(f.vm,f.packs,f.sections);
  auto call=[&](const char* owner,const char* method,const char* sig,std::vector<Value> args,Object* self=nullptr){
    return f.registry.invoke(owner,method,sig,self,args);
  };
  SLRR_REQUIRE(call("java.game.OptionsDialog","getRendererOption","(I)I",{}).v.i==0);
  auto& graphics=host_enhanced_graphics();const int previous=graphics.msaa;
  call("java.game.OptionsDialog","setRendererOption","(II)V",{});
  SLRR_REQUIRE(graphics.msaa==previous);
  call("java.game.OptionsDialog","setRendererOption","(II)V",{F(0),F(2)});
  SLRR_REQUIRE(graphics.msaa==2);graphics.msaa=previous;
  for (auto args : {std::vector<Value>{},std::vector<Value>{I(1)},std::vector<Value>{Value::make_obj(&object),nan,I(32)}})
    call("java.render.GfxEngine","changeVideoMode","(III)V",args);
  call("java.game.OptionsDialog","setRendererOption","(II)V",{I(0),Value::make_obj(&object)});
  SLRR_REQUIRE(graphics.msaa==previous);
  Object text,font;
  slrr_text_create(&text,&font,0,0);slrr_text_set_string(&text,"test");
  slrr_text_update(&text);
  call("java.render.Text","setPos","(FF)V",{I(1),F(.5f)},&text);
  SLRR_REQUIRE(slrr_osd_texts().back().x==1.f && slrr_osd_texts().back().y==.5f);
  slrr_text_destroy(&text);
  Object render,position,orientation;
  gameref_upsert(&render,GameRefKind::RenderRef);
  position.fields["x"]=I(2);position.fields["y"]=F(3);position.fields["z"]=I(4);
  orientation.fields["y"]=I(1);
  call("java.util.resource.RenderRef","setMatrix","(Ljava.lang.Vector3;Ljava.lang.Ypr;)V",
       {Value::make_obj(&position),Value::make_obj(&orientation)},&render);
  const auto* state=gameref_get(&render);
  SLRR_REQUIRE(state && state->pos[0]==2 && state->pos[1]==3 && state->pos[2]==4);
  gameref_remove(&render);
  Object camera;
  call("java.render.Camera","create","(Ljava.util.resource.ResourceRef;Ljava.render.Viewport;IFFFFFII)V",{Value::make_obj(nullptr),Value::make_obj(nullptr),F(3)},&camera);
  SLRR_REQUIRE(gameref_get(&camera)->cam_pri==3);
  call("java.render.Camera","activate","(Ljava.render.Viewport;I)V",{Value::make_obj(nullptr),F(5)},&camera);
  SLRR_REQUIRE(gameref_get(&camera)->cam_pri==5);
  call("java.render.Camera","destroy","()V",{},&camera);
  auto time=call("java.lang.String","timeToString","(FI)Ljava.lang.String;",{I(79),F(2)});
  SLRR_REQUIRE(time.tag==ValueTag::Obj && time.v.o->str==format_game_time(79,2));
  time=call("java.lang.String","timeToString","(F)Ljava.lang.String;",{Value::make_obj(&object)});
  SLRR_REQUIRE(time.v.o->str==format_game_time(0,0));
  call("java.lang.System","exit","(Ljava.lang.String;)V",{I(1)});
  SLRR_REQUIRE(f.vm.get_static("java.lang.System","_hostQuit").v.i==1);
  // Math natives: radians, no NaN or infinity reaches the script.
  auto math=[&](const char* method,std::vector<Value> args){
    return call("java.lang.Math",method,std::string(method)=="atan2"?"(FF)F":"(F)F",args).v.f;
  };
  auto close_to=[](float a,float b){return std::fabs(a-b)<1e-5f;};
  SLRR_REQUIRE(close_to(math("sin",{F(1.5707964f)}),1.f) && close_to(math("cos",{I(0)}),1.f));
  SLRR_REQUIRE(close_to(math("tan",{F(0.7853982f)}),1.f) && close_to(math("atan",{I(1)}),0.7853982f));
  SLRR_REQUIRE(close_to(math("asin",{F(2.f)}),1.5707964f) && close_to(math("acos",{F(-5.f)}),3.1415927f));
  SLRR_REQUIRE(close_to(math("atan2",{I(1),I(-1)}),2.3561945f) && math("atan2",{})==0.f);
  SLRR_REQUIRE(close_to(math("exp",{I(1)}),2.7182817f) && math("exp",{F(1000.f)})==0.f);
  SLRR_REQUIRE(close_to(math("log",{F(2.7182817f)}),1.f) && math("log",{I(0)})==0.f && math("log",{F(-1.f)})==0.f);
  SLRR_REQUIRE(close_to(math("wrapAngle",{F(4.f)}),4.f-6.2831853f) && close_to(math("wrapAngle",{F(-7.f)}),-7.f+6.2831853f));
  SLRR_REQUIRE(std::isfinite(math("wrapAngle",{F(1e30f)})) && math("sin",{nan})==0.f);
  fs::create_directories(f.root/"probe/src");
  std::ofstream(f.root/"probe/src/Safety.java") << R"JAVA(package probe;
public class Safety {
 public static void store(int[] a,int i,int v){a[i]=v;}
 public static void compound(int[] a,int i,int v){a[i]+=v;}
}
)JAVA";
  auto report=script_compile_all(f.root.string(),"probe");
  SLRR_REQUIRE(report.failed==0 && report.succeeded>0);
  fs::path compiled;
  for(auto& entry:fs::recursive_directory_iterator(f.root))if(entry.path().extension()==".class")compiled=entry.path();
  SLRR_REQUIRE(!compiled.empty() && f.vm.load_class_file(compiled.string()));
  Object array;array.is_array=true;array.slots={I(1)};array.fields["length"]=I(1);
  auto store=[&](const char* method,int index,int value){f.vm.invoke("probe.Safety",method,"([III)V",{Value::make_obj(&array),I(index),I(value)},true);};
  store("store",3,9);SLRR_REQUIRE(array.slots.size()==4 && array.slots[3].v.i==9 && array.fields["length"].v.i==4);
  store("compound",3,2);SLRR_REQUIRE(array.slots[3].v.i==11);
  for(int index:{-1,1<<20,INT32_MAX}) {
    store("store",index,99);store("compound",index,99);
    SLRR_REQUIRE(array.slots.size()==4 && array.fields["length"].v.i==4 && array.slots[3].v.i==11);
  }
  // The patched Math class: its constants and script helpers, read from a
  // caller compiled against it.
  const fs::path math_class=fs::path("patch/system/Scripts/lang/Math.class");
  fs::create_directories(f.root/"system/Scripts/lang");
  fs::copy_file(math_class,f.root/"system/Scripts/lang/Math.class");
  SLRR_REQUIRE(f.vm.load_class_file((f.root/"system/Scripts/lang/Math.class").string()));
  fs::create_directories(f.root/"probe2/src");
  std::ofstream(f.root/"probe2/src/MathUse.java") << R"JAVA(package probe2;
public class MathUse {
 public static float pi(){return Math.PI;}
 public static float rad(float d){return d*Math.DEG2RAD;}
 public static float clamp(float v){return Math.clamp(v,-1.0,2.0);}
 public static float mix(float t){return Math.lerp(10.0,20.0,t);}
 public static float absmin(float a,float b){return Math.min(Math.abs(a),Math.abs(b));}
 public static float wave(float a){return Math.sin(a)+Math.max(Math.sign(a),0.0);}
 public static int pick(int n){return Math.randomInt(n);}
}
)JAVA";
  report=script_compile_all(f.root.string(),"probe2");
  SLRR_REQUIRE(report.failed==0 && report.succeeded>0);
  SLRR_REQUIRE(f.vm.load_class_file((f.root/"probe2/MathUse.class").string()));
  auto use=[&](const char* method,const char* sig,std::vector<Value> args){return f.vm.invoke("probe2.MathUse",method,sig,args,true);};
  auto as_float=[](Value v){return v.tag==ValueTag::Int?float(v.v.i):v.v.f;};
  SLRR_REQUIRE(close_to(as_float(use("pi","()F",{})),3.1415927f));
  SLRR_REQUIRE(close_to(as_float(use("rad","(F)F",{F(180.f)})),3.1415927f));
  SLRR_REQUIRE(as_float(use("clamp","(F)F",{F(5.f)}))==2.f && as_float(use("clamp","(F)F",{F(-5.f)}))==-1.f && as_float(use("clamp","(F)F",{F(.5f)}))==.5f);
  SLRR_REQUIRE(close_to(as_float(use("mix","(F)F",{F(.25f)})),12.5f));
  SLRR_REQUIRE(as_float(use("absmin","(FF)F",{F(-3.f),F(2.f)}))==2.f);
  SLRR_REQUIRE(close_to(as_float(use("wave","(F)F",{F(1.5707964f)})),2.f) && close_to(as_float(use("wave","(F)F",{F(-1.5707964f)})),-1.f));
  for(int i=0;i<50;++i){const int r=use("pick","(I)I",{I(3)}).v.i;SLRR_REQUIRE(r>=0 && r<3);}
  SLRR_REQUIRE(use("pick","(I)I",{I(0)}).v.i==0);
  store("store",(1<<20)-1,7);
  SLRR_REQUIRE(array.slots.size()==(1<<20) && array.slots.back().v.i==7);
  std::puts("PASS native short/mistagged/boxed arguments; bounded direct and compound array stores");
}
