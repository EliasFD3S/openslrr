#include "slrr/natives_boot.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/object.hpp"

namespace slrr {
void install_osd_script_hover(Jvm& jvm) {
  osd_set_hover_handler([&jvm, last_osd = static_cast<Object*>(nullptr)](const OsdWidget* w) mutable {
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
