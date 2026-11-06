#include "slrr/natives_boot.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/object.hpp"
#include "slrr/script_threads.hpp"
#include <memory>
#include <algorithm>
#include <cmath>

namespace slrr {
void install_osd_script_hover(Jvm& jvm) {
  osd_set_slider_handler([&jvm](const OsdWidget& w, float x, float) {
    auto object = [](Object* o, const char* key) -> Object* {
      if (!o) return nullptr;
      auto i=o->fields.find(key);
      return i!=o->fields.end() && i->second.tag==ValueTag::Obj ? i->second.v.o : nullptr;
    };
    auto number = [](Object* o, const char* key, float fallback) {
      if (!o) return fallback;
      auto i=o->fields.find(key);
      if(i==o->fields.end())return fallback;
      return i->second.tag==ValueTag::Float ? i->second.v.f :
             i->second.tag==ValueTag::Int ? float(i->second.v.i) : fallback;
    };
    Object* slider=w.button;
    for(int depth=0; slider && depth<4; ++depth) {
      if(jvm.tree_instance_of(slider,"java.render.osd.Slider"))break;
      slider=object(slider,"parent");
    }
    if(!slider || !jvm.tree_instance_of(slider,"java.render.osd.Slider"))return false;
    Object* osd=object(slider,"osd");
    const float width=number(osd,"vpWidth",1.f);
    const float x0=number(slider,"x0",0.f), x1=number(slider,"x1",0.f);
    if(width<=0.f || x0==x1)return true;
    const float local=(x-(2.f*number(osd,"vpLeft",0.f)+width-1.f))/width;
    const float t=std::clamp((local-x0)/(x1-x0),0.f,1.f);
    const float a=number(slider,"a",0.f), b=number(slider,"b",1.f);
    const float before=number(slider,"value",0.f);
    jvm.invoke_by_name(slider->class_name,"setValue",
                      {Value::make_obj(slider),Value::make_float(a+(b-a)*t)},false);
    const float value=number(slider,"value",before);
    if(!std::isfinite(value) || value==before)return true;
    if(osd)osd->fields["sliderValue"]=Value::make_float(value);
    // Gadget.sendMessage uses Events for async GameType handlers (tuned
    // parts), and osdCommand for synchronous dialogs such as OptionsDialog.
    // Live Java threads consume their message port; retain synchronous
    // delivery for handlers that have a port but no running script thread.
    Object* handler=object(slider,"handler");
    const int cmd=static_cast<int>(number(slider,"cmd",static_cast<float>(w.cmd)));
    if(handler && jvm.tree_instance_of(handler,"java.lang.GameType") && object(handler,"mp")) {
      Object* event=jvm.new_object("java.util.Event");
      jvm.invoke_by_name(event->class_name,"<init>",
         {Value::make_obj(event),Value::make_int(cmd),Value::make_int(4)},false);
      event->fields["gadget"]=Value::make_obj(slider);
      if(script_thread_alive(jvm,object(handler,"t"))) {
        Object* port=object(handler,"mp");
        jvm.invoke_by_name(port->class_name,"putMessage",
           {Value::make_obj(port),Value::make_obj(event)},false);
      } else {
        jvm.invoke(handler->class_name,"handleMessage","(Ljava.util.Event;)V",
           {Value::make_obj(handler),Value::make_obj(event)},false);
      }
    } else if(handler) {
      jvm.invoke_by_name(handler->class_name,"osdCommand",
         {Value::make_obj(handler),Value::make_int(cmd)},false);
    }
    return true;
  });
  auto last = std::make_shared<Object*>(nullptr);
  jvm.add_gc_roots([last](const GcRootVisitor& visit) { visit(*last); });
  osd_set_hover_handler([&jvm, last](const OsdWidget* w) {
    auto object=[](Object* o,const char* key)->Object* {
      if(!o)return nullptr;auto i=o->fields.find(key);
      return i!=o->fields.end()&&i->second.tag==ValueTag::Obj?i->second.v.o:nullptr;
    };
    auto clear=[&](Object* osd){
      if(!osd)return;
      jvm.invoke("java.render.Osd","deactivateText","()V",{Value::make_obj(osd)},false);
      // Also clean an orphaned tooltip left by an earlier manual unfocus.
      if(Object* text=object(osd,"tooltipText"))
        jvm.invoke("java.render.Text","changeText","(Ljava.lang.String;)V",{Value::make_obj(text),Value::make_obj(nullptr)},false);
      if(Object* rect=object(osd,"tooltipRect"))
        jvm.invoke_by_name(rect->class_name,"finalize",{Value::make_obj(rect)},false);
      osd->fields["tooltipText"]=Value::make_obj(nullptr);
      osd->fields["tooltipRect"]=Value::make_obj(nullptr);
      osd->fields["pointedGadget"]=Value::make_obj(nullptr);
    };
    Object* g=w?w->button:nullptr;
    if(Object* parent=object(g,"parent"))
      if(jvm.tree_instance_of(parent,"java.render.osd.Slider"))g=parent;
    Object* osd=object(g,"osd");
    if(*last && *last!=osd)clear(*last);
    if(!osd){*last=nullptr;return;}
    *last=osd;
    osd->fields["controlMode"]=Value::make_int(0);
    if(object(osd,"pointedGadget")==g)return;
    clear(osd);
    osd->fields["pointedGadget"]=Value::make_obj(g);
    osd->fields["lastPointedGadget"]=Value::make_obj(g);
    osd->fields["lastUserActivityTimeStamp"]=jvm.invoke("java.lang.System","currentTime","()F",{},true);
    if(g)jvm.invoke(g->class_name,"focus","()V",{Value::make_obj(g)},false);
  });
}
}  // namespace slrr
