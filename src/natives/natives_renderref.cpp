#include "slrr/natives_boot.hpp"
#include "natives_internal.hpp"

#include "slrr/garage_camera.hpp"
#include "slrr/gameref.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/script_cfg.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/video_fmv.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace slrr {

void register_renderref_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  // ---- java.util.resource.RenderRef (declaring class) ----
  // Java: create(parent,rid,alias) → create(parent, new RenderRef(rid), alias).
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "create",
      "(Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;"
      "Ljava.lang.String;)V",
      [&rpk](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        Object* parent =
            (args.size() >= 1 && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                               : nullptr;
        Object* type =
            (args.size() >= 2 && args[1].tag == ValueTag::Obj) ? args[1].v.o
                                                               : nullptr;
        Object* alias =
            (args.size() >= 3 && args[2].tag == ValueTag::Obj) ? args[2].v.o
                                                               : nullptr;
        // Soft PE RenderRef.create @ 0x480EE0 — INSTANCE_RENDER.
        GameRefState* st = gameref_upsert(self, GameRefKind::RenderRef);
        if (!st) return Value::make_void();
        st->script = self;
        st->class_name = self->class_name;
        st->empty = false;
        if (parent) gameref_set_parent(self, parent);
        if (type) {
          int32_t tid = 0;
          if (GameRefState* ts = gameref_get(type)) tid = ts->type_id;
          if (tid == 0) {
            auto it = type->fields.find("id");
            if (it != type->fields.end() && it->second.tag == ValueTag::Int)
              tid = it->second.v.i;
          }
          st->type_id = tid;
          self->fields["id"] = Value::make_int(tid);
        }
        if (alias && !alias->str.empty()) st->alias = alias->str;
        if (st->type_id)
          apply_script_cfg(rpk, st, static_cast<uint32_t>(st->type_id));
        auto copy_num = [](Object* from, Object* to, const char* key) {
          if (!from || !to) return;
          auto it = from->fields.find(key);
          if (it != from->fields.end()) to->fields[key] = it->second;
        };
        copy_num(type, self, "_mw");
        copy_num(type, self, "_mh");
        copy_num(type, self, "_tid");
        if (st->alias == "Rectangle")
          self->fields["_rect"] = Value::make_int(1);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setMatrix",
      "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V",
      [&rpk](Object* self, const std::vector<Value>& args) {
        float px = 0, py = 0, pz = 0, yaw = 0, pitch = 0, roll = 0;
        auto getf = [](Object* o, const char* n, float* out) {
          if (!o) return;
          auto it = o->fields.find(n);
          if (it != o->fields.end() && it->second.tag == ValueTag::Float)
            *out = it->second.v.f;
        };
        if (!args.empty() && args[0].tag == ValueTag::Obj)
          getf(args[0].v.o, "x", &px), getf(args[0].v.o, "y", &py),
              getf(args[0].v.o, "z", &pz);
        if (args.size() >= 2 && args[1].tag == ValueTag::Obj)
          getf(args[1].v.o, "y", &yaw), getf(args[1].v.o, "p", &pitch),
              getf(args[1].v.o, "r", &roll);
        gameref_set_matrix(self, px, py, pz, yaw, pitch, roll);
        // Text.renderinst.setMatrix moves the OSD line (credits scroller).
        Object* text = nullptr;
        if (self) {
          auto tit = self->fields.find("_host_text");
          if (tit != self->fields.end() && tit->second.tag == ValueTag::Obj)
            text = tit->second.v.o;
        }
        if (text) {
          float nx = px, ny = py;
          void* osd = nullptr;
          // createText stored the mapped position; remap script y the same way.
          auto oit = text->fields.find("osd");
          if (oit != text->fields.end() && oit->second.tag == ValueTag::Obj)
            osd = oit->second.v.o;
          if (osd)
            osd_map(osd_view(static_cast<Object*>(osd)), px, py, &nx, &ny);
          slrr_text_set_pos(text, nx, ny);
        }
        const bool rect =
            self && self->fields.count("_rect") &&
            self->fields["_rect"].tag == ValueTag::Int &&
            self->fields["_rect"].v.i != 0;
        if (rect) {
          auto num = [](Object* o, const char* k) -> float {
            if (!o) return 0.f;
            auto it = o->fields.find(k);
            if (it == o->fields.end()) return 0.f;
            if (it->second.tag == ValueTag::Float) return it->second.v.f;
            if (it->second.tag == ValueTag::Int)
              return static_cast<float>(it->second.v.i);
            return 0.f;
          };
          Object* group = nullptr;
          if (GameRefState* st = gameref_get(self))
            group = static_cast<Object*>(st->parent);
          Object* osd = nullptr;
          if (group) {
            auto oit = group->fields.find("osd");
            if (oit != group->fields.end() && oit->second.tag == ValueTag::Obj)
              osd = oit->second.v.o;
          }
          const float mw = num(self, "_mw");
          const float mh = num(self, "_mh");
          uint32_t tid = 0;
          auto tit = self->fields.find("_tid");
          if (tit != self->fields.end() && tit->second.tag == ValueTag::Int)
            tid = static_cast<uint32_t>(tit->second.v.i);
          const uint32_t local = tid & 0xFFFFu;
          float nx = px, ny = py;
          if (osd) osd_world_to_ndc(osd, px, py, &nx, &ny);
          auto already = self->fields.find("_osd_wi");
          if (already != self->fields.end() &&
              already->second.tag == ValueTag::Int) {
            osd_set_widget_pos(already->second.v.i, nx, ny);
            osd_attach_button_picture(self);
          } else if (tid != 0 && local != 0x0018u && mw > 0.f && mh > 0.f) {
            bool plate = false;
            float hw = 0.f, hh = 0.f;
            osd_mesh_halves(osd, mw, mh, &hw, &hh, &plate);
            OsdWidget w;
            w.owner = osd;
            w.x = nx;
            w.y = ny;
            w.half_w = hw;
            w.half_h = hh;
            // Slider knob (RID_SLD_KNOB) is created before the track. Marking
            // it a plate draws it underneath the bar.
            w.background = local != 0x0091u && local != 0x012bu && local != 0x013cu;
            w.no_hit = true;
            w.icon_id = tid;
            w.tex_alpha = local == 0x00A2u;
            w.icon_path = rpk.texture_path_for_id(tid);
            if (group) {
              auto git = group->fields.find("_host_group");
              if (git != group->fields.end() && git->second.tag == ValueTag::Int)
                w.group = git->second.v.i;
            }
            const int32_t wi = static_cast<int32_t>(osd_widgets().size());
            self->fields["_osd_wi"] = Value::make_int(wi);
            osd_add_widget(std::move(w));
            osd_attach_button_picture(self);
          }
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setType",
      "(Ljava.util.resource.ResourceRef;)V",
      [](Object* self, const std::vector<Value>& args) {
        Object* type =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        GameRefState* st = gameref_upsert(self, GameRefKind::RenderRef);
        if (!st || !type) return Value::make_void();
        int32_t tid = 0;
        if (GameRefState* ts = gameref_get(type)) tid = ts->type_id;
        if (tid == 0) {
          auto it = type->fields.find("id");
          if (it != type->fields.end() && it->second.tag == ValueTag::Int)
            tid = it->second.v.i;
        }
        st->type_id = tid;
        self->fields["id"] = Value::make_int(tid);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "getTypeID", "()I",
      [](Object* self, const std::vector<Value>&) {
        GameRefState* st = gameref_get(self);
        return Value::make_int(st ? st->type_id : 0);
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "getPos", "()Ljava.lang.Vector3;",
      [&jvm](Object* self, const std::vector<Value>&) {
        Object* v = jvm.new_object("java.lang.Vector3");
        GameRefState* st = gameref_get(self);
        if (v && st) {
          v->fields["x"] = Value::make_float(st->pos[0]);
          v->fields["y"] = Value::make_float(st->pos[1]);
          v->fields["z"] = Value::make_float(st->pos[2]);
        }
        return Value::make_obj(v);
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "getBoneId", "(Ljava.lang.String;)I",
      [](Object*, const std::vector<Value>&) {
        return Value::make_int(0);
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "changeResource",
      "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        Object* neu =
            (args.size() >= 2 && args[1].tag == ValueTag::Obj) ? args[1].v.o
                                                               : nullptr;
        if (!neu) return Value::make_void();
        auto num = [](Object* o, const char* k) -> float {
          auto it = o->fields.find(k);
          if (it == o->fields.end()) return 0.f;
          if (it->second.tag == ValueTag::Float) return it->second.v.f;
          if (it->second.tag == ValueTag::Int)
            return static_cast<float>(it->second.v.i);
          return 0.f;
        };
        const float mw = num(neu, "_mw");
        const float mh = num(neu, "_mh");
        if (mw > 0.f || mh > 0.f) {
          self->fields["_mw"] = Value::make_float(mw);
          self->fields["_mh"] = Value::make_float(mh);
          return Value::make_void();
        }
        int32_t id = 0;
        if (GameRefState* st = gameref_get(neu)) id = st->type_id;
        if (id == 0) {
          auto it = neu->fields.find("id");
          if (it != neu->fields.end() && it->second.tag == ValueTag::Int)
            id = it->second.v.i;
        }
        auto kept = neu->fields.find("_tid");
        if (id == 0 && kept != neu->fields.end() &&
            kept->second.tag == ValueTag::Int)
          id = kept->second.v.i;
        if (id != 0) self->fields["_tid"] = Value::make_int(id);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setColor", "(I)V",
      [](Object* self,const std::vector<Value>& args){
        if(auto* state=gameref_get(self);state&&!args.empty()&&args[0].tag==ValueTag::Int)state->color=static_cast<uint32_t>(args[0].v.i)&0xffffff;
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setLight", "(III)V", native_noop);
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setFlare",
      "(Ljava.util.resource.ResourceRef;IFFII)V", native_noop);
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "setMatrix",
      "(ILjava.util.resource.ResourceRef;Ljava.lang.Vector3;Ljava.lang.Ypr;)V",
      native_noop);
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "lineCreate",
      "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)I",
      [](Object*, const std::vector<Value>&) {
        return Value::make_int(0);
      });
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "lineAdd",
      "(Ljava.lang.Vector3;Ljava.lang.Vector3;IF)V", native_noop);
  jvm.natives().register_method(
      "java.util.resource.RenderRef", "plotRoute",
      "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;IF"
      "Ljava.lang.Vector3;)I",
      [](Object*, const std::vector<Value>&) {
        return Value::make_int(0);
      });

  jvm.natives().register_method(
      "java.util.resource.GroundRef", "setFog", "(IFF)V",
      [](Object*, const std::vector<Value>& args) {
        const int32_t col =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        const float near_z =
            (args.size() >= 2 && args[1].tag == ValueTag::Float) ? args[1].v.f
                                                                 : 0.f;
        const float far_z =
            (args.size() >= 3 && args[2].tag == ValueTag::Float) ? args[2].v.f
                                                                 : 0.f;
        gameref_set_fog(col, near_z, far_z);
        return Value::make_void();
      });
}

}  // namespace slrr
