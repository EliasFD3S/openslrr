#include "slrr/natives_boot.hpp"
#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/script_events.hpp"
#include "slrr/part_runtime.hpp"
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

// A Vehicle is the script handle. When it is not bound to its chassis, the
// mesh still hangs on the chassis field. Pose and parent that mesh.
Object* placed_body(Object* self) {
  Object* body = part_chassis(self);
  if (!body) return self;
  GameRefState* self_node = gameref_get(self);
  GameRefState* body_node = gameref_get(body);
  if (!body_node || body_node == self_node) return self;
  body_node->flags &= ~kWorldTreeRoot;
  body_node->empty = false;
  return body;
}

// Resource `script path.class` → instance whose statics (MapName, MapImage)
// are also instance fields. TrackSelect reads them off the Vector element.
// The track constructor builds the world; the selector only needs the names.
Object* script_instance_for(Jvm& jvm, const RpkIndex& rpk, uint32_t full_id) {
  std::string script = rpk.payload_prop(full_id, "script");
  while (!script.empty() &&
         (script.back() == '\r' || script.back() == '\n' || script.back() == ' '))
    script.pop_back();
  if (script.size() < 7) return nullptr;
  for (char& c : script)
    if (c == '/') c = '\\';
  std::string root = rpk.game_root();
  if (!root.empty() && root.back() != '\\' && root.back() != '/')
    root.push_back('\\');
  const std::string disk = root + script;
  if (!jvm.load_class(disk)) {
    std::printf("[track] class miss %s\n", disk.c_str());
    return nullptr;
  }
  std::string simple = script;
  const auto slash = simple.find_last_of('\\');
  if (slash != std::string::npos) simple = simple.substr(slash + 1);
  if (simple.size() > 6 && simple.compare(simple.size() - 6, 6, ".class") == 0)
    simple.resize(simple.size() - 6);
  const TufaClass* tc = jvm.find_class(simple);
  if (!tc) return nullptr;
  Object* inst = jvm.new_object(tc->name());
  if (!inst) return nullptr;
  for (const auto& f : tc->static_fields) {
    const std::string fname = tc->utf8(f.name);
    if (fname.empty()) continue;
    Value cur = jvm.get_static(tc->name(), fname);
    if (cur.tag == ValueTag::Void && f.tree != kTufaNone &&
        f.tree < tc->trees.size() && !tc->trees[f.tree].empty() &&
        tc->trees[f.tree].size() <= 48) {
      TufaMember m = f;
      if (m.locals < 1) m.locals = 1;
      TreeEvalResult tr = tree_eval(&jvm, *tc, m, nullptr, {});
      if (tr.ok && tr.value.tag != ValueTag::Void) {
        jvm.set_static(tc->name(), fname, tr.value);
        cur = tr.value;
      }
    }
    if (cur.tag == ValueTag::Void) continue;
    inst->fields[fname] = cur;
  }
  std::string shown;
  auto nit = inst->fields.find("MapName");
  if (nit != inst->fields.end() && nit->second.tag == ValueTag::Obj &&
      nit->second.v.o)
    shown = nit->second.v.o->str;
  std::printf("[track] %s MapName=%s\n", tc->name().c_str(), shown.c_str());
  return inst;
}

// VehicleType resources carry only `script Brand_VT.class`. Stock create
// builds that class so the constructor fills vtdarr; init() then ORs the
// set mask. Returning the blank GameRef left every new-dealer slot null.
Object* script_construct(Jvm& jvm, const RpkIndex& rpk, uint32_t full_id,
                         const std::string& alias = "VehicleType") {
  std::string script = rpk.payload_prop(full_id, "script");
  while (!script.empty() && (script.back() == '\r' || script.back() == '\n' ||
                             script.back() == ' '))
    script.pop_back();
  if (script.size() < 7) return nullptr;
  for (char& c : script)
    if (c == '/') c = '\\';
  std::string root = rpk.game_root();
  if (!root.empty() && root.back() != '\\' && root.back() != '/')
    root.push_back('\\');
  const std::string disk = root + script;
  if (!jvm.load_class(disk)) {
    std::printf("[vt] class miss %s\n", disk.c_str());
    return nullptr;
  }
  std::string simple = script;
  const auto slash = simple.find_last_of('\\');
  if (slash != std::string::npos) simple = simple.substr(slash + 1);
  if (simple.size() > 6 && simple.compare(simple.size() - 6, 6, ".class") == 0)
    simple.resize(simple.size() - 6);
  const TufaClass* tc = jvm.find_class(simple);
  if (!tc) return nullptr;
  Object* inst = jvm.new_object(tc->name());
  if (!inst) return nullptr;
  if (GameRefState* st = gameref_upsert(inst, GameRefKind::GameType)) {
    st->type_id = static_cast<int32_t>(full_id);
    st->script = inst;
    st->class_name = tc->name();
    st->alias = alias;
    st->empty = false;
    st->flags &= ~kWorldTreeRoot;
  }
  inst->fields["id"] = Value::make_int(static_cast<int32_t>(full_id));
  jvm.invoke_by_name(
      tc->name(), "<init>",
      {Value::make_obj(inst), Value::make_int(static_cast<int32_t>(full_id))},
      false);

  // A catalog kit is a scripted Set, not a VehicleType or a plain handle.
  // Its constructor supplies the label; build(Inventory) supplies the parts.
  if (jvm.tree_instance_of(inst, "java.game.parts.Set")) {
    std::printf("[set] created %s id=0x%x\n", inst->class_name.c_str(), full_id);
    return inst;
  }

  int models = 0;
  int mask = 0;
  float stock = 0.f;
  auto num = [](const Value& v) -> float {
    if (v.tag == ValueTag::Float) return v.v.f;
    if (v.tag == ValueTag::Int) return static_cast<float>(v.v.i);
    return 0.f;
  };
  auto vec = inst->fields.find("vtdarr");
  if (vec != inst->fields.end() && vec->second.tag == ValueTag::Obj &&
      vec->second.v.o) {
    Object* v = vec->second.v.o;
    auto cit = v->fields.find("elementCount");
    if (cit != v->fields.end())
      models = static_cast<int>(num(cit->second));
    Object* data = nullptr;
    auto dit = v->fields.find("elementData");
    if (dit != v->fields.end() && dit->second.tag == ValueTag::Obj)
      data = dit->second.v.o;
    if (data && data->is_array) {
      const int n = std::min(models, static_cast<int>(data->slots.size()));
      for (int i = 0; i < n; ++i) {
        if (data->slots[static_cast<size_t>(i)].tag != ValueTag::Obj ||
            !data->slots[static_cast<size_t>(i)].v.o)
          continue;
        Object* model = data->slots[static_cast<size_t>(i)].v.o;
        auto mit = model->fields.find("vehicleSetMask");
        const int bits =
            mit == model->fields.end() ? 0 : static_cast<int>(num(mit->second));
        mask |= bits;
        // Constructor adds prevalence for stock (4) and used (2) models.
        if (bits & 0x6) {
          auto pit = model->fields.find("prevalence");
          if (pit != model->fields.end()) stock += num(pit->second);
        }
      }
    }
  }
  auto have = inst->fields.find("vehicleSetMask");
  if (mask && (have == inst->fields.end() || num(have->second) == 0.f))
    inst->fields["vehicleSetMask"] = Value::make_int(mask);
  float prevalence = 0.f;
  auto pit = inst->fields.find("prevalence");
  if (pit != inst->fields.end()) prevalence = num(pit->second);
  if (prevalence == 0.f && stock > 0.f) {
    inst->fields["prevalence"] = Value::make_float(stock);
    prevalence = stock;
  }
  std::printf("[vt] %s id=0x%x models=%d mask=0x%x prevalence=%.0f\n",
              tc->name().c_str(), full_id, models, mask, prevalence);
  return inst;
}

void register_gameref_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  // GameType natives — Soft PE registers on declaring class only
  // (java.lang.GameType). 0-arg createNativeInstance() Java wrapper TREE is
  // brittle (arg underflow) → host leaf forwards to 4-arg native semantics.
  auto gt_set_event_mask = [](Object* self, const std::vector<Value>& args) {
    const int32_t m =
        (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
    gameref_set_event_mask(self, m);
    return Value::make_void();
  };
  auto gt_clear_event_mask = [](Object* self, const std::vector<Value>& args) {
    const int32_t m =
        (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
    gameref_clear_event_mask(self, m);
    return Value::make_void();
  };
  auto gt_add_timer = [](Object* self, const std::vector<Value>& args) {
    // Splash.addTimer(3, 1) — Soft often boxes the delay as Int, not Float.
    // PE: fire_at = physWorldTime + deadline (relative seconds).
    float delay = 0.f;
    if (!args.empty()) {
      if (args[0].tag == ValueTag::Float) delay = args[0].v.f;
      else if (args[0].tag == ValueTag::Int)
        delay = static_cast<float>(args[0].v.i);
    }
    const int32_t id =
        (args.size() >= 2 && args[1].tag == ValueTag::Int) ? args[1].v.i : 0;
    const float fire = gameref_now() + delay;
    gameref_add_timer(self, fire, id);
    if (self && self->class_name.find("Splash") != std::string::npos)
      std::printf("[native] %s.addTimer delay=%.2fs id=%d fire=%.2f\n",
                  self->class_name.c_str(), delay, id, fire);
    return Value::make_void();
  };

  constexpr const char* kGameType = "java.lang.GameType";
  auto gt_create_native4 =
      [](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        GameRefState* st = gameref_upsert(self, GameRefKind::GameType);
        if (!st) return Value::make_void();
        st->script = self;
        st->class_name = self->class_name;
        st->empty = false;
        if (!args.empty() && args[0].tag == ValueTag::Obj)
          gameref_set_parent(self, args[0].v.o);
        if (args.size() >= 2 && args[1].tag == ValueTag::Int)
          st->type_id = args[1].v.i;
        if (args.size() >= 4 && args[3].tag == ValueTag::Obj && args[3].v.o)
          st->alias = args[3].v.o->str;
        if (st->alias == "dummy" || st->alias == "controller") gameref_make_live(self);
        gameref_set_flags(self, kWorldTreeRoot);
        return Value::make_void();
      };
  jvm.natives().register_method(
      kGameType, "createNativeInstance",
      "(Ljava.util.resource.GameRef;ILjava.lang.String;Ljava.lang.String;)V",
      gt_create_native4);
  // Soft wrappers createNativeInstance() / (GameRef) → same PE leaf.
  jvm.natives().register_method(
      kGameType, "createNativeInstance", "()V",
      [gt_create_native4](Object* self, const std::vector<Value>&) {
        return gt_create_native4(self, {});
      });
  jvm.natives().register_method(
      kGameType, "createNativeInstance",
      "(Ljava.util.resource.GameRef;)V",
      [gt_create_native4](Object* self, const std::vector<Value>& args) {
        return gt_create_native4(self, args);
      });
  jvm.natives().register_method(kGameType, "setEventMask", "(I)V",
                                gt_set_event_mask);
  jvm.natives().register_method(kGameType, "clearEventMask", "(I)V",
                                gt_clear_event_mask);
  jvm.natives().register_method(kGameType, "addTimer", "(FI)V", gt_add_timer);
  jvm.natives().register_method(kGameType, "removeAllTimers", "()V",
                                [](Object* self, const std::vector<Value>&) {
                                  gameref_remove_all_timers(self);
                                  return Value::make_void();
                                });
  for(bool enabled:{true,false})jvm.natives().register_method(kGameType,enabled?"registerCallback":"unregisterCallback","(I)V",
    [&jvm,enabled](Object* self,const std::vector<Value>& args){
      if(!args.empty()&&args[0].tag==ValueTag::Int)script_callback_set(jvm,self,args[0].v.i,enabled);
      return Value::make_void();
    });
  jvm.natives().register_method(kGameType,"unregisterCallbacks","()V",
    [&jvm](Object* self,const std::vector<Value>&){script_callbacks_remove(jvm,self);return Value::make_void();});
  jvm.natives().register_method(kGameType,"remNotification","(Ljava.util.resource.GameRef;I)V",
    [&jvm](Object* self,const std::vector<Value>& args){
      if(args.size()>=2&&args[0].tag==ValueTag::Obj&&args[1].tag==ValueTag::Int)
        script_notification_remove(jvm,self,args[0].v.o,args[1].v.i);
      return Value::make_void();
    });
  auto add_notification=[&jvm](Object* self,const std::vector<Value>& args){
    auto text=[&](size_t i){return args.size()>i&&args[i].tag==ValueTag::Obj&&args[i].v.o?args[i].v.o->str:std::string{};};
    if(args.size()>=4&&args[0].tag==ValueTag::Obj&&args[1].tag==ValueTag::Int&&args[2].tag==ValueTag::Int)
      script_notification_add(jvm,self,args[0].v.o,args[1].v.i,args[2].v.i,text(3),text(4));
    return Value::make_void();
  };
  jvm.natives().register_method(kGameType,"addNotification","(Ljava.util.resource.GameRef;IILjava.lang.String;)V",add_notification);
  jvm.natives().register_method(kGameType,"addNotification","(Ljava.util.resource.GameRef;IILjava.lang.String;Ljava.lang.String;)V",add_notification);

  // GameRef pose / flags / parent — used by Dummy, RenderRef, Camera, Scene.
  jvm.natives().register_method(
      "java.util.resource.GameRef", "setFlags", "(I)V",
      [](Object* self, const std::vector<Value>& args) {
        const int32_t f =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        gameref_set_flags(self, f);
        if (f & kWorldTreeLeaf) osd_on_group_leaf(self, true);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "clearFlags", "(I)V",
      [](Object* self, const std::vector<Value>& args) {
        const int32_t f =
            (!args.empty() && args[0].tag == ValueTag::Int) ? args[0].v.i : 0;
        gameref_clear_flags(self, f);
        if (f & kWorldTreeLeaf) osd_on_group_leaf(self, false);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "getFlags", "()I",
      [](Object* self, const std::vector<Value>&) {
        return Value::make_int(gameref_get_flags(self));
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "setMatrix",
      "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V",
      [](Object* self, const std::vector<Value>& args) {
        float px = 0, py = 0, pz = 0, yaw = 0, pitch = 0, roll = 0;
        auto read_vec3 = [](Object* o, float* a, float* b, float* c) {
          if (!o) return;
          auto getf = [&](const char* n, float* out) {
            auto it = o->fields.find(n);
            if (it == o->fields.end()) return;
            if (it->second.tag == ValueTag::Float) *out = it->second.v.f;
            else if (it->second.tag == ValueTag::Int)
              *out = static_cast<float>(it->second.v.i);
          };
          getf("x", a);
          getf("y", b);
          getf("z", c);
        };
        if (!args.empty() && args[0].tag == ValueTag::Obj)
          read_vec3(args[0].v.o, &px, &py, &pz);
        if (args.size() >= 2 && args[1].tag == ValueTag::Obj) {
          Object* o = args[1].v.o;
          if (o) {
            auto getf = [&](const char* n, float* out) {
              auto it = o->fields.find(n);
              if (it == o->fields.end()) return;
              if (it->second.tag == ValueTag::Float) *out = it->second.v.f;
              else if (it->second.tag == ValueTag::Int)
                *out = static_cast<float>(it->second.v.i);
            };
            getf("y", &yaw);
            getf("p", &pitch);
            getf("r", &roll);
          }
        }
        Object* moved = placed_body(self);
        gameref_set_matrix(moved, px, py, pz, yaw, pitch, roll);
        part_park_chassis(moved);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "setState",
      "(Ljava.lang.Vector3;Ljava.lang.Ypr;Ljava.lang.Vector3;Ljava.lang.Vector3;)V",
      [](Object* self, const std::vector<Value>& args) {
        if (!self || args.size() != 4) return Value::make_void();
        float values[12]{};
        for (size_t i = 0; i < 4; ++i) {
          if (args[i].tag != ValueTag::Obj) return Value::make_void();
          const auto* obj = args[i].v.o;
          if (!obj) continue;
          const char* names[3] = {i == 1 ? "y" : "x", i == 1 ? "p" : "y", i == 1 ? "r" : "z"};
          for (size_t j = 0; j < 3; ++j) {
            const auto found = obj->fields.find(names[j]);
            if (found == obj->fields.end()) continue;
            const auto value = found->second;
            values[i * 3 + j] = value.tag == ValueTag::Float ? value.v.f :
                value.tag == ValueTag::Int ? float(value.v.i) : 0.f;
            if (!std::isfinite(values[i * 3 + j])) return Value::make_void();
          }
        }
        gameref_set_matrix(self, values[0], values[1], values[2],
                           values[3], values[4], values[5]);
        if (auto* state = gameref_get(self)) {
          std::copy(values + 6, values + 9, state->vel);
          std::copy(values + 9, values + 12, state->angular_vel);
        }
        part_apply_state(self, values + 6, values + 9);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "getDetail", "()F",
      [](Object* self, const std::vector<Value>&) {
        const auto* state = gameref_get(self);
        return Value::make_float(state && !state->empty ? state->detail : 0.f);
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "setParent",
      "(Ljava.util.resource.ResourceRef;)V",
      [](Object* self, const std::vector<Value>& args) {
        Object* p =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        gameref_set_parent(placed_body(self), p);
        return Value::make_void();
      });
  // Soft PE GameRef lean — Runtime Resources/GameRef_*.
  jvm.natives().register_method(
      "java.util.resource.GameRef", "isEmpty", "()I",
      [](Object* self, const std::vector<Value>&) {
        GameRefState* st = gameref_get(self);
        return Value::make_int(!st || st->empty ? 1 : 0);
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "getScriptInstance", "()Ljava.lang.Object;",
      [](Object* self, const std::vector<Value>&) {
        GameRefState* st = gameref_get(self);
        Object* script =
            st && st->script ? static_cast<Object*>(st->script) : self;
        return Value::make_obj(script);
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "isScripted", "(Ljava.lang.String;)I",
      [&jvm, &rpk](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_int(0);
        std::string want;
        if (!args.empty() && args[0].tag == ValueTag::Obj && args[0].v.o)
          want = args[0].v.o->str;
        const auto* state = gameref_get(self);
        std::string name;
        if (state && state->script && state->live_id) {
          name = static_cast<Object*>(state->script)->class_name;
        } else {
          auto script = rpk.payload_prop(resource_id_of(self), "script");
          std::replace(script.begin(), script.end(), '\\', '/');
          if (script.empty()) {
            return Value::make_int(0);
          }
          const bool fqn = script.find('/') == std::string::npos &&
                           script.find('\\') == std::string::npos &&
                           script.find('.') != std::string::npos &&
                           (script.size() < 6 ||
                            script.compare(script.size() - 6, 6, ".class") != 0);
          if (fqn) {
            name = script;
            if (!jvm.find_class(name)) jvm.load_class(name);
          } else {
            name = std::filesystem::path(script).stem().string();
            if (!jvm.find_class(name))
              jvm.load_class_file((std::filesystem::path(rpk.game_root()) / script).string());
          }
        }
        const auto* cls = jvm.find_class(name);
        if (!cls) return Value::make_int(0);
        if (want.empty()) return Value::make_int(1);
        for (int depth = 0; cls && depth < 64; ++depth) {
          if (cls->name() == want) return Value::make_int(1);
          const auto base = cls->super_name();
          if (base == want) return Value::make_int(1);
          if (base.empty()) break;
          jvm.load_class(base);
          cls = jvm.find_class(base);
        }
        return Value::make_int(0);
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "getPos", "()Ljava.lang.Vector3;",
      [&jvm](Object* self, const std::vector<Value>&) {
        Object* v = jvm.new_object("java.lang.Vector3");
        GameRefState* st = gameref_get(self);
        if (v && st) {
          float x = st->pos[0], y = st->pos[1], z = st->pos[2];
          float sx = 0.f, sy = 0.f, sz = 0.f;
          if (part_car_script_pos(self, &sx, &sy, &sz)) {
            x = sx;
            y = sy;
            z = sz;
          }
          v->fields["x"] = Value::make_float(x);
          v->fields["y"] = Value::make_float(y);
          v->fields["z"] = Value::make_float(z);
        }
        return Value::make_obj(v);
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "setPos", "(Ljava.lang.Vector3;)V",
      [](Object* self, const std::vector<Value>& args) {
        GameRefState* st = gameref_get(self);
        if(!st)st=gameref_upsert(self);
        if (!st || args.empty() || args[0].tag != ValueTag::Obj || !args[0].v.o)
          return Value::make_void();
        Object* o = args[0].v.o;
        auto getf = [&](const char* n, float* out) {
          auto it = o->fields.find(n);
          if (it == o->fields.end()) return;
          if (it->second.tag == ValueTag::Float) *out = it->second.v.f;
          else if (it->second.tag == ValueTag::Int)
            *out = static_cast<float>(it->second.v.i);
        };
        getf("x", &st->pos[0]);
        getf("y", &st->pos[1]);
        getf("z", &st->pos[2]);
          st->matrix_override=false;
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "getOri", "()Ljava.lang.Ypr;",
      [&jvm](Object* self, const std::vector<Value>&) {
        Object* v = jvm.new_object("java.lang.Ypr");
        GameRefState* st = gameref_get(self);
        if (v && st) {
          v->fields["y"] = Value::make_float(st->ypr[0]);
          v->fields["p"] = Value::make_float(st->ypr[1]);
          v->fields["r"] = Value::make_float(st->ypr[2]);
        }
        return Value::make_obj(v);
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "getVel", "()Ljava.lang.Vector3;",
      [&jvm](Object* self, const std::vector<Value>&) {
        Object* v = jvm.new_object("java.lang.Vector3");
        GameRefState* st = gameref_get(self);
        if (v && st) {
          v->fields["x"] = Value::make_float(st->vel[0]);
          v->fields["y"] = Value::make_float(st->vel[1]);
          v->fields["z"] = Value::make_float(st->vel[2]);
        }
        return Value::make_obj(v);
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "queueEvent",
      "(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V",
      [&jvm](Object* self, const std::vector<Value>& args) {
        // EVENT_COMMAND = 0x10. Garage.cameraSetup sends smooth/dist/zoom/force/torque.
        int type = 0;
        if (args.size() > 1) {
          if (args[1].tag == ValueTag::Int) type = args[1].v.i;
          else if (args[1].tag == ValueTag::Float)
            type = static_cast<int>(args[1].v.f);
        }
        if (type != 0x10 || args.size() < 3 || args[2].tag != ValueTag::Obj ||
            !args[2].v.o)
          return Value::make_void();
        GameRefState* st = gameref_get(self);
        const std::string& line = args[2].v.o->str;
        if (st && st->alias == "controller" && line == "setcontrol") {
          jvm.invoke("java.io.Controller", "setcontrol", "()V",
                     {Value::make_obj(self)}, false);
          return Value::make_void();
        }
        if (part_command(self, line)) return Value::make_void();
        if (!st || (st->type_id & 0xFFFF) != 0x33) return Value::make_void();
        gameref_scene_command(st->key, line);
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "setActiveCollision", "()V", native_noop);

  // GameRef.create / create_native — Soft PE @ 0x47D… (declaring: GameRef).
  // Java ctors call create(…) after super(); do not stub GameRef.<init>.
  auto gameref_bind_create =
      [&rpk](Object* self, Object* parent, Object* type, Object* alias_o) {
        GameRefState* st = gameref_upsert(self, GameRefKind::Generic);
        if (!st) return;
        st->script = self;
        st->class_name = self ? self->class_name : std::string{};
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
          if (self) self->fields["id"] = Value::make_int(tid);
        }
        if (alias_o && !alias_o->str.empty()) st->alias = alias_o->str;
        if (st->type_id)
          apply_script_cfg(rpk, st, static_cast<uint32_t>(st->type_id));
      };
  jvm.natives().register_method(
      "java.util.resource.GameRef", "create_native",
      "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;"
      "Ljava.lang.String;Ljava.lang.String;)V",
      [gameref_bind_create](Object* self, const std::vector<Value>& args) {
        Object* parent =
            (args.size() >= 1 && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                               : nullptr;
        Object* type =
            (args.size() >= 2 && args[1].tag == ValueTag::Obj) ? args[1].v.o
                                                               : nullptr;
        Object* alias =
            (args.size() >= 4 && args[3].tag == ValueTag::Obj) ? args[3].v.o
                                                               : nullptr;
        gameref_bind_create(self, parent, type, alias);
        Object* params =
            (args.size() >= 3 && args[2].tag == ValueTag::Obj) ? args[2].v.o
                                                               : nullptr;
        gameref_bind_scene_camera(self, params ? params->str : std::string{});
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.util.resource.GameRef", "create",
      "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;"
      "Ljava.lang.String;Ljava.lang.String;)Ljava.lang.GameType;",
      [&jvm, &rpk, gameref_bind_create](Object* self,
                                        const std::vector<Value>& args) {
        Object* parent =
            (args.size() >= 1 && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                               : nullptr;
        Object* type =
            (args.size() >= 2 && args[1].tag == ValueTag::Obj) ? args[1].v.o
                                                               : nullptr;
        Object* alias =
            (args.size() >= 4 && args[3].tag == ValueTag::Obj) ? args[3].v.o
                                                               : nullptr;
        gameref_bind_create(self, parent, type, alias);
        Object* params =
            (args.size() >= 3 && args[2].tag == ValueTag::Obj) ? args[2].v.o
                                                               : nullptr;
        gameref_bind_scene_camera(self, params ? params->str : std::string{});
        int32_t id = 0;
        if (type) {
          if (GameRefState* ts = gameref_get(type)) id = ts->type_id;
          if (id == 0) {
            auto it = type->fields.find("id");
            if (it != type->fields.end() && it->second.tag == ValueTag::Int)
              id = it->second.v.i;
          }
        }
        const auto native = rpk.payload_prop(static_cast<uint32_t>(id), "native");
        if(id && (native.rfind("car ",0)==0 || native.rfind("part ",0)==0)) {
          Object* part=part_create(jvm,rpk,parent,static_cast<uint32_t>(id),alias?alias->str:"part",params?params->str:std::string{});
          if(part)gameref_bind_alias(self,part);
          return Value::make_obj(part);
        }
        // Track selector metadata remains lazy; native part resources use
        // the script-backed factory above, with independent live handles.
        const std::string alias_s =
            (alias && !alias->str.empty()) ? alias->str : std::string{};
        if (id && alias_s.find("Track") != std::string::npos) {
          if (Object* inst =
                  script_instance_for(jvm, rpk, static_cast<uint32_t>(id)))
            return Value::make_obj(inst);
        }
        if (id && alias_s == "VehicleType") {
          if (Object* inst =
                  script_construct(jvm, rpk, static_cast<uint32_t>(id)))
            return Value::make_obj(inst);
        }
        if (id && alias_s == "set_loader") {
          if (Object* inst = script_construct(jvm, rpk, static_cast<uint32_t>(id), alias_s)) {
            gameref_set_parent(inst, parent);
            gameref_bind_alias(self, inst);
            return Value::make_obj(inst);
          }
          return Value::make_obj(nullptr);
        }
        return Value::make_obj(self);
      });
}

}  // namespace slrr
