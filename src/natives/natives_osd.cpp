#include "slrr/natives_boot.hpp"
#include "natives_internal.hpp"

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

void register_osd_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  jvm.natives().register_method("java.render.Text","getFontSize","(Ljava.util.resource.ResourceRef;)I",
    [](Object*,const std::vector<Value>& args){
      Object* font=!args.empty()&&args[0].tag==ValueTag::Obj?args[0].v.o:nullptr;
      const auto* loaded=slrr_font_get(font);
      return Value::make_int(loaded?loaded->px_height:10);
    });
  (void)rpk;
  (void)sections;
  // OSD factories are Java. These Viewport methods are real PE natives
  // (java.render.Viewport). Rect is the one create/resize write:
  // left, top, width, height, all normalized to the screen. getAspect is
  // the display aspect; the viewport's own width cancels.
  auto vp_alive = [](Object* self) {
    if (!self) return false;
    auto it = self->fields.find("vp_ready");
    return it != self->fields.end() && it->second.tag == ValueTag::Int &&
           it->second.v.i != 0;
  };
  auto vp_f = [](Object* self, const char* key) {
    auto it = self->fields.find(key);
    if (it == self->fields.end()) return 0.f;
    if (it->second.tag == ValueTag::Float) return it->second.v.f;
    if (it->second.tag == ValueTag::Int) return static_cast<float>(it->second.v.i);
    return 0.f;
  };
  auto as_f = [](const Value& v, float d) {
    if (v.tag == ValueTag::Float) return v.v.f;
    if (v.tag == ValueTag::Int) return static_cast<float>(v.v.i);
    return d;
  };
  auto vp_store = [](Object* self, int32_t pri, float x, float y, float w,
                     float h) {
    self->fields["pri"] = Value::make_int(pri);
    self->fields["vp_ready"] = Value::make_int(1);
    self->fields["vp_x"] = Value::make_float(x);
    self->fields["vp_y"] = Value::make_float(y);
    self->fields["vp_w"] = Value::make_float(w);
    self->fields["vp_h"] = Value::make_float(h);
    // Same rect under the names the rest of the host already reads.
    self->fields["vx"] = Value::make_float(x);
    self->fields["vy"] = Value::make_float(y);
    self->fields["vw"] = Value::make_float(w);
    self->fields["vh"] = Value::make_float(h);
    auto* state=gameref_upsert(self,GameRefKind::Viewport);state->empty=false;
    state->vp_pri=pri;state->vp_x=x;state->vp_y=y;state->vp_w=w;state->vp_h=h;
    gameref_make_live(self);
  };
  jvm.natives().register_method(
      "java.render.Viewport", "create", "(IFFFF)V",
      [vp_store, as_f](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        const int32_t pri =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        const float x = args.size() > 1 ? as_f(args[1], 0.f) : 0.f;
        const float y = args.size() > 2 ? as_f(args[2], 0.f) : 0.f;
        const float w = args.size() > 3 ? as_f(args[3], 1.f) : 1.f;
        const float h = args.size() > 4 ? as_f(args[4], 1.f) : 1.f;
        vp_store(self, pri, x, y, w, h);
        self->fields["active"] = Value::make_int(0);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.Viewport", "destroy", "()V",
      [](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_void();
        self->fields["vp_ready"] = Value::make_int(0);
        gameref_remove(self);
        self->fields["active"] = Value::make_int(0);
        auto it = self->fields.find("_osd");
        if (it != self->fields.end() && it->second.tag == ValueTag::Obj)
          osd_set_owner_visible(it->second.v.o, false);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.Viewport", "activate", "(I)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        const int32_t flags =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        self->fields["active"] = Value::make_int(1);
        self->fields["vp_renderflags"] = Value::make_int(flags);
        if(auto* state=gameref_get(self)){state->vp_active=true;state->vp_flags=flags;}
        auto it = self->fields.find("_osd");
        if (it != self->fields.end() && it->second.tag == ValueTag::Obj)
          osd_set_owner_visible(it->second.v.o, true);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.Viewport", "deactivate", "()V",
      [](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_void();
        self->fields["active"] = Value::make_int(0);
        self->fields["vp_renderflags"] = Value::make_int(0);
        if(auto* state=gameref_get(self))state->vp_active=false;
        auto it = self->fields.find("_osd");
        if (it != self->fields.end() && it->second.tag == ValueTag::Obj)
          osd_set_owner_visible(it->second.v.o, false);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.Viewport", "getWidth", "()F",
      [vp_alive, vp_f](Object* self, const std::vector<Value>&) {
        if (!vp_alive(self)) return Value::make_float(0.f);
        return Value::make_float(vp_f(self, "vp_w"));
      });
  jvm.natives().register_method(
      "java.render.Viewport", "getHeight", "()F",
      [vp_alive, vp_f](Object* self, const std::vector<Value>&) {
        if (!vp_alive(self)) return Value::make_float(0.f);
        return Value::make_float(vp_f(self, "vp_h"));
      });
  jvm.natives().register_method(
      "java.render.Viewport", "getAspect", "()F",
      [vp_alive](Object* self, const std::vector<Value>&) {
        // PE: (rect.w * display_w) / (rect.w * display_h). The viewport
        // width cancels, so a dialog reports the same aspect as the menu.
        if (!vp_alive(self)) return Value::make_float(1.f);
        const float fb = osd_framebuffer_aspect();
        return Value::make_float(fb > 0.1f ? fb : 1.f);
      });
  jvm.natives().register_method(
      "java.render.Viewport", "getTop", "()F",
      [vp_alive, vp_f](Object* self, const std::vector<Value>&) {
        if (!vp_alive(self)) return Value::make_float(0.f);
        return Value::make_float(vp_f(self, "vp_y"));
      });
  jvm.natives().register_method(
      "java.render.Viewport", "getLeft", "()F",
      [vp_alive, vp_f](Object* self, const std::vector<Value>&) {
        if (!vp_alive(self)) return Value::make_float(0.f);
        return Value::make_float(vp_f(self, "vp_x"));
      });
  jvm.natives().register_method(
      "java.render.Viewport", "resize", "(FFFF)V",
      [vp_alive, vp_store, as_f](Object* self, const std::vector<Value>& args) {
        if (!vp_alive(self)) return Value::make_void();
        const float x = args.size() > 0 ? as_f(args[0], 0.f) : 0.f;
        const float y = args.size() > 1 ? as_f(args[1], 0.f) : 0.f;
        const float w = args.size() > 2 ? as_f(args[2], 0.f) : 0.f;
        const float h = args.size() > 3 ? as_f(args[3], 0.f) : 0.f;
        int32_t pri = 0;
        auto it = self->fields.find("pri");
        if (it != self->fields.end() && it->second.tag == ValueTag::Int)
          pri = it->second.v.i;
        vp_store(self, pri, x, y, w, h);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.Viewport", "unproject",
      "(Ljava.lang.Vector3;I)Ljava.lang.Vector3;",
      [&jvm](Object*, const std::vector<Value>&) {
        // PE walks the viewport cameras and returns null on a miss.
        // No camera is bound on the menu, so the call returns the origin.
        Object* out = jvm.new_object("java.lang.Vector3");
        if (out) {
          out->fields["x"] = Value::make_float(0.f);
          out->fields["y"] = Value::make_float(0.f);
          out->fields["z"] = Value::make_float(0.f);
        }
        return Value::make_obj(out);
      });
  // Camera PE natives — Osd.show() creates a Camera over the OSD viewport.
  jvm.natives().register_method(
      "java.render.Camera", "create",
      "(Ljava.util.resource.ResourceRef;Ljava.render.Viewport;IFFFFFII)V",
      [as_f](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        if (!args.empty()) self->fields["parent"] = args[0];
        if (args.size() > 1) self->fields["vp"] = args[1];
        auto* state=gameref_upsert(self,GameRefKind::Camera);state->empty=false;
        gameref_set_parent(self,!args.empty()&&args[0].tag==ValueTag::Obj?args[0].v.o:nullptr);
        state->viewport=args.size()>1&&args[1].tag==ValueTag::Obj?args[1].v.o:nullptr;
        state->cam_pri=args.size()>2?args[2].v.i:0;
        state->half_aov=args.size()>3?as_f(args[3],45):45;
        state->dmin=args.size()>4?as_f(args[4],.1f):.1f;
        state->dmax=args.size()>5?as_f(args[5],100):100;state->cam_active=true;
        return Value::make_void();
      });
  jvm.natives().register_method("java.render.Camera", "destroy", "()V", [](Object* self,const std::vector<Value>&){gameref_remove(self);return Value::make_void();});
  jvm.natives().register_method(
      "java.render.Camera", "activate", "(Ljava.render.Viewport;I)V",
      [](Object* self,const std::vector<Value>& args){if(auto* s=gameref_get(self)){if(!args.empty()&&args[0].tag==ValueTag::Obj)s->viewport=args[0].v.o;if(args.size()>1)s->cam_pri=args[1].v.i;s->cam_active=true;}return Value::make_void();});
  jvm.natives().register_method(
      "java.render.Camera", "deactivate", "(Ljava.render.Viewport;)V",
      [](Object* self,const std::vector<Value>& args){if(auto* s=gameref_get(self);s&&(args.empty()||(args[0].tag==ValueTag::Obj&&args[0].v.o==s->viewport)))s->cam_active=false;return Value::make_void();});
  jvm.natives().register_method(
      "java.render.Camera", "setFog", "(IFF)V",
      [](Object* self, const std::vector<Value>& args) {
        auto* state = gameref_get(self);
        if (!state) return Value::make_void();
        auto number = [](const Value& value, float fallback) {
          if (value.tag == ValueTag::Float) return value.v.f;
          if (value.tag == ValueTag::Int) return static_cast<float>(value.v.i);
          return fallback;
        };
        const int32_t color = !args.empty() && args[0].tag == ValueTag::Int ? args[0].v.i
                                                                            : 0x0091badb;
        const float near_z = args.size() > 1 ? number(args[1], 1500.f) : 1500.f;
        const float far_z = args.size() > 2 ? number(args[2], 3000.f) : 3000.f;
        gameref_camera_set_fog(*state, color, near_z, far_z);
        return Value::make_void();
      });
  // PhysicsRef PE natives — createButton TREE → createBox hit volumes.
  jvm.natives().register_method(
      "java.util.resource.PhysicsRef", "createBox",
      "(Ljava.util.resource.ResourceRef;FFFLjava.lang.String;)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        auto num = [](const Value& v, float d) {
          if (v.tag == ValueTag::Float) return v.v.f;
          if (v.tag == ValueTag::Int) return static_cast<float>(v.v.i);
          return d;
        };
        const float sx = args.size() > 1 ? num(args[1], 0.1f) : 0.1f;
        const float sy = args.size() > 2 ? num(args[2], 0.1f) : 0.1f;
        Object* group =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        Object* osd = nullptr;
        if (group) {
          gameref_set_parent(self, group);
          auto oit = group->fields.find("osd");
          if (oit != group->fields.end() && oit->second.tag == ValueTag::Obj)
            osd = oit->second.v.o;
        }
        float hw = 0.f, hh = 0.f;
        // World colliders have no OSD owner. Their names are for debugging.
        if (!osd) return Value::make_void();
        osd_mesh_halves(osd, sx, sy, &hw, &hh, nullptr);
        OsdWidget w;
        w.cmd = 0;
        w.hit_only = true;
        w.half_w = hw;
        w.half_h = hh;
        w.owner = osd;
        w.group = osd_current_group();
        if (group) {
          auto git = group->fields.find("_host_group");
          if (git != group->fields.end() && git->second.tag == ValueTag::Int)
            w.group = git->second.v.i;
        }
        // createButton passes the label as the debug string. Hover matches
        // that string against the Text the same call creates.
        if (args.size() > 4 && args[4].tag == ValueTag::Obj && args[4].v.o)
          w.tip = args[4].v.o->str;
        const int32_t wi = static_cast<int32_t>(osd_widgets().size());
        self->fields["_osd_wi"] = Value::make_int(wi);
        osd_add_widget(std::move(w));
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.PhysicsRef", "setMatrix",
      "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self || args.empty() || args[0].tag != ValueTag::Obj || !args[0].v.o)
          return Value::make_void();
        Object* pos = args[0].v.o;
        auto fx = [](Object* o, const char* k) -> float {
          auto it = o->fields.find(k);
          if (it == o->fields.end()) return 0.f;
          if (it->second.tag == ValueTag::Float) return it->second.v.f;
          if (it->second.tag == ValueTag::Int)
            return static_cast<float>(it->second.v.i);
          return 0.f;
        };
        Object* osd = nullptr;
        if (GameRefState* st = gameref_get(self)) {
          if (Object* group = static_cast<Object*>(st->parent)) {
            auto oit = group->fields.find("osd");
            if (oit != group->fields.end() && oit->second.tag == ValueTag::Obj)
              osd = oit->second.v.o;
          }
        }
        float nx = fx(pos, "x"), ny = fx(pos, "y");
        if (osd) osd_world_to_ndc(osd, nx, ny, &nx, &ny);
        auto wit = self->fields.find("_osd_wi");
        if (wit != self->fields.end() && wit->second.tag == ValueTag::Int)
          osd_set_widget_pos(wit->second.v.i, nx, ny);
        self->fields["pos"] = args[0];
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.PhysicsRef", "getPos", "()Ljava.lang.Vector3;",
      [&jvm](Object* self, const std::vector<Value>&) {
        if (self) {
          const auto stored = self->fields.find("pos");
          if (stored != self->fields.end() && stored->second.tag == ValueTag::Obj &&
              stored->second.v.o)
            return stored->second;
        }
        Object* v = jvm.new_object("java.lang.Vector3");
        if (v) {
          const GameRefState* st = gameref_get(self);
          v->fields["x"] = Value::make_float(st ? st->pos[0] : 0.f);
          v->fields["y"] = Value::make_float(st ? st->pos[1] : 0.f);
          v->fields["z"] = Value::make_float(st ? st->pos[2] : 0.f);
        }
        return Value::make_obj(v);
      });
  // java.render.Text natives — legacy RenderNatives path (SCX+atlas).
  jvm.natives().register_method(
      "java.render.Text", "setDefaultColor", "(I)V",
      [](Object*, const std::vector<Value>& a) {
        if (!a.empty() && a[0].tag == ValueTag::Int)
          slrr_text_set_default_color(static_cast<uint32_t>(a[0].v.i));
        return Value::make_void();
      });
  jvm.natives().register_method("java.render.Text", "getDefaultColor", "()I",
      [](Object*, const std::vector<Value>&) {
        return Value::make_int(static_cast<int32_t>(slrr_text_default_color()));
      });
  jvm.natives().register_method(
      "java.render.Text", "create",
      "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;FF)V",
      [&jvm](Object* self, const std::vector<Value>& args) {
        Object* charset =
            (args.size() >= 2 && args[1].tag == ValueTag::Obj) ? args[1].v.o
                                                               : nullptr;
        auto num = [](const Value& v) -> float {
          if (v.tag == ValueTag::Float) return v.v.f;
          if (v.tag == ValueTag::Int) return static_cast<float>(v.v.i);
          return 0.f;
        };
        const float x = args.size() >= 3 ? num(args[2]) : 0.f;
        const float y = args.size() >= 4 ? num(args[3]) : 0.f;
        Object* parent =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        Object* osd = nullptr;
        int32_t group = osd_current_group();
        if (parent) {
          auto oit = parent->fields.find("osd");
          if (oit != parent->fields.end() && oit->second.tag == ValueTag::Obj)
            osd = oit->second.v.o;
          auto git = parent->fields.find("_host_group");
          if (git != parent->fields.end() && git->second.tag == ValueTag::Int)
            group = git->second.v.i;
        }
        float nx = x, ny = y;
        if (osd) osd_map(osd_view(osd), x, y, &nx, &ny);
        ensure_charset_loaded(charset);
        slrr_text_create(self, charset, nx, ny);
        slrr_text_set_group(self, group);
        if (osd) slrr_text_set_osd(self, osd);
        slrr_text_set_font_slot(self, font_slot_for_charset(jvm, charset));
        if (self) {
          self->fields["charset"] = Value::make_obj(charset);
          self->fields["osd"] = Value::make_obj(osd);
          self->fields["_text_x"] = Value::make_float(nx);
          self->fields["_text_y"] = Value::make_float(ny);
          self->fields["_script_y"] = Value::make_float(y);
          // Credits scroll with text.renderinst.setMatrix. The field
          // initializer is a `new RenderRef()` the host often skips (tree
          // longer than the field-init budget), so getfield yields 0 and
          // the scroller moves a throwaway. Own the RenderRef here.
          Object* rr = nullptr;
          auto rit = self->fields.find("renderinst");
          if (rit != self->fields.end() && rit->second.tag == ValueTag::Obj)
            rr = rit->second.v.o;
          if (!rr) {
            rr = jvm.new_object("java.util.resource.RenderRef");
            self->fields["renderinst"] = Value::make_obj(rr);
          }
          if (rr) rr->fields["_host_text"] = Value::make_obj(self);
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.Text", "changeColor", "(I)V",
      [](Object* self, const std::vector<Value>& args) {
        const uint32_t c =
            (!args.empty() && args[0].tag == ValueTag::Int)
                ? static_cast<uint32_t>(args[0].v.i)
                : 0xFFFFFFFFu;
        slrr_text_set_color(self, c);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.Text", "changeAlign", "(I)V",
      [](Object* self, const std::vector<Value>& args) {
        const int32_t a =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 2;
        slrr_text_set_align(self, a);
        // Corner build lines: right align, script y above -0.92. They stay
        // up when a page group is hidden.
        if (self && a == 0) {
          auto it = self->fields.find("_script_y");
          if (it != self->fields.end() && it->second.tag == ValueTag::Float &&
              it->second.v.f < -0.92f)
            slrr_text_set_pinned(self, true);
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.Text", "setPos", "(FF)V",
      [](Object* self, const std::vector<Value>& args) {
        const float x =
            args.size() >= 1 && args[0].tag == ValueTag::Float ? args[0].v.f
                                                               : 0.f;
        const float y =
            args.size() >= 2 && args[1].tag == ValueTag::Float ? args[1].v.f
                                                               : 0.f;
        slrr_text_set_pos(self, x, y);
        return Value::make_void();
      });
  auto sync_button_label = [](Object* self) {
    if (!self) return;
    std::string label;
    auto it = self->fields.find("text");
    if (it != self->fields.end() && it->second.tag == ValueTag::Obj &&
        it->second.v.o)
      label = it->second.v.o->str;
    Object* osd = nullptr;
    auto oit = self->fields.find("osd");
    if (oit != self->fields.end() && oit->second.tag == ValueTag::Obj)
      osd = oit->second.v.o;
    float y = 0.f;
    auto yit = self->fields.find("_text_y");
    if (yit != self->fields.end() && yit->second.tag == ValueTag::Float)
      y = yit->second.v.f;
    osd_sync_button_label(osd, self, y, label);
  };
  jvm.natives().register_method(
      "java.render.Text", "update", "()V",
      [sync_button_label](Object* self, const std::vector<Value>&) {
        if (self) {
          auto it = self->fields.find("text");
          if (it != self->fields.end() && it->second.tag == ValueTag::Obj &&
              it->second.v.o)
            slrr_text_set_string(self, it->second.v.o->str.c_str());
          else if (!self->str.empty())
            slrr_text_set_string(self, self->str.c_str());
          // changeLabelText is Java: it stores the string and calls update.
          // The native changeText never runs, so the hit box kept tip "".
          sync_button_label(self);
        }
        slrr_text_update(self);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.Text", "changeText", "(Ljava.lang.String;)V",
      [](Object* self, const std::vector<Value>& args) {
        Object* s =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        if (self) self->fields["text"] = Value::make_obj(s);
        const std::string label = s ? s->str : std::string{};
        slrr_text_set_string(self, label.c_str());
        slrr_text_update(self);
        // FileRequester rows are addItem("") then changeLabelText. The hit
        // box kept the empty tip, so the name never turned white.
        if (self) {
          Object* osd = nullptr;
          auto oit = self->fields.find("osd");
          if (oit != self->fields.end() && oit->second.tag == ValueTag::Obj)
            osd = oit->second.v.o;
          float y = 0.f;
          auto yit = self->fields.find("_text_y");
          if (yit != self->fields.end() && yit->second.tag == ValueTag::Float)
            y = yit->second.v.f;
          osd_sync_button_label(osd, self, y, label);
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.Text", "getWidthPixels",
      "(Ljava.lang.String;Ljava.util.resource.ResourceRef;)F",
      [](Object*, const std::vector<Value>& args) {
        Object* str =
            (args.size() >= 1 && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                               : nullptr;
        Object* font =
            (args.size() >= 2 && args[1].tag == ValueTag::Obj) ? args[1].v.o
                                                               : nullptr;
        if (font && !slrr_font_ready(font)) {
          int32_t rid = 0;
          auto it = font->fields.find("id");
          if (it != font->fields.end() && it->second.tag == ValueTag::Int)
            rid = it->second.v.i;
          if (!slrr_font_load_from_rid(font, rid))
            slrr_font_load(font, "simple20");
        }
        const char* s = str ? str->str.c_str() : "";
        return Value::make_float(slrr_font_measure_px(font, s));
      });
  // Text.getWidth / getHeight are `2*pixels / (Config.video_* * vp.size)`.
  // The TREE ends on a divide the evaluator rejects, so dialog lines were
  // measured as 0 and drawn from the center of the plate.
  auto ensure_font = [](Object* font) {
    if (font && !slrr_font_ready(font)) {
      int32_t rid = 0;
      auto it = font->fields.find("id");
      if (it != font->fields.end() && it->second.tag == ValueTag::Int)
        rid = it->second.v.i;
      if (!slrr_font_load_from_rid(font, rid)) slrr_font_load(font, "simple20");
    }
  };
  auto vp_span = [](Object* vp, const char* key) -> float {
    if (!vp) return 1.f;
    auto ready = vp->fields.find("vp_ready");
    if (ready == vp->fields.end() || ready->second.tag != ValueTag::Int ||
        ready->second.v.i == 0)
      return 1.f;
    auto it = vp->fields.find(key);
    if (it == vp->fields.end()) return 1.f;
    float v = 1.f;
    if (it->second.tag == ValueTag::Float) v = it->second.v.f;
    else if (it->second.tag == ValueTag::Int)
      v = static_cast<float>(it->second.v.i);
    return v > 0.02f ? v : 1.f;
  };
  // Text.java divides by Config.video_*, the logical mode, not the
  // window. A 1920 back buffer with video_x still at 800 made every token
  // look narrow, so the 10-line catalog box never inserted " (...)".
  auto video_axis = [&jvm](const char* field, int32_t fb, int32_t fallback) {
    Value v = jvm.get_static("java.game.Config", field);
    int32_t n = 0;
    if (v.tag == ValueTag::Int) n = v.v.i;
    else if (v.tag == ValueTag::Float) n = static_cast<int32_t>(v.v.f);
    if (fb > 2) return static_cast<float>(fb);
    if (n > 2) return static_cast<float>(n);
    return static_cast<float>(fallback);
  };
  jvm.natives().register_method(
      "java.render.Text", "getWidth",
      "(Ljava.lang.String;Ljava.util.resource.ResourceRef;Ljava.render.Viewport;)F",
      [ensure_font, vp_span, video_axis](Object*, const std::vector<Value>& args) {
        Object* str =
            (args.size() >= 1 && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                               : nullptr;
        Object* font =
            (args.size() >= 2 && args[1].tag == ValueTag::Obj) ? args[1].v.o
                                                               : nullptr;
        Object* vp =
            (args.size() >= 3 && args[2].tag == ValueTag::Obj) ? args[2].v.o
                                                               : nullptr;
        ensure_font(font);
        const float px = slrr_font_measure_px(font, str ? str->str.c_str() : "");
        const float screen = video_axis("video_x", osd_framebuffer_width(), 800);
        return Value::make_float(2.f * px / (screen * vp_span(vp, "vp_w")));
      });
  jvm.natives().register_method(
      "java.render.Text", "getHeight",
      "(Ljava.util.resource.ResourceRef;Ljava.render.Viewport;)F",
      [ensure_font, vp_span, video_axis](Object*, const std::vector<Value>& args) {
        Object* font =
            (args.size() >= 1 && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                               : nullptr;
        Object* vp =
            (args.size() >= 2 && args[1].tag == ValueTag::Obj) ? args[1].v.o
                                                               : nullptr;
        ensure_font(font);
        float px = 20.f;
        if (const SlrrFont* f = slrr_font_get(font))
          if (f->px_height > 0) px = static_cast<float>(f->px_height);
        const float screen = video_axis("video_y", osd_framebuffer_height(), 600);
        return Value::make_float(2.f * px / (screen * vp_span(vp, "vp_h")));
      });
  jvm.natives().register_method(
      "java.render.Text", "getPos", "()Ljava.lang.Vector3;",
      [&jvm](Object* self, const std::vector<Value>&) {
        Object* v = jvm.new_object("java.lang.Vector3");
        float x = 0.f, y = 0.f;
        if (self) {
          auto ix = self->fields.find("_text_x");
          auto iy = self->fields.find("_text_y");
          if (ix != self->fields.end() && ix->second.tag == ValueTag::Float)
            x = ix->second.v.f;
          if (iy != self->fields.end() && iy->second.tag == ValueTag::Float)
            y = iy->second.v.f;
        }
        v->fields["x"] = Value::make_float(x);
        v->fields["y"] = Value::make_float(y);
        v->fields["z"] = Value::make_float(0.f);
        return Value::make_obj(v);
      });
}

}  // namespace slrr
