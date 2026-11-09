#include "slrr/natives_boot.hpp"
#include "natives_internal.hpp"

#include "slrr/gameref.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/host_cursor.hpp"
#include "slrr/host_input.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/render_backend.hpp"
#include "slrr/script_cfg.hpp"
#include "slrr/slrr_font.hpp"
#include "slrr/video_fmv.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace slrr {

// A map GroundRef is a native ground: its triangles live in the instance
// tree. Stitched outdoor maps also carry static object placements.
// The garage shell is already the boot scene.
void spawn_ground_shell(Jvm& jvm, const RpkIndex& rpk, Object* ground) {
  if (!ground || ground->class_name.find("GroundRef") == std::string::npos)
    return;
  GameRefState* st = gameref_get(ground);
  if (!st || st->type_id == 0) return;
  const auto primary = static_cast<uint32_t>(st->type_id);
  std::vector<uint32_t> pending{primary};
  std::unordered_set<uint32_t> visited;
  for (size_t i = 0; i < pending.size(); ++i) {
    const auto id = pending[i];
    if (!visited.insert(id).second) continue;
    auto map = scene_instances_ground_map(rpk, id);
    const uint32_t full = map.render_id;
    if (!full) continue;
    const bool already_attached = std::any_of(st->children.begin(), st->children.end(),
        [full](void* child) {
          const auto* state = gameref_get(child);
          return state && static_cast<uint32_t>(state->type_id) == full;
        });
    if (already_attached) continue;
    const RpkIndex::ModelRef model =
        rpk.model_for_resource(RpkIndex::pack_from_full(full), full & 0xffffu);
    if (model.ok) {
      std::string src = model.source;
      for (char& c : src)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (src.find("garage") != std::string::npos) continue;
    } else if (!map.poly) {
      continue;
    }
    scene_instances_override_mesh(full, map.poly);
    scene_instances_override_collision(full, map.collision);
    if (id != primary || !map.exterior_maps.empty())
      scene_instances_add_ground_props(map);
    Object* node = jvm.new_object("java.util.resource.RenderRef");
    if (!node) return;
    GameRefState* shell = gameref_upsert(node, GameRefKind::RenderRef);
    if (!shell) return;
    shell->script = node;
    shell->class_name = node->class_name;
    shell->empty = false;
    shell->type_id = static_cast<int32_t>(full);
    node->fields["id"] = Value::make_int(static_cast<int32_t>(full));
    gameref_set_parent(node, ground);
    if (id == primary) ground->fields["_ground_shell"] = Value::make_obj(node);
    pending.insert(pending.end(), map.exterior_maps.begin(), map.exterior_maps.end());
    std::printf("[scene] ground shell 0x%x '%s' src=%s poly=%d phys=%zu\n", full,
                model.alias.c_str(), model.source.c_str(),
                map.poly ? map.poly->info.triangles : 0,
                map.collision ? map.collision->indices.size() / 3 : 0);
  }
}

// Catalog categories. The walk is a stack: the last sibling is shown first.
// Other cars' folders are dropped, and the loaded car's folder is moved to
// the end so its parts lead the page. A plain function: a lambda captured
// here would die with this stack frame and crash on the next catalog open.
bool catalog_category_local(uint32_t local) {
  switch (local) {
    case 0x0010: case 0x0012: case 0x001C: case 0x00B6: case 0x00B7:
    case 0x00BD: case 0x00BE: case 0x00C9: case 0x023B: case 0x023C:
    case 0x0246: case 0x0247: case 0x0248: case 0x0249: case 0x024A:
    case 0x024B: case 0x0250: case 0x0252: case 0x0255: case 0x025B:
    case 0x025C: case 0x025D: case 0x025E: case 0x026B: case 0x026E:
    case 0x026F: case 0x0277: case 0x0279: case 0x0281: case 0x0282:
    case 0xD120: case 0xD121: case 0xF228: case 0xF229: case 0xF22A:
    case 0xF22D: case 0xF233: case 0xF235: case 0xF23B: case 0xF23C:
    case 0xF23E: case 0xF23F: case 0xF240: case 0xF241: case 0xF242:
    case 0xF243: case 0xF244: case 0xF245: case 0xF246: case 0xF247:
    case 0xF248: case 0xF249: case 0xF24B: case 0xF24C: case 0xF24F:
    case 0xF27D:
      return true;
    default:
      return false;
  }
}

int player_car_pack(Jvm& jvm) {
  Value pl = jvm.get_static("java.game.GameLogic", "player");
  if (pl.tag != ValueTag::Obj || !pl.v.o) return 0;
  auto cit = pl.v.o->fields.find("car");
  if (cit == pl.v.o->fields.end() || cit->second.tag != ValueTag::Obj ||
      !cit->second.v.o)
    return 0;
  Object* car = cit->second.v.o;
  Object* part = car;
  auto chassis = car->fields.find("chassis");
  if (chassis != car->fields.end() && chassis->second.tag == ValueTag::Obj &&
      chassis->second.v.o)
    part = chassis->second.v.o;
  GameRefState* st = gameref_get(part);
  if (!st || st->type_id == 0) st = gameref_get(car);
  return st ? (st->type_id >> 16) : 0;
}

std::unordered_set<int> vehicle_packs(const RpkIndex& rpk) {
  std::unordered_set<int> packs;
  for (int i = 1; i < rpk.pack_count(); ++i) {
    const RpkPack* p = rpk.pack_by_id(i);
    if (!p) continue;
    std::string path = p->path;
    for (char& c : path)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (path.find("cars") == std::string::npos) continue;
    const uint32_t root = RpkIndex::make_full_id(p->pack_id, 0x1000);
    if (!rpk.resolve(root)) continue;
    for (uint32_t id : rpk.children_of(root))
      packs.insert(RpkIndex::pack_from_full(id));
    break;
  }
  return packs;
}

std::vector<uint32_t> catalog_siblings(Jvm& jvm, const RpkIndex& rpk,
                                       uint32_t parent) {
  auto kids = rpk.children_of(parent);
  if (!catalog_category_local(parent & 0xFFFFu)) return kids;
  const int car = player_car_pack(jvm);
  if (car == 0) return kids;
  const auto vehicles = vehicle_packs(rpk);
  std::vector<uint32_t> keep, mine;
  for (uint32_t id : kids) {
    const int pack = RpkIndex::pack_from_full(id);
    if (pack == car) mine.push_back(id);
    else if (vehicles.count(pack)) continue;
    else keep.push_back(id);
  }
  keep.insert(keep.end(), mine.begin(), mine.end());
  return keep;
}

void register_resref_natives(Jvm& jvm, RpkIndex& rpk, SectionMachine& sections) {
  // ---- java.util.resource.ResourceRef (declaring class only) ----
  // Java ctors: ResourceRef(){ newNative(); } / ResourceRef(I){ this(); set(I); }
  // Do NOT register <init> — let TUFA run those wrappers.
  constexpr const char* kResRef = "java.util.resource.ResourceRef";
  auto res_sync_id_field = [](Object* self, int32_t id) {
    if (self) self->fields["id"] = Value::make_int(id);
  };
  auto release_runtime_sound = [](Object* self) {
    const auto* sound = gameref_get(self);
    if (sound && sound->key == self && !sound->sound_file.empty()) {
      gameref_remove(self);
      self->fields.erase("loaded");
      self->fields.erase("typeof");
    }
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
      [&jvm, &rpk, res_sync_id_field, release_runtime_sound](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        int32_t id = 0;
        if (!args.empty()) {
          if (args[0].tag == ValueTag::Int)
            id = args[0].v.i;
          else if (args[0].tag == ValueTag::Float)
            id = static_cast<int32_t>(args[0].v.f);
        }
        if(auto* live=gameref_find_live(id)) {
          if (live->key != self) release_runtime_sound(self);
          gameref_bind_alias(self,live->key);
          res_sync_id_field(self,id);
          return Value::make_void();
        }
        release_runtime_sound(self);
        GameRefState* st = gameref_upsert(self);
        if(st)st->binding=nullptr;
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
        spawn_ground_shell(jvm, rpk, self);
        return Value::make_void();
      });
  jvm.natives().register_method(
      kResRef, "set", "(Ljava.util.resource.ResourceRef;)V",
      [res_sync_id_field, release_runtime_sound](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        Object* other =
            (!args.empty() && args[0].tag == ValueTag::Obj) ? args[0].v.o
                                                            : nullptr;
        if (other == self || (other && gameref_get(other) &&
                              gameref_get(other) == gameref_get(self)))
          return Value::make_void();
        release_runtime_sound(self);
        if (!other) {
          // set(null) → clear bind (PE sub_545FC0(handle,0)).
          if (GameRefState* st = gameref_upsert(self)) {
            st->binding=nullptr;
            st->type_id = 0;
            st->empty = true;
          }
          res_sync_id_field(self, 0);
          return Value::make_void();
        }
        int32_t id = 0;
        if(auto* live=gameref_get(other);live&&live->live_id) {
          gameref_bind_alias(self,live->key);res_sync_id_field(self,live->live_id);return Value::make_void();
        }
        if (GameRefState* os = gameref_get(other))
          id = os->type_id;
        if (id == 0) {
          auto it = other->fields.find("id");
          if (it != other->fields.end() && it->second.tag == ValueTag::Int)
            id = it->second.v.i;
        }
        GameRefState* st = gameref_upsert(self);
        if (st) {
          st->binding = nullptr;
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
        if (const auto* runtime = gameref_get(self); runtime && !runtime->sound_file.empty()) {
          self->fields["loaded"] = Value::make_int(1);
          self->fields["typeof"] = Value::make_int(6);
          return Value::make_void();
        }
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
                                  // changeTexture destroys the old picture before the
                                  // replacement exists. Drop that quad first: a part
                                  // record returns before the hide below.
                                  if (self) {
                                    auto it = self->fields.find("_osd_wi");
                                    if (it != self->fields.end() &&
                                        it->second.tag == ValueTag::Int)
                                      osd_hide_widget(it->second.v.i);
                                  }
                                  if(part_destroy(self))return Value::make_void();
                                  // Vehicle.destroy() is this native. The chassis is a
                                  // separate part record; leaving it kept the previous
                                  // catalog mesh under the preview camera.
                                  if (self) {
                                    auto chassis = self->fields.find("chassis");
                                    if (chassis != self->fields.end() &&
                                        chassis->second.tag == ValueTag::Obj)
                                      part_destroy(chassis->second.v.o);
                                  }
                                  if (self) {
                                      auto text=self->fields.find("_host_text");
                                      if(text!=self->fields.end()&&text->second.tag==ValueTag::Obj)slrr_text_destroy(text->second.v.o);
                                  }
                                  auto* canonical=gameref_get(self);
                                  gameref_remove(canonical?canonical->key:self);
                                  return Value::make_void();
                                });
  jvm.natives().register_method(
      kResRef, "id", "()I",
      [](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_int(0);
        if (GameRefState* st = gameref_get(self)) {
          if(st->live_id)return Value::make_int(st->live_id);
          if (st->type_id != 0) return Value::make_int(st->type_id);
        }
        auto it = self->fields.find("id");
        if (it != self->fields.end() && it->second.tag == ValueTag::Int)
          return it->second;
        return Value::make_int(0);
      });
  jvm.natives().register_method(
      kResRef, "type", "()I",
      [](Object* self, const std::vector<Value>&) {
        if (!self) return Value::make_int(0);
        if (const auto* sound = gameref_get(self); sound && !sound->sound_file.empty())
          return Value::make_int(6);
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
        return Value::make_int(p ? p->live_id?p->live_id:p->type_id : 0);
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
    // A resource-tree node can expose GameRef script queries before it has a
    // live instance (Catalog walks these handles without instantiating parts).
    Object* o = jvm.new_object("java.util.resource.GameRef");
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
      [&jvm, &rpk, make_res](Object* self, const std::vector<Value>&) {
        GameRefState* st = gameref_get(self);
        if (st && !st->children.empty())
          return Value::make_obj(static_cast<Object*>(st->children.front()));
        const int32_t id = resource_id_of(self);
        const auto children = catalog_siblings(jvm, rpk, static_cast<uint32_t>(id));
        return children.empty() ? Value::make_obj(nullptr) : make_res(children.front());
      });
  jvm.natives().register_method(
      kResRef, "getNextChild", "()Ljava.util.resource.ResourceRef;",
      [&jvm, &rpk, make_res](Object* self, const std::vector<Value>&) {
        const int32_t id = resource_id_of(self);
        const int32_t pack = RpkIndex::pack_from_full(static_cast<uint32_t>(id));
        if (pack <= 0) return Value::make_obj(nullptr);
        const RpkEntry* e = rpk.resolve(static_cast<uint32_t>(id));
        if (!e) return Value::make_obj(nullptr);
        const auto parent = rpk.resolve_embedded_id(pack, e->super_id);
        const auto children = catalog_siblings(jvm, rpk, parent);
        auto found = std::find(children.begin(), children.end(), static_cast<uint32_t>(id));
        if (found == children.end() || ++found == children.end()) return Value::make_obj(nullptr);
        return make_res(*found);
      });
  // Java countChildNodes / getChildNodes leave their counters uninitialized.
  jvm.natives().register_method(
      kResRef, "getChildNodes", "()[Ljava.util.resource.ResourceRef;",
      [&jvm, &rpk, make_res](Object* self, const std::vector<Value>&) {
        const int32_t id = resource_id_of(self);
        const int32_t pack = RpkIndex::pack_from_full(id);
        std::vector<Value> kids;
        for (auto child : rpk.children_of(static_cast<uint32_t>(id)))
          kids.push_back(make_res(child));
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
      "(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V",
      [&jvm](Object* self, const std::vector<Value>& args) {
        if (!self) return Value::make_void();
        std::string file;
        if (args.size() > 1 && args[1].tag == ValueTag::Obj && args[1].v.o)
          file = args[1].v.o->str;
        for (char& c : file)
          if (c == '/') c = '\\';
        if (!file.empty() && !(file.size() > 1 && file[1] == ':')) {
          std::string root = jvm.game_root();
          if (!root.empty() && root.back() != '\\' && root.back() != '/')
            root += '\\';
          file = root + file;
        }
        Object* path = jvm.new_string(file);
        if (path) self->fields["_texpath"] = Value::make_obj(path);
        return Value::make_void();
      });
  jvm.natives().register_method(
      kResRef, "makeSound",
      "(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V",
      [&jvm, res_sync_id_field](Object* self, const std::vector<Value>& args) {
        if (!self || args.size() != 2 || args[0].tag != ValueTag::Obj ||
            args[1].tag != ValueTag::Obj || !args[0].v.o || !args[1].v.o ||
            args[0].v.o == self) return Value::make_void();
        const auto file = under_game(jvm.game_root(), args[1].v.o->str);
        std::error_code ec;
        if (file.empty() || !std::filesystem::is_regular_file(file, ec))
          return Value::make_void();
        auto* sound = gameref_upsert(self);
        sound->binding = nullptr;
        sound->type_id = 0;
        sound->sound_file = file.string();
        gameref_set_parent(self, args[0].v.o);
        res_sync_id_field(self, gameref_make_live(self));
        self->fields["typeof"] = Value::make_int(6);
        self->fields["loaded"] = Value::make_int(1);
        return Value::make_void();
      });
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
          if (GameRefState* dst = gameref_upsert(self, GameRefKind::RenderRef)) {
            dst->type_id = id;
            if (const auto* source = gameref_get(src)) {
              dst->texture_replacements = source->texture_replacements;
              dst->directional_light.flare = source->directional_light.flare;
            } else {
              dst->texture_replacements.clear();
              dst->directional_light.flare = {};
            }
          }
        }
        for (const char* k : {"_mw", "_mh", "_tid", "_texpath"}) {
          auto it = src->fields.find(k);
          if (it != src->fields.end()) self->fields[k] = it->second;
        }
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.render.LoadingScreen", "<init>", "()V", native_noop);
}

}  // namespace slrr
