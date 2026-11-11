#include "slrr/script_threads.hpp"
#include "slrr/diagnostics.hpp"
#include "slrr/jvm.hpp"
#include "slrr/natives_boot.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace slrr {
namespace {
using Clock=std::chrono::steady_clock;
double now(){return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();}
struct Cancelled {};
struct Scheduler;
struct Thread {
  Scheduler* scheduler=nullptr; Object* object=nullptr;
  void* fiber=nullptr; bool alive=false, suspended=false, stopping=false;
  double wake=0; Jvm::ExecutionState execution;
  std::uint64_t id=0; Clock::time_point started_at;
};
struct Scheduler {
  Jvm* vm=nullptr;void* host=nullptr;bool converted=false, ticking=false;
  Thread* current=nullptr;Clock::time_point deadline;
  size_t next=0;
  std::uint64_t next_fiber_id=0;
  Jvm::ExecutionState host_execution;
  std::unordered_map<Object*,std::unique_ptr<Thread>> threads;
};
std::unordered_map<Jvm*,std::unique_ptr<Scheduler>> schedulers;
Scheduler& state(Jvm& vm){auto& p=schedulers[&vm];if(!p){p=std::make_unique<Scheduler>();p->vm=&vm;}return *p;}
Thread* find(Jvm& vm,Object* o){auto s=schedulers.find(&vm);if(s==schedulers.end())return nullptr;auto i=s->second->threads.find(o);return i==s->second->threads.end()?nullptr:i->second.get();}
#ifdef _WIN32
void yield(Scheduler& s){
  auto& t=*s.current;
  s.vm->exchange_execution_state(t.execution);
  s.vm->exchange_execution_state(s.host_execution);
  SwitchToFiber(s.host);
  if(t.stopping)throw Cancelled{};
}
void WINAPI entry(void* p){
  auto& t=*static_cast<Thread*>(p);auto& s=*t.scheduler;
  std::string result="ok",error;
  try {
    if(t.stopping)throw Cancelled{};
    Object* target=t.object;
    auto i=target->fields.find("target");
    if(i!=target->fields.end()&&i->second.tag==ValueTag::Obj&&i->second.v.o)target=i->second.v.o;
    s.vm->invoke_by_name(target->class_name,"run",{Value::make_obj(target)},false);
  } catch(const Cancelled&) {
    result="cancelled";
  } catch(const std::exception& e){
    result="error";error=e.what();std::fprintf(stderr,"[thread] %s\n",e.what());
  }
  t.alive=false;t.object->fields["_thread_alive"]=Value::make_int(0);
  if(diagnostics::enabled()){
    diagnostics::Event event;event.category=diagnostics::Category::Fiber;
    event.name="fiber.exit";event.object=t.object;event.result=std::move(result);
    event.error=std::move(error);event.duration_ns=static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-t.started_at).count());
    diagnostics::trace(std::move(event));
  }
  yield(s);
  std::terminate();
}
void resume(Scheduler& s,Thread& t){
  // MAX_PRIORITY is used by script loaders. Give CPU-bound loading a
  // bounded frame-sized burst instead of spreading it over dozens of frames.
  const int priority=std::max(0,script_thread_priority(t.object));
  const int slice=1+(priority*15)/10;
  s.current=&t;s.deadline=Clock::now()+std::chrono::milliseconds(slice);
  s.vm->exchange_execution_state(s.host_execution);
  s.vm->exchange_execution_state(t.execution);
  const auto previous_fiber=diagnostics::current_fiber_id();
  diagnostics::set_fiber(t.id);
  SwitchToFiber(t.fiber);
  if(previous_fiber)diagnostics::set_fiber(*previous_fiber);else diagnostics::clear_fiber();
  s.current=nullptr;
  if(!t.alive && t.fiber){DeleteFiber(t.fiber);t.fiber=nullptr;}
}
#endif
}
void script_thread_init(Jvm& vm,Object* object,const std::string& name){
  if(!object)return;
  if(auto* thread=find(vm,object);thread&&thread->alive)return;
  object->fields["_thread_name"]=Value::make_obj(vm.new_string(name));
  object->fields["_thread_alive"]=Value::make_int(0);
  object->fields["_thread_daemon"]=Value::make_int(0);
}
void script_thread_start(Jvm& vm,Object* object){
  if(!object)return;
  // A stopped fiber can still own VM frames until it resumes to unwind.
  if(auto* thread=find(vm,object);thread&&thread->alive)return;
  if(!object->fields.contains("_thread_name"))
    script_thread_init(vm,object,object->class_name);
  auto& s=state(vm);
  auto& entry_state=s.threads[object];
  if(!entry_state)entry_state=std::make_unique<Thread>();
  auto& t=*entry_state;
  t=Thread{};
  t.scheduler=&s;
  t.object=object;
  t.id=++s.next_fiber_id;
  t.started_at=Clock::now();
#ifdef _WIN32
  t.fiber=CreateFiberEx(64*1024,1024*1024,FIBER_FLAG_FLOAT_SWITCH,entry,&t);
  t.alive=t.fiber!=nullptr;
#else
  std::fprintf(stderr,"[thread] cooperative stacks require Windows\n");
#endif
  object->fields["_thread_alive"]=Value::make_int(t.alive?1:0);
  if(diagnostics::enabled()){
    diagnostics::Event event;event.category=diagnostics::Category::Fiber;
    event.name="fiber.start";event.object=object;event.fiber_id=t.id;
    event.result=t.alive?"ok":"error";
    if(!t.alive)event.error="CreateFiberEx failed";
    diagnostics::trace(std::move(event));
  }
}
bool script_thread_alive(Jvm& vm,Object* o){auto* t=find(vm,o);return t&&t->alive&&!t->stopping;}
int script_thread_priority(Object* o){
  if(!o)return 0;auto i=o->fields.find("_thread_priority");
  return i!=o->fields.end()&&i->second.tag==ValueTag::Int?std::clamp(i->second.v.i,-10,10):0;
}
void script_thread_set_priority(Object* o,int priority){if(o)o->fields["_thread_priority"]=Value::make_int(std::clamp(priority,-10,10));}
void script_thread_set_daemon(Object* object,bool daemon){
  if(object&&object->fields.contains("_thread_name"))
    object->fields["_thread_daemon"]=Value::make_int(daemon?1:0);
}
void script_thread_stop(Jvm& vm,Object* o){
  auto* t=find(vm,o);if(!t||!t->alive)return;
  t->stopping=true;t->suspended=false;t->wake=0;
  o->fields["_thread_alive"]=Value::make_int(0);
  if(diagnostics::enabled()){
    diagnostics::Event event;event.category=diagnostics::Category::Fiber;
    event.name="fiber.stop";event.object=o;event.fiber_id=t->id;
    event.result="stopping";diagnostics::trace(std::move(event));
  }
  if(t->scheduler->current==t)throw Cancelled{};
}
void script_thread_suspend(Jvm& vm,Object* o,bool suspended){
  auto* t=find(vm,o);if(!t||!t->alive||t->stopping)return;t->suspended=suspended;
#ifdef _WIN32
  if(suspended&&t->scheduler->current==t)yield(*t->scheduler);
#endif
}
bool script_thread_active(Jvm& vm){auto i=schedulers.find(&vm);return i!=schedulers.end()&&i->second->current;}
void script_thread_sleep(Jvm& vm,float ms){
  if(!std::isfinite(ms)||ms<0)ms=0;
  auto& s=state(vm);
#ifdef _WIN32
  if(s.current){s.current->wake=now()+double(ms)*.001;yield(s);return;}
#endif
  host_script_sleep(ms);
}
bool script_thread_checkpoint(Jvm& vm){
  auto i=schedulers.find(&vm);if(i==schedulers.end()||!i->second->current)return false;
  auto& s=*i->second;
#ifdef _WIN32
  if(s.current->stopping)throw Cancelled{};
  if(Clock::now()>=s.deadline){
    if(diagnostics::enabled()){
      diagnostics::Event event;event.category=diagnostics::Category::Fiber;
      event.name="fiber.checkpoint";event.object=s.current->object;
      event.result="timeslice";diagnostics::trace(std::move(event));
    }
    yield(s);return true;
  }
#endif
  return false;
}
void script_threads_tick(Jvm& vm){
  auto& s=state(vm);if(s.ticking||s.current)return;
#ifdef _WIN32
  if(!s.host){s.converted=!IsThreadAFiber();s.host=s.converted?ConvertThreadToFiberEx(nullptr,FIBER_FLAG_FLOAT_SWITCH):GetCurrentFiber();if(!s.host)return;}
  s.ticking=true;
  struct Guard{bool& b;~Guard(){b=false;}} guard{s.ticking};
  std::vector<Thread*> ready;
  for(auto& p:s.threads)if(p.second->alive&&!p.second->suspended&&p.second->wake<=now())ready.push_back(p.second.get());
  std::stable_sort(ready.begin(),ready.end(),[](const Thread* a,const Thread* b){return script_thread_priority(a->object)>script_thread_priority(b->object);});
  const bool urgent=!ready.empty()&&script_thread_priority(ready.front()->object)==10;
  const auto frame_deadline=Clock::now()+std::chrono::milliseconds(urgent?16:8);
  if(!ready.empty()) {
    const size_t start=s.next%ready.size();size_t ran=0;
    while(ran<ready.size()){
      auto* t=ready[(start+ran)%ready.size()];++ran;
      if(t->alive&&!t->suspended&&t->wake<=now())resume(s,*t);
      if(Clock::now()>=frame_deadline)break;
    }
    s.next=(start+ran)%ready.size();
  }
  for(auto i=s.threads.begin();i!=s.threads.end();) {
    if(!i->second->alive && !i->second->fiber)i=s.threads.erase(i);
    else ++i;
  }
#endif
}
void script_threads_roots(Jvm& vm,const GcRootVisitor& visit){
  auto i=schedulers.find(&vm);if(i==schedulers.end())return;
  for(auto& p:i->second->threads)if(p.second->alive)visit(p.second->object);
}
void script_threads_shutdown(Jvm& vm){
  auto i=schedulers.find(&vm);if(i==schedulers.end())return;auto& s=*i->second;
#ifdef _WIN32
  if(s.current)return;
  for(auto& p:s.threads){auto& t=*p.second;if(t.alive){t.stopping=true;t.suspended=false;if(s.host)resume(s,t);}if(t.fiber)DeleteFiber(t.fiber);}
  if(s.converted)ConvertFiberToThread();
#endif
  schedulers.erase(i);
}
}
