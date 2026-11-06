#include "slrr/script_events.hpp"
#include "slrr/gameref.hpp"
#include <algorithm>
#include <unordered_map>
#include <vector>
namespace slrr {
namespace {
struct Watch {uint64_t id;Object* listener;Object* source;uint32_t type,alias;std::string message,method;};
struct Hook {uint64_t id;Object* object;int mode;};
struct Events {uint64_t sequence=0;std::vector<Watch> watches;std::vector<Hook> hooks;bool ticking=false;int depth=0;};
std::unordered_map<Jvm*,Events> states;
Object* canonical(Object* o){auto* ref=gameref_get(o);return ref?static_cast<Object*>(ref->key):o;}
}
void script_notification_remove(Jvm& vm,Object* listener,Object* source,int type){
  if(!listener||!source)return;auto& w=states[&vm].watches;source=canonical(source);
  auto i=std::find_if(w.begin(),w.end(),[&](const Watch& n){return n.listener==listener&&n.source==source&&n.type==uint32_t(type);});
  if(i!=w.end())w.erase(i);
}
void script_notification_add(Jvm& vm,Object* listener,Object* source,int type,int alias,const std::string& message,const std::string& method){
  if(!listener||!source)return;script_notification_remove(vm,listener,source,type);auto& s=states[&vm];
  s.watches.insert(s.watches.begin(),{++s.sequence,listener,canonical(source),uint32_t(type),uint32_t(alias),message,method});
}
void script_callback_set(Jvm& vm,Object* object,int mode,bool enabled){
  if(!object||(mode!=8&&mode!=9&&mode!=28))return;auto& s=states[&vm];
  auto i=std::find_if(s.hooks.begin(),s.hooks.end(),[&](const Hook& h){return h.object==object&&h.mode==mode;});
  if(!enabled){if(i!=s.hooks.end())s.hooks.erase(i);}
  else if(i==s.hooks.end())s.hooks.push_back({++s.sequence,object,mode});
}
void script_callbacks_remove(Jvm& vm,Object* object){auto& hooks=states[&vm].hooks;hooks.erase(std::remove_if(hooks.begin(),hooks.end(),[&](const Hook& h){return h.object==object;}),hooks.end());}
bool script_callback_registered(Jvm& vm,Object* object,int mode){auto& h=states[&vm].hooks;return std::any_of(h.begin(),h.end(),[&](const Hook& n){return n.object==object&&n.mode==mode;});}
void script_callbacks_tick(Jvm& vm,float dt){
  auto& s=states[&vm];if(s.ticking||s.hooks.empty())return;s.ticking=true;
  struct Guard{bool& b;~Guard(){b=false;}} guard{s.ticking};
  const auto snapshot=s.hooks;
  Jvm::RootScope receivers(vm,[&](const GcRootVisitor& visit){for(const auto& h:snapshot)visit(h.object);});
  // Separate stock lists: CONTROL, DRIVE, ANIMATE; registration order within each.
  for(int mode:{8,9,28})for(const auto& h:snapshot) {
    if(h.mode!=mode||!std::any_of(s.hooks.begin(),s.hooks.end(),[&](const Hook& n){return n.id==h.id;}))continue;
    const char* method=mode==8?"control":mode==9?"drive":"animate";
    std::vector<Value> args{Value::make_obj(h.object)};
    if(mode==8)args.push_back(Value::make_float(dt));
    vm.invoke(h.object->class_name,method,mode==8?"(F)V":"()V",args,false);
  }
}
int script_event_emit(Jvm& vm,Object* source,Object* related,int event,Value param,bool related_event){
  if(!source)return 0;auto& s=states[&vm];if(s.depth>=32)return 0;
  ++s.depth;struct Guard{int& n;~Guard(){--n;}} guard{s.depth};
  Jvm::RootScope roots(vm,[&](const GcRootVisitor& visit){visit(source);visit(related);if(param.tag==ValueTag::Obj)visit(param.v.o);});
  source=canonical(source);const auto snapshot=s.watches;int delivered=0;
  Jvm::RootScope receivers(vm,[&](const GcRootVisitor& visit){for(const auto& w:snapshot){visit(w.listener);visit(w.source);}});
  for(const auto& w:snapshot){
    if(w.source!=source||!(w.type&uint32_t(event)))continue;
    auto i=std::find_if(s.watches.begin(),s.watches.end(),[&](const Watch& n){return n.id==w.id;});
    if(i==s.watches.end())continue;
    if(w.type&0x80000000u)s.watches.erase(i); // before reentrant script dispatch
    if((w.type&0x40000000u)&&related_event)continue;
    int forwarded=(w.alias&0x0fffffffu)?int(w.alias&0x0fffffffu):event;
    auto* target=gameref_get(w.listener);if(!target||!(target->event_mask&forwarded))continue;
    Value out=param;
    const bool integer=(forwarded&0x0fffffff)==0x80;
    if(w.alias&0x10000000u)out=integer?Value::make_int(0):Value::make_obj(w.message.empty()?nullptr:vm.new_string(w.message));
    if(integer&&out.tag!=ValueTag::Int)out=Value::make_int(0);
    if(!integer&&out.tag!=ValueTag::Obj)out=Value::make_obj(vm.new_string(std::to_string(out.tag==ValueTag::Int?out.v.i:0)));
    std::string method=w.method.empty()?"handleEvent":w.method;
    if(w.method.empty()){
      auto a=w.listener->fields.find("asyncMode");
      if(a!=w.listener->fields.end()&&a->second.tag==ValueTag::Int&&a->second.v.i)method="handleEventAsync";
    }
    vm.invoke(w.listener->class_name,method,integer?"(Ljava.util.resource.GameRef;II)V":"(Ljava.util.resource.GameRef;ILjava.lang.String;)V",
              {Value::make_obj(w.listener),Value::make_obj(related?related:source),Value::make_int(forwarded),out},false);
    ++delivered;
  }
  return delivered;
}
void script_events_roots(Jvm& vm,const GcRootVisitor& visit){auto i=states.find(&vm);if(i==states.end())return;for(auto&w:i->second.watches){visit(w.listener);visit(w.source);}for(auto&h:i->second.hooks)visit(h.object);}
void script_events_destroy(Object* object){for(auto& p:states){auto& s=p.second;s.hooks.erase(std::remove_if(s.hooks.begin(),s.hooks.end(),[&](const Hook& h){return h.object==object;}),s.hooks.end());s.watches.erase(std::remove_if(s.watches.begin(),s.watches.end(),[&](const Watch& w){return w.listener==object||w.source==object;}),s.watches.end());}}
void script_events_shutdown(Jvm& vm){states.erase(&vm);}
}
