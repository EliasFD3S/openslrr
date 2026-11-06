#include "slrr/host_game_options.hpp"
#include "slrr/jvm.hpp"
#include <chrono>
#include <cstdio>

namespace slrr {
namespace {
bool loaded=false, dirty=false;
std::chrono::steady_clock::time_point changed;
}
void host_game_options_load(Jvm& jvm) {
  loaded=false;dirty=false;
  // Engine-owned Config in stock. A VM static keeps it alive through GC.
  Object* options=jvm.new_object("java.util.Config");
  jvm.set_static("java.lang.System","_hostGameOptions",Value::make_obj(options));
  jvm.invoke_by_name(options->class_name,"<init>",
                     {Value::make_obj(options),Value::make_int(0)},false);
  loaded=true;
}
void host_game_options_dirty() {
  if(!loaded)return;
  dirty=true;changed=std::chrono::steady_clock::now();
}
void host_game_options_update(Jvm& jvm, bool force) {
  if(!loaded || !dirty || (!force && std::chrono::steady_clock::now()-changed < std::chrono::milliseconds(500)))return;
  Value v=jvm.get_static("java.lang.System","_hostGameOptions");
  if(v.tag!=ValueTag::Obj || !v.v.o)return;
  Value result=jvm.invoke_by_name(v.v.o->class_name,"saveConfig",
       {v,Value::make_obj(jvm.new_string("save/game/options"))},false);
  if(result.tag==ValueTag::Int && result.v.i){dirty=false;std::printf("[options] saved save/game/options\n");}
  else {changed=std::chrono::steady_clock::now();std::fprintf(stderr,"[options] save failed\n");}
}
}
