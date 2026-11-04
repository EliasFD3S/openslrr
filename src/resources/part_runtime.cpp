#include "slrr/part_runtime.hpp"
#include "slrr/gameref.hpp"
#include "slrr/jvm.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/scene_instances.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace slrr {
namespace {
struct Record {
  InstalledPart state;
  std::shared_ptr<const PartConfig> config;
  std::unordered_map<int, Object *> children;
  std::unordered_set<int> disabled;
  std::vector<Object *> renders;
  float max_wear = 1.f;
};
std::unordered_map<Object *, Record> records;
std::unordered_map<uint32_t, std::shared_ptr<const PartConfig>> configurations;
RpkIndex *index = nullptr;
Jvm *runtime = nullptr;
Object *canonical(Object *object) {
  auto *node = gameref_get(object);
  return node ? static_cast<Object *>(node->key) : object;
}
Record *record(Object *object) {
  auto found = records.find(canonical(object));
  return found == records.end() ? nullptr : &found->second;
}
Object *object(const std::vector<Value> &args, size_t i) {
  return i < args.size() && args[i].tag == ValueTag::Obj ? args[i].v.o
                                                         : nullptr;
}
int number(const std::vector<Value> &args, size_t i, int fallback = 0) {
  return i >= args.size()                 ? fallback
         : args[i].tag == ValueTag::Int   ? args[i].v.i
         : args[i].tag == ValueTag::Float ? static_cast<int>(args[i].v.f)
                                          : fallback;
}
float scalar(const std::vector<Value> &args, size_t i, float fallback = 1) {
  return i >= args.size()                 ? fallback
         : args[i].tag == ValueTag::Float ? args[i].v.f
         : args[i].tag == ValueTag::Int   ? static_cast<float>(args[i].v.i)
                                          : fallback;
}
std::shared_ptr<const PartConfig> configuration(uint32_t resource) {
  auto found = configurations.find(resource);
  if (found != configurations.end())
    return found->second;
  auto cfg = std::make_shared<PartConfig>(load_part_config(*index, resource));
  configurations.emplace(resource, cfg);
  return cfg;
}
std::unordered_map<uint64_t, std::unordered_set<uint64_t>> slot_aliases;
uint64_t slot_identity(uint32_t resource, int slot) {
  return (static_cast<uint64_t>(resource) << 32) | static_cast<uint32_t>(slot);
}
const std::unordered_set<uint64_t> &aliases(uint32_t resource,
                                            const PartSlot &slot) {
  const auto key = slot_identity(resource, slot.id);
  auto found = slot_aliases.find(key);
  if (found != slot_aliases.end())
    return found->second;
  std::unordered_set<uint64_t> result;
  std::vector<PartSlotLink> queue{{resource, slot.id}};
  for (size_t cursor = 0; cursor < queue.size() && result.size() < 256;
       ++cursor) {
    auto current = queue[cursor];
    if (!result.insert(slot_identity(current.resource, current.slot)).second)
      continue;
    auto cfg = configuration(current.resource);
    for (const auto &own : cfg->slots)
      if (own.id == current.slot)
        queue.insert(queue.end(), own.compatible.begin(), own.compatible.end());
  }
  return slot_aliases.emplace(key, std::move(result)).first->second;
}
bool matches(const Record &parent, const PartSlot &mount, const Record &child,
             const PartSlot &mate) {
  const auto &child_aliases = aliases(child.state.resource, mate);
  for (const auto &link : mount.attach)
    if (child_aliases.count(slot_identity(link.resource, link.slot)))
      return true;
  const auto &parent_aliases = aliases(parent.state.resource, mount);
  for (const auto &link : mate.attach)
    if (parent_aliases.count(slot_identity(link.resource, link.slot)))
      return true;
  return false;
}
struct Selection {
  Object *parent = nullptr;
  const PartSlot *mount = nullptr;
  const PartSlot *mate = nullptr;
};
Selection select(Object *child, Object *root, int own, int target,
                 bool free_only = true) {
  auto *c = record(child);
  if (!c)
    return {};
  std::unordered_set<Object *> seen;
  auto visit = [&](auto &&visit, Object *parent) -> Selection {
    auto *p = record(parent);
    if (!p || !seen.insert(parent).second || parent == child)
      return {};
    for (const auto &mount : p->config->slots) {
      if ((target && mount.id != target) ||
          (free_only && p->children.count(mount.id)) ||
          p->disabled.count(mount.id) || mount.id == p->state.own_slot)
        continue;
      for (const auto &mate : c->config->slots)
        if ((!own || mate.id == own) && matches(*p, mount, *c, mate))
          return {parent, &mount, &mate};
    }
    for (const auto &entry : p->children)
      if (auto result = visit(visit, entry.second); result.parent)
        return result;
    return {};
  };
  return visit(visit, canonical(root));
}
Object *car(Object *self) {
  std::unordered_set<Object *> seen;
  self = canonical(self);
  while (auto *part = record(self)) {
    if (!seen.insert(self).second)
      return nullptr;
    if (!part->state.parent)
      return part->config->native_kind == "car" ? self : nullptr;
    self = part->state.parent;
  }
  return nullptr;
}
void render_part(Jvm &jvm, Object *self, Record &rec) {
  auto add = [&](uint32_t resource, const PartPose &pose) {
    if (!resource ||
        !index
             ->model_for_resource(RpkIndex::pack_from_full(resource),
                                  resource & 0xffff)
             .ok)
      return;
    Object *render = jvm.new_object("java.util.resource.RenderRef");
    auto *node = gameref_upsert(render, GameRefKind::RenderRef);
    node->type_id = static_cast<int32_t>(resource);
    node->script = render;
    node->alias = "part render";
    node->paint_source = rec.config->texture;
    node->vehicle_material = true;
    node->paint_texture = rec.state.texture;
    node->empty = false;
    gameref_set_parent(render, self);
    auto transform = part_pose_matrix(pose);
    std::copy(transform.begin(), transform.end(), node->local_matrix);
    node->matrix_override = true;
    render->fields["id"] = Value::make_int(resource);
    rec.renders.push_back(render);
  };
  add(rec.config->render, rec.config->render_pose);
  // Conditional lamps and LOD alternatives are not unconditional body meshes.
  if (!rec.config->renders.empty() &&
      rec.config->render == rec.config->renders[0].resource)
    for (size_t i = 1; i < rec.config->renders.size(); i++)
      if (!rec.config->renders[i].lod && !rec.config->renders[i].flags)
        add(rec.config->renders[i].resource, rec.config->renders[i].pose);
}
void stock_parts(Jvm &jvm, Object *part, Object *descriptor, int depth = 0) {
  if (depth > 48 || records.size() > 2000)
    return;
  jvm.invoke_by_name(part->class_name, "addStockParts",
                     {Value::make_obj(part), Value::make_obj(descriptor)},
                     false);
  auto *current = record(part);
  if (!current)
    return;
  auto stock = current->config->stock;
  for (uint32_t resource : stock) {
    Object *child = part_create(
        jvm, *index, static_cast<Object *>(gameref_get(part)->parent), resource,
        "stockpart");
    if (!child)
      continue;
    if (!part_install(child, part)) {
      part_destroy(child);
      continue;
    }
    stock_parts(jvm, child, descriptor, depth + 1);
  }
}
Object *add_part(Jvm &jvm, Object *parent, uint32_t resource,
                 Object *descriptor, const std::string &alias) {
  if (!record(parent) || records.size() >= 2000)
    return nullptr;
  Object *child = part_create(
      jvm, *index, static_cast<Object *>(gameref_get(parent)->parent), resource,
      alias);
  if (!child)
    return nullptr;
  if (!part_install(child, parent)) {
    part_destroy(child);
    return nullptr;
  }
  if (descriptor) {
    auto *rec = record(child);
    auto field = [&](const char *name, float fallback) {
      auto it = descriptor->fields.find(name);
      return it == descriptor->fields.end()      ? fallback
             : it->second.tag == ValueTag::Float ? it->second.v.f
             : it->second.tag == ValueTag::Int
                 ? static_cast<float>(it->second.v.i)
                 : fallback;
    };
    rec->state.wear = field("wear", 1);
    rec->state.tear = field("tear", 1);
    rec->state.texture = static_cast<uint32_t>(field("color", 0));
    for (auto *render : rec->renders)
      gameref_get(render)->paint_texture = rec->state.texture;
    stock_parts(jvm, child, descriptor);
  }
  return child;
}
} // namespace

Object *part_create(Jvm &jvm, RpkIndex &resources, Object *parent,
                    uint32_t resource, const std::string &alias) {
  index = &resources;
  auto cfg = configuration(resource);
  if (!cfg->valid) {
    for (const auto &error : cfg->errors)
      std::fprintf(stderr, "[part] invalid 0x%x %s: %s\n", resource,
                   cfg->source.c_str(), error.c_str());
    return nullptr;
  }
  std::string script = resources.payload_prop(resource, "script");
  std::replace(script.begin(), script.end(), '\\', '/');
  std::string class_name = "java.game.parts.Part";
  if (!script.empty()) {
    if (!jvm.find_class(std::filesystem::path(script).stem().string()) &&
        !jvm.load_class(
            (std::filesystem::path(resources.game_root()) / script).string()))
      return nullptr;
    auto *cls = jvm.find_class(std::filesystem::path(script).stem().string());
    if (!cls)
      return nullptr;
    class_name = cls->name();
  }
  Object *part = jvm.new_object(class_name);
  if (!part)
    return nullptr;
  Record rec;
  rec.state.object = part;
  rec.state.resource = resource;
  rec.config = cfg;
  // PE copies the cfg `texture` line onto the part at create. Catalog
  // shows that id on the color chip: cars:0x0025 is default_white.
  rec.state.texture = cfg->texture;
  records.emplace(part, std::move(rec));
  auto *node = gameref_upsert(part, GameRefKind::GameType);
  node->type_id = resource;
  node->script = part;
  node->class_name = class_name;
  node->alias = alias;
  int id = gameref_make_live(part);
  if (parent)
    gameref_make_live(parent);
  gameref_set_parent(part, parent);
  jvm.invoke_by_name(class_name, "<init>",
                     {Value::make_obj(part), Value::make_int(id)}, false);
  node = gameref_get(part);
  node->type_id = resource;
  node->script = part;
  node->flags &= ~kWorldTreeRoot;
  node->empty = false;
  gameref_set_parent(part, parent);
  render_part(jvm, part, records.at(part));
  std::printf(
      "[part] create id=0x%x type=0x%x class=%s slots=%zu renders=%zu\n", id,
      resource, class_name.c_str(), cfg->slots.size(),
      records.at(part).renders.size());
  return part;
}
bool part_install(Object *child, Object *root, int own, int target) {
  child = canonical(child);
  root = canonical(root);
  auto *c = record(child);
  if (!c || c->state.parent)
    return false;
  auto selected = select(child, root, own, target);
  if (!selected.parent) {
    std::fprintf(
        stderr, "[part] no compatible free slot child=0x%x root=0x%x\n",
        c->state.resource, record(root) ? record(root)->state.resource : 0);
    return false;
  }
  for (Object *ancestor = selected.parent; ancestor;) {
    if (ancestor == child)
      return false;
    auto *p = record(ancestor);
    ancestor = p ? p->state.parent : nullptr;
  }
  auto *p = record(selected.parent);
  PartMatrix parent_frame, child_frame;
  if (!part_slot_matrix(*p->config, *selected.mount, parent_frame) ||
      !part_slot_matrix(*c->config, *selected.mate, child_frame)) {
    std::fprintf(
        stderr,
        "[part] unsupported slot frame child=0x%x parent=%d/%d own=%d/%d\n",
        c->state.resource, selected.mount->id, selected.mount->type,
        selected.mate->id, selected.mate->type);
    return false;
  }
  auto transform = part_matrix_multiply(parent_frame,
                                        part_matrix_inverse_rigid(child_frame));
  gameref_set_parent(child, selected.parent);
  auto *node = gameref_get(child);
  std::copy(transform.begin(), transform.end(), node->local_matrix);
  node->matrix_override = true;
  c->state.parent = selected.parent;
  c->state.parent_slot = selected.mount->id;
  c->state.own_slot = selected.mate->id;
  p->children[selected.mount->id] = child;
  if (runtime)
    if (auto *chassis = car(child))
      runtime->invoke_by_name(
          chassis->class_name, "addpart",
          {Value::make_obj(chassis), Value::make_obj(child)}, false);
  return true;
}
bool part_command(Object *part, const std::string &command) {
  part = canonical(part);
  auto *rec = record(part);
  if (!rec)
    return false;
  std::istringstream input(command);
  std::string verb;
  input >> verb;
  if (verb == "install") {
    int own = 0, target = 0;
    if (input >> own >> target) {
      if (auto *node = gameref_find_live(target))
        part_install(part, static_cast<Object *>(node->key), own);
    }
    return true;
  }
  if (verb == "remove") {
    int unused = 0, target = 0;
    if (!(input >> unused >> target))
      return true;
    auto *node = gameref_find_live(target);
    if (!node)
      return true;
    if (runtime)
      if (auto *chassis = car(part))
        for (const auto &descendant : part_installed_tree(part))
          runtime->invoke_by_name(
              chassis->class_name, "rempart",
              {Value::make_obj(chassis), Value::make_obj(descendant.object)},
              false);
    if (auto *parent = record(rec->state.parent))
      parent->children.erase(rec->state.parent_slot);
    rec->state.parent = nullptr;
    rec->state.parent_slot = rec->state.own_slot = 0;
    gameref_set_parent(part, node->key);
    gameref_set_matrix(part, 0, 0, 0, 0, 0, 0);
    return true;
  }
  if (verb == "reset" && rec->config->native_kind == "car") {
    // Static garage placement: settle the assembled visual bounds on y=0.
    // The driving/suspension solver is a separate system.
    auto *node = gameref_get(part);
    float low = 0;
    for (const auto &instance : scene_instances_collect_root(part)) {
      const auto &geo = *instance.geometry;
      const auto &m = instance.transform;
      for (size_t i = 0; i < geo.positions.size(); i += 3) {
        float y = m[1] * geo.positions[i] * .01f +
                  m[5] * geo.positions[i + 1] * .01f -
                  m[9] * geo.positions[i + 2] * .01f + m[13];
        low = std::min(low, y);
      }
    }
    if (low < 0)
      node->pos[1] -= low;
    return true;
  }
  return false;
}
bool part_destroy(Object *part) {
  part = canonical(part);
  auto *rec = record(part);
  if (!rec)
    return false;
  auto children = rec->children;
  for (auto entry : children)
    part_destroy(entry.second);
  if (runtime)
    if (auto *chassis = car(part); chassis && chassis != part)
      runtime->invoke_by_name(chassis->class_name, "rempart",
                              {Value::make_obj(chassis), Value::make_obj(part)},
                              false);
  if (auto *parent = record(rec->state.parent))
    parent->children.erase(rec->state.parent_slot);
  for (auto *render : rec->renders)
    gameref_remove(render);
  records.erase(part);
  gameref_remove(part);
  return true;
}
std::vector<InstalledPart> part_installed_tree(Object *root) {
  std::vector<InstalledPart> out;
  std::unordered_set<Object *> seen;
  auto visit = [&](auto &&visit, Object *p) -> void {
    auto *rec = record(p);
    if (!rec || !seen.insert(p).second)
      return;
    out.push_back(rec->state);
    for (auto entry : rec->children)
      visit(visit, entry.second);
  };
  visit(visit, canonical(root));
  return out;
}
bool part_car_ground(void *key, float *y) {
  if (!key || !y || !car(static_cast<Object *>(key)))
    return false;
  bool any = false;
  float low = 0.f;
  for (const auto &instance : scene_instances_collect_root(key)) {
    if (!instance.geometry)
      continue;
    const auto &geo = *instance.geometry;
    const float scale = geo.position_scale;
    const float *m = instance.transform.data();
    const float xs[2] = {geo.info.bbox_min[0] * scale, geo.info.bbox_max[0] * scale};
    const float ys[2] = {geo.info.bbox_min[1] * scale, geo.info.bbox_max[1] * scale};
    const float zs[2] = {geo.info.bbox_min[2] * scale, geo.info.bbox_max[2] * scale};
    for (float x : xs)
      for (float yy : ys)
        for (float z : zs) {
          const float wy = m[1] * x + m[5] * yy - m[9] * z + m[13];
          if (!any || wy < low) {
            low = wy;
            any = true;
          }
        }
  }
  if (!any)
    return false;
  *y = low;
  return true;
}

Object *vehicle_spawn_stock(Jvm &jvm, RpkIndex &resources, Object *parent,
                            uint32_t chassis, uint32_t texture) {
  if (!texture) {
    const auto colors = jvm.get_static("java.game.GameLogic", "CARCOLORS");
    if (colors.tag == ValueTag::Obj && colors.v.o)
      for (const auto &color : colors.v.o->slots)
        if (color.tag == ValueTag::Int && color.v.i) {
          texture = color.v.i;
          break;
        }
  }
  auto *part = part_create(jvm, resources, parent, chassis, "Vehicle chassis");
  if (!part)
    return nullptr;
  auto *descriptor = jvm.new_object("java.game.parts.Descriptor");
  for (const char *name : {"optical", "power", "wear", "tear"})
    descriptor->fields[name] = Value::make_float(1);
  descriptor->fields["color"] = Value::make_int(texture);
  stock_parts(jvm, part, descriptor);
  auto *vehicle = jvm.new_object("java.game.Vehicle");
  vehicle->fields["chassis"] = Value::make_obj(part);
  gameref_bind_alias(vehicle, part);
  return vehicle;
}

void register_part_natives(Jvm &jvm, RpkIndex &resources) {
  runtime = &jvm;
  records.clear();
  configurations.clear();
  slot_aliases.clear();
  index = &resources;
  const std::string owner = "java.game.parts.Part";
  auto bind = [&](const char *name, const char *signature, NativeFn fn) {
    jvm.natives().register_method(owner, name, signature, std::move(fn));
  };
  auto info = [](Object *self, const std::vector<Value> &args) {
    auto *state = gameref_get(self);
    auto *p = record(self);
    const int query = number(args, 0);
    if (!state)
      return Value::make_int(0);
    if (query == 5 || query == 1)
      return Value::make_int(state->live_id);
    if (query == 6)
      return Value::make_int(state->type_id);
    if (query == 55)
      return Value::make_int(p ? p->config->category : 0);
    if (query == 24 || query == 43) {
      auto *parent = gameref_get(state->parent);
      return Value::make_int(parent ? parent->live_id : 0);
    }
    if (query == 71 || query == 72) {
      int id = number(args, 1);
      if (auto *text = object(args, 1)) {
        try {
          id = std::stoi(text->str);
        } catch (...) {
          return Value::make_int(0);
        }
      }
      auto *target = gameref_find_live(id);
      return Value::make_int(
          target && select(canonical(self), static_cast<Object *>(target->key),
                           0, 0, query == 71)
                        .parent
              ? 1
              : 0);
    }
    if (query == 11) {
      float size = 0;
      for (const auto &instance :
           scene_instances_collect_root(canonical(self))) {
        const auto &mesh = instance.geometry->info;
        for (int i = 0; i < 3; ++i)
          size = std::max(size, mesh.bbox_max[i] - mesh.bbox_min[i]);
      }
      return Value::make_int(static_cast<int>(std::ceil(size)));
    }
    return Value::make_int(0);
  };
  jvm.natives().register_method("java.util.resource.GameRef", "getInfo",
                                "(II)I", info);
  jvm.natives().register_method("java.util.resource.GameRef", "getInfo",
                                "(ILjava.lang.String;)I", info);
  bind("getSlots", "()I", [](Object *self, const std::vector<Value> &) {
    auto *p = record(self);
    return Value::make_int(p ? static_cast<int>(p->config->slots.size()) : 0);
  });
  bind("getSlotID", "(I)I", [](Object *self, const std::vector<Value> &a) {
    auto *p = record(self);
    int i = number(a, 0);
    return Value::make_int(!p        ? 0
                           : i == -1 ? p->state.own_slot
                           : i >= 0 && static_cast<size_t>(i) <
                                           p->config->slots.size()
                               ? p->config->slots[i].id
                               : 0);
  });
  bind("partOnSlot", "(I)Ljava.game.parts.Part;",
       [](Object *self, const std::vector<Value> &a) {
         auto *p = record(self);
         if (!p)
           return Value::make_obj(nullptr);
         int slot = number(a, 0);
         if (slot == -1)
           return Value::make_obj(canonical(self));
         if (slot == p->state.own_slot && p->state.parent)
           return Value::make_obj(p->state.parent);
         auto found = p->children.find(slot);
         return Value::make_obj(found == p->children.end() ? nullptr
                                                           : found->second);
       });
  bind("slotIDOnSlot", "(I)I", [](Object *self, const std::vector<Value> &a) {
    auto *p = record(self);
    if (!p)
      return Value::make_int(0);
    int slot = number(a, 0);
    if (slot == p->state.own_slot && p->state.parent)
      return Value::make_int(p->state.parent_slot);
    auto found = p->children.find(slot);
    auto *child = found == p->children.end() ? nullptr : record(found->second);
    return Value::make_int(child ? child->state.own_slot : 0);
  });
  bind("getCarRef", "()Ljava.game.parts.Part;",
       [](Object *s, const std::vector<Value> &) {
         return Value::make_obj(car(s));
       });
  bind("getCar", "()I", [](Object *s, const std::vector<Value> &) {
    return Value::make_int(gameref_make_live(car(s)));
  });
  auto mass_of = [](Object *s, const std::vector<Value> &) {
    auto *p = record(s);
    return Value::make_float(p && p->config ? p->config->mass : 0.f);
  };
  bind("getMass", "()F", mass_of);
  // Chassis.getMass is its own native (empty TREE). The catalog delivery
  // price is chassis.getMass()*5.8; resolving only Part left that call at 0.
  jvm.natives().register_method("java.game.parts.bodypart.Chassis", "getMass",
                                "()F", mass_of);
  for (const auto &property :
       std::vector<std::pair<std::string, float InstalledPart::*>>{
           {"Wear", &InstalledPart::wear}, {"Tear", &InstalledPart::tear}}) {
    jvm.natives().register_method(
        owner, "get" + property.first, "()F",
        [field = property.second](Object *s, const std::vector<Value> &) {
          auto *p = record(s);
          return Value::make_float(p ? p->state.*field : 1);
        });
    jvm.natives().register_method(
        owner, "set" + property.first, "(F)F",
        [field = property.second](Object *s, const std::vector<Value> &args) {
          const float value = scalar(args, 0);
          if (auto *p = record(s); p && std::isfinite(value))
            p->state.*field = value;
          return Value::make_float(value);
        });
    jvm.natives().register_method(
        owner, "set" + property.first, "(F)V",
        [field = property.second](Object *s, const std::vector<Value> &a) {
          if (auto *p = record(s))
            p->state.*field = std::clamp(scalar(a, 0), 0.f, 1.f);
          return Value::make_void();
        });
  }
  bind("setMaxWear", "(F)V", [](Object *self, const std::vector<Value> &args) {
    float value = scalar(args, 0);
    if (auto *p = record(self); p && std::isfinite(value) && value >= 0)
      p->max_wear = value;
    return Value::make_void();
  });
  bind("getWheelID", "()I", [](Object *self, const std::vector<Value> &) {
    auto *p = record(self);
    while (p && p->state.parent) {
      auto *parent = record(p->state.parent);
      if (!parent)
        break;
      if (parent->config->native_kind == "car") {
        int slot = p->state.parent_slot;
        return Value::make_int(slot > 100 && slot <= 400 ? (slot - 101) % 10
                                                         : -1);
      }
      p = parent;
    }
    return Value::make_int(-1);
  });
  jvm.natives().register_method("java.game.parts.DynoData", "newNative", "()V",
                                [](Object *self, const std::vector<Value> &) {
                                  if (self)
                                    self->fields["_host_native_allocated"] =
                                        Value::make_int(1);
                                  return Value::make_void();
                                });
  jvm.natives().register_method(
      "java.game.parts.DynoData", "deleteNative", "()V",
      [](Object *self, const std::vector<Value> &) {
        if (self)
          self->fields.erase("_host_native_allocated");
        return Value::make_void();
      });
  for (bool maximum : {false, true})
    jvm.natives().register_method(
        "java.game.parts.bodypart.Chassis", maximum ? "getMax" : "getMin",
        "()Ljava.lang.Vector3;",
        [&jvm, maximum](Object *self, const std::vector<Value> &) {
          auto *vector = jvm.new_object("java.lang.Vector3");
          float bounds[3]{};
          if (auto *p = record(self)) {
            auto model = index->model_for_resource(
                RpkIndex::pack_from_full(p->config->render),
                p->config->render & 0xffff);
            std::replace(model.source.begin(), model.source.end(), '\\', '/');
            auto mesh = load_scx_geometry(
                (std::filesystem::path(index->game_root()) / model.source)
                    .string());
            if (mesh.info.ok)
              for (int i = 0; i < 3; i++)
                bounds[i] =
                    (maximum ? mesh.info.bbox_max[i] : mesh.info.bbox_min[i]) *
                    .01f;
          }
          for (int i = 0; i < 3; i++)
            vector->fields[i == 0   ? "x"
                           : i == 1 ? "y"
                                    : "z"] = Value::make_float(bounds[i]);
          return Value::make_obj(vector);
        });
  bind("getTexture", "()I", [](Object *s, const std::vector<Value> &) {
    auto *p = record(s);
    return Value::make_int(p ? p->state.texture : 0);
  });
  bind("setTexture", "(I)I", [](Object *s, const std::vector<Value> &a) {
    auto *p = record(s);
    int old = p ? p->state.texture : 0;
    if (p) {
      p->state.texture = number(a, 0);
      for (auto *render : p->renders)
        gameref_get(render)->paint_texture = p->state.texture;
    }
    return Value::make_int(old);
  });
  bind(
      "addPart",
      "(ILjava.lang.String;Ljava.game.parts.Descriptor;)Ljava.game.parts.Part;",
      [&jvm](Object *s, const std::vector<Value> &a) {
        auto *name = object(a, 1);
        return Value::make_obj(add_part(jvm, canonical(s), number(a, 0),
                                        object(a, 2),
                                        name ? name->str : "part"));
      });
  bind("disableSlot", "(II)V", [](Object *s, const std::vector<Value> &a) {
    if (auto *p = record(s)) {
      if (number(a, 1))
        p->disabled.insert(number(a, 0));
      else
        p->disabled.erase(number(a, 0));
    }
    return Value::make_void();
  });
  bind("install_OK",
       "(Ljava.util.resource.GameRef;ILjava.util.resource.GameRef;ILjava.lang."
       "Vector3;)[I",
       [&jvm](Object *s, const std::vector<Value> &a) {
         auto found =
             select(canonical(s), object(a, 0), number(a, 1), number(a, 3));
         if (!found.parent)
           return Value::make_obj(nullptr);
         if (auto *destination = object(a, 2))
           gameref_bind_alias(destination, found.parent);
         auto *pair = jvm.new_object("[I");
         pair->is_array = true;
         pair->slots = {Value::make_int(found.mate->id),
                        Value::make_int(found.mount->id)};
         pair->fields["length"] = Value::make_int(2);
         return Value::make_obj(pair);
       });
}
} // namespace slrr
