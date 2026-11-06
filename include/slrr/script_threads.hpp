#pragma once
#include "slrr/gc_roots.hpp"
#include <string>
namespace slrr {
class Jvm; struct Object;
void script_thread_init(Jvm&,Object*,const std::string&);
void script_thread_start(Jvm&,Object*);
void script_thread_stop(Jvm&,Object*);
bool script_thread_alive(Jvm&,Object*);
void script_thread_suspend(Jvm&,Object*,bool);
void script_thread_sleep(Jvm&,float milliseconds);
bool script_thread_checkpoint(Jvm&);
bool script_thread_active(Jvm&);
void script_threads_tick(Jvm&);
void script_threads_shutdown(Jvm&);
void script_threads_roots(Jvm&,const GcRootVisitor&);
}
