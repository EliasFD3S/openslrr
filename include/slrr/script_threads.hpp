#pragma once
#include "slrr/gc_roots.hpp"
#include <cstddef>
#include <string>
namespace slrr {
class Jvm; struct Object;
void script_thread_init(Jvm&,Object*,const std::string&);
void script_thread_start(Jvm&,Object*);
void script_thread_stop(Jvm&,Object*);
bool script_thread_alive(Jvm&,Object*);
int script_thread_priority(Object*);
void script_thread_set_priority(Object*,int);
void script_thread_set_daemon(Object*,bool);
void script_thread_suspend(Jvm&,Object*,bool);
void script_thread_sleep(Jvm&,float milliseconds);
bool script_thread_checkpoint(Jvm&);
bool script_thread_active(Jvm&);
std::size_t script_threads_live_count();
void script_threads_tick(Jvm&);
void script_threads_shutdown(Jvm&);
void script_threads_roots(Jvm&,const GcRootVisitor&);
}
