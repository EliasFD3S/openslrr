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

void register_resref_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  // ---- java.util.resource.ResourceRef (declaring class only) ----
  // Java ctors: ResourceRef(){ newNative(); } / ResourceRef(I){ this(); set(I); }
  // Do NOT register <init> — let TUFA run those wrappers.
  constexpr const char* kResRef = "java.util.resource.ResourceRef";
  auto res_sync_id_field = [](Object* self, int32_t id) {
    if (self) self->fields["id"] = Value::make_int(id);
  };
  jvm.natives().register_method(
      kResRef, "newNative", "()V",
      [](Object* self, const std::vector<Value>&) {
        // Soft PE ResourceRef.newNative @ 0x47CEA0 — allocate Native.ptr.
        GameRefState* st = gameref_upsert(self);
        if (st) {
          st->script = self;
          st->class_name = self ? self->class_name : std::string{};
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      kResRef, "deleteNative", "()V",
      [](Object* self, const std::vector<Value>&) {
        gameref_remove(self);
        return Value::make_void();
      });
  jvm.natives().register_method(
      kResRef, "set", "(I)V",
      [&rpk, res_sync_id_field](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        int32_t id = 0;
        if (!args.empty()) {
          if (args[0].tag == ValueTag::Int)
            id = args[0].v.i;
          else if (args[0].tag == ValueTag::Float)
            id = static_cast<int32_t>(args[0].v.f);
        }
        GameRefState* st = gameref_upsert(self);
        if (st) {
          st->type_id = id;
          if (id != 0) {
            st->empty = false;
            const RpkEntry* e = rpk.resolve(static_cast<uint32_t>(id));
            if (e) {
              self->fields["typeof"] = Value::make_int(e->typeof_);
              self->str = e->alias;
            }
          }
        }
        res_sync_id_field(self, id);
        return Value::make_void();
      });
  jvm.natives().register_method(
      kResRef, "set", "(Ljava.util.resource.ResourceRef;)V",
      [res_sync_id_field](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        Object* other =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        if (!other) {
          // set(null) → clear bind (PE sub_545FC0(handle,0)).
          if (GameRefState* st = gameref_upsert(self)) {
            st->type_id = 0;
            st->empty = true;
          }
          res_sync_id_field(self, 0);
          return Value::make_void();
        }
        int32_t id = 0;
        if (GameRefState* os = gameref_get(other))
          id = os->type_id;
        if (id == 0) {
          auto it = other->fields.find("id");
          if (it != other->fields.end() && it->second.tag == ValueTag::Int)
            id = it->second.v.i;
        }
        GameRefState* st = gameref_upsert(self);
        if (st) {
          st->type_id = id;
          st->empty = (id == 0);
        }
        res_sync_id_field(self, id);
        return Value::make_void();
      });
  jvm.natives().register_method(
      kResRef, "load", "()V",
      [&jvm, &rpk](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_void();
        GameRefState* st = gameref_upsert(self);
        int32_t id = st ? st->type_id : 0;
        if (id == 0) {
          auto it = self->fields.find("id");
          if (it != self->fields.end() && it->second.tag == ValueTag::Int)
            id = it->second.v.i;
          if (st) st->type_id = id;
        }
        const RpkEntry* e = rpk.resolve(static_cast<uint32_t>(id));
        self->fields["loaded"] = Value::make_int(e ? 1 : 0);
        if (e) {
          self->fields["typeof"] = Value::make_int(e->typeof_);
          self->str = e->alias;
          if (st) st->empty = false;
          const std::string src = rpk.payload_prop(static_cast<uint32_t>(id),
                                                   "sourcefile");
          const std::string tex =
              rpk.payload_prop(static_cast<uint32_t>(id), "texture");
          if (!src.empty())
            self->fields["sourcefile"] =
                Value::make_obj(jvm.new_string(src));
          if (!tex.empty())
            self->fields["texture"] = Value::make_obj(jvm.new_string(tex));
          if (st)
            apply_script_cfg(rpk, st, static_cast<uint32_t>(id));
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      kResRef, "unload", "()V",
      [](Object* self, const std::vector<Value>&) {
        if (self) self->fields["loaded"] = Value::make_int(0);
        return Value::make_void();
      });
  jvm.natives().register_method(
      kResRef, "cache", "()V",
      [&jvm](Object* self, const std::vector<Value>&) {
        // Soft PE cache → same bind path as load (stand-in).
        jvm.invoke("java.util.resource.ResourceRef", "load", "()V",
                   {Value::make_obj(self)}, false);
        return Value::make_void();
      });
  jvm.natives().register_method(
      kResRef, "precache", "()V",
      [&jvm](Object* self, const std::vector<Value>&) {
        jvm.invoke("java.util.resource.ResourceRef", "load", "()V",
                   {Value::make_obj(self)}, false);
        return Value::make_void();
      });
  jvm.natives().register_method(kResRef, "destroy", "()V",
                                [](Object* self, const std::vector<Value>&) {
                                  if (self) {
                                      auto text=self->fields.find("_host_text");
                                      if(text!=self->fields.end()&&text->second.tag==ValueTag::Obj)slrr_text_destroy(text->second.v.o);
                                    auto it = self->fields.find("_osd_wi");
                                    if (it != self->fields.end() &&
                                        it->second.tag == ValueTag::Int)
                                      osd_hide_widget(it->second.v.i);
                                  }
                                  gameref_remove(self);
                                  return Value::make_void();
                                });
  jvm.natives().register_method(
      kResRef, "id", "()I",
      [](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_int(0);
        if (GameRefState* st = gameref_get(self))
          if (st->type_id != 0) return Value::make_int(st->type_id);
        auto it = self->fields.find("id");
        if (it != self->fields.end() && it->second.tag == ValueTag::Int)
          return it->second;
        return Value::make_int(0);
      });
  jvm.natives().register_method(
      kResRef, "type", "()I",
      [](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_int(0);
        auto it = self->fields.find("typeof");
        if (it != self->fields.end() && it->second.tag == ValueTag::Int)
          return it->second;
        return Value::make_int(0);
      });
  jvm.natives().register_method(
      kResRef, "getParentID", "()I",
      [](Object* self, const std::vector<Value>&) {
        GameRefState* st = gameref_get(self);
        if (!st || !st->parent) return Value::make_int(0);
        GameRefState* p = gameref_get(st->parent);
        return Value::make_int(p ? p->type_id : 0);
      });
  jvm.natives().register_method(
      kResRef, "getParent", "()Ljava.util.resource.ResourceRef;",
      [](Object* self, const std::vector<Value>&) {
        GameRefState* st = gameref_get(self);
        Object* p =
            st && st->parent ? static_cast<Object*>(st->parent) : nullptr;
        return Value::make_obj(p);
      });
  auto make_res = [&jvm](uint32_t full) -> Value {
    Object* o = jvm.new_object("java.util.resource.ResourceRef");
    if (!o) return Value::make_obj(nullptr);
    o->fields["id"] = Value::make_int(static_cast<int32_t>(full));
    if (GameRefState* st = gameref_upsert(o)) {
      st->type_id = static_cast<int32_t>(full);
      st->empty = false;
    }
    return Value::make_obj(o);
  };
  jvm.natives().register_method(
      kResRef, "getFirstChild", "()Ljava.util.resource.ResourceRef;",
      [&rpk, make_res](Object* self, const std::vector<Value>&) {
        GameRefState* st = gameref_get(self);
        if (st && !st->children.empty())
          return Value::make_obj(static_cast<Object*>(st->children.front()));
        const int32_t id = resource_id_of(self);
        const int32_t pack = rpk_pack_of(rpk, id);
        const uint32_t child =
            rpk_child_after(rpk, pack, static_cast<uint32_t>(id), 0, true);
        if (!child) return Value::make_obj(nullptr);
        return make_res(child);
      });
  jvm.natives().register_method(
      kResRef, "getNextChild", "()Ljava.util.resource.ResourceRef;",
      [&rpk, make_res](Object* self, const std::vector<Value>&) {
        const int32_t id = resource_id_of(self);
        const int32_t pack = RpkIndex::pack_from_full(static_cast<uint32_t>(id));
        if (pack <= 0) return Value::make_obj(nullptr);
        const RpkEntry* e = rpk.find_entry(pack, static_cast<uint32_t>(id));
        if (!e) return Value::make_obj(nullptr);
        const uint32_t next = rpk_child_after(rpk, pack, e->super_id, e->type_id,
                                              false);
        if (!next) return Value::make_obj(nullptr);
        return make_res(next);
      });
  // Java countChildNodes / getChildNodes leave their counters uninitialized.
  jvm.natives().register_method(
      kResRef, "getChildNodes", "()[Ljava.util.resource.ResourceRef;",
      [&jvm, &rpk, make_res](Object* self, const std::vector<Value>&) {
        const int32_t id = resource_id_of(self);
        const int32_t pack = rpk_pack_of(rpk, id);
        const uint32_t parent_local = static_cast<uint32_t>(id) & 0xFFFFu;
        std::vector<Value> kids;
        // Children live in other packs too. super_id hi16 is the dependency
        // slot (Test_Track.rpk: 0x10ABC → dep 1 = MW_Mod, local MAP_ROOT).
        if (pack > 0) {
          for (int32_t pi = 1; pi < rpk.pack_count() && kids.size() < 512;
               ++pi) {
            const RpkPack* p = rpk.pack_by_id(pi);
            if (!p) continue;
            for (const RpkEntry& e : p->entries) {
              if (kids.size() >= 512) break;
              if ((e.super_id & 0xFFFFu) != parent_local) continue;
              const uint32_t hi = e.super_id >> 16;
              bool match = false;
              if (hi == 0) {
                match = pi == pack;
              } else {
                for (const RpkDep& d : p->deps) {
                  if ((d.id >> 16) != hi) continue;
                  const RpkPack* dep = rpk.pack_by_name(d.path);
                  match = dep && dep->pack_id == pack;
                  break;
                }
              }
              if (!match) continue;
              if (pi == pack && (e.type_id & 0xFFFFu) == parent_local) continue;
              kids.push_back(
                  make_res(RpkIndex::make_full_id(pi, e.type_id)));
            }
          }
        }
        Object* arr = jvm.new_object("java.util.resource.ResourceRef");
        if (arr) {
          arr->is_array = true;
          arr->class_name = "[Ljava.util.resource.ResourceRef;";
          arr->slots = std::move(kids);
          arr->fields["length"] =
              Value::make_int(static_cast<int32_t>(arr->slots.size()));
        }
        std::printf("[rpk] getChildNodes 0x%x pack=%d n=%d\n", id, pack,
                    arr ? arr->fields["length"].v.i : 0);
        return Value::make_obj(arr);
      });
  jvm.natives().register_method(
      kResRef, "getWTRoot", "()Ljava.util.resource.ResourceRef;",
      [](Object* self, const std::vector<Value>&) {
        void* cur = self;
        for (int i = 0; i < 48 && cur; ++i) {
          GameRefState* st = gameref_get(cur);
          if (!st || !st->parent) break;
          if (st->flags & kWorldTreeRoot) break;
          cur = st->parent;
        }
        return Value::make_obj(static_cast<Object*>(cur));
      });
  jvm.natives().register_method(
      kResRef, "makeTexture",
      "(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V", native_noop);
  jvm.natives().register_method(
      kResRef, "makeSound",
      "(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V", native_noop);
  jvm.natives().register_method(
      kResRef, "scaleMesh", "(FFF)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        auto num = [](const Value& v) -> float {
          if (v.tag == ValueTag::Float) return v.v.f;
          if (v.tag == ValueTag::Int) return static_cast<float>(v.v.i);
          return 0.f;
        };
        const float w = !args.empty() ? num(args[0]) : 1.f;
        const float h = args.size() > 1 ? num(args[1]) : 1.f;
        self->fields["_mw"] = Value::make_float(w);
        self->fields["_mh"] = Value::make_float(h);
        return Value::make_void();
      });
  jvm.natives().register_method(
      kResRef, "duplicate", "(Ljava.util.resource.ResourceRef;)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        Object* src =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        if (!src) return Value::make_void();
        int32_t id = 0;
        if (GameRefState* st = gameref_get(src)) id = st->type_id;
        if (id == 0) {
          auto it = src->fields.find("id");
          if (it != src->fields.end() && it->second.tag == ValueTag::Int)
            id = it->second.v.i;
        }
        if (id != 0) {
          self->fields["id"] = Value::make_int(id);
          if (GameRefState* dst = gameref_upsert(self, GameRefKind::RenderRef))
            dst->type_id = id;
        }
        for (const char* k : {"_mw", "_mh", "_tid"}) {
          auto it = src->fields.find(k);
          if (it != src->fields.end()) self->fields[k] = it->second;
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.LoadingScreen", "<init>", "()V", native_noop);
}

}  // namespace slrr
