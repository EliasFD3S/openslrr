#include "slrr/natives_boot.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/object.hpp"
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
    // Match Slider.sendMessage before the host's synchronous OSD dispatch.
    Object* handler=object(slider,"handler");
    if(handler)jvm.invoke_by_name(handler->class_name,"osdCommand",
       {Value::make_obj(handler),Value::make_int(static_cast<int>(number(slider,"cmd",static_cast<float>(w.cmd))))},false);
    return true;
  });
  auto last = std::make_shared<Object*>(nullptr);
  jvm.add_gc_roots([last](const GcRootVisitor& visit) { visit(*last); });
  osd_set_hover_handler([&jvm, last](const OsdWidget* w) {
    Object*& last_osd = *last;
    // Physics hit volumes carry the gadget. The 0x012B plate is not a button.
    Object* g = (w && w->button) ? w->button : nullptr;
    Object* osd = nullptr;
    if (g) {
      auto it = g->fields.find("osd");
      if (it != g->fields.end() && it->second.tag == ValueTag::Obj)
        osd = it->second.v.o;
    }
    if (last_osd && osd && last_osd != osd) {
      auto previous = last_osd->fields.find("pointedGadget");
      if (previous != last_osd->fields.end() && previous->second.tag == ValueTag::Obj &&
          previous->second.v.o) {
        Object* old = previous->second.v.o;
        jvm.invoke(old->class_name, "unfocus", "()V", {Value::make_obj(old)}, false);
      }
      last_osd->fields["pointedGadget"] = Value::make_obj(nullptr);
    }
    if (!osd) osd = last_osd;
    if (g && osd) last_osd = osd;
    if (!osd) return;
    Object* cur = nullptr;
    auto pit = osd->fields.find("pointedGadget");
    if (pit != osd->fields.end() && pit->second.tag == ValueTag::Obj)
      cur = pit->second.v.o;
    osd->fields["controlMode"] = Value::make_int(0);
    if (cur == g) return;
    if (cur && !cur->class_name.empty())
      jvm.invoke(cur->class_name, "unfocus", "()V", {Value::make_obj(cur)},
                 false);
    osd->fields["pointedGadget"] = Value::make_obj(g);
    osd->fields["controlMode"] = Value::make_int(0);
    if (g && !g->class_name.empty())
      jvm.invoke(g->class_name, "focus", "()V", {Value::make_obj(g)}, false);
  });
}
}  // namespace slrr
