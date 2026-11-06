#pragma once
#include "slrr/jvm.hpp"
namespace slrr {
void script_notification_add(Jvm&,Object* listener,Object* source,int type,int alias,const std::string& message,const std::string& method);
void script_notification_remove(Jvm&,Object* listener,Object* source,int type);
void script_callback_set(Jvm&,Object*,int mode,bool enabled);
void script_callbacks_remove(Jvm&,Object*);
bool script_callback_registered(Jvm&,Object*,int mode);
void script_callbacks_tick(Jvm&,float dt);
int script_event_emit(Jvm&,Object* source,Object* related,int event,Value param,bool related_event=false);
void script_events_roots(Jvm&,const GcRootVisitor&);
void script_events_destroy(Object*);
void script_events_shutdown(Jvm&);
}
