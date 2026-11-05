#include "slrr/part_runtime.hpp"
#include "slrr/dyno_model.hpp"
#include "slrr/gameref.hpp"
#include "slrr/jvm.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/scx_mesh.hpp"
#include "slrr/suspension_geometry.hpp"
#include "slrr/vehicle_dynamics.hpp"
#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace slrr {
namespace {
// One wheel record of a chassis. getWheel hands out a fresh wrapper; the
// setters land here. Width is stored as half the script value (the exe
// multiplies by 0.5). Pacejka 0/2/4/11/12/14 mirror sliction, friction,
// stiffness, max load, frictn_x and load smooth.
struct WheelPhys {
  bool has_arm = false, has_hub = false;
  bool steer_set = false;
  bool live = false, contact = false, has_pos = false, has_ypr = false;
  float pos[3]{}, ypr[3]{};
  float drive = 0, steer = 0, radius = 0, width = 0;
  float cpatch_hw = 0, cpatch_ang = 0, cpatch_off = 0;
  float friction = 1, frictn_x = 1, sliction = 1, stiffness = 100;
  float roll_res = 0.001f, bearing = 20, max_load = 1000, load_smooth = 0;
  float pacejka[17] = {1.4f, 0, 1.49f,  0,   15.2f,  0,    0, 0, -1.f,
                       0,    0, 8000.f, 1.f, 0.015f, 0.4f, 0, 0};
  float force = 0, damp_bound = 0, damp_rebound = 0;
  float rest = 0, min_len = 0, max_len = 0;
  float ic[6]{};
  float brake = 0, hbrake = 0;
  int opp = -1;
  float arm[7]{}, hub[10]{};
  bool authored_position = false;
  float input_position[3]{};
};
struct SfxItem {
  int sfx = 0;
  float pitch = 1, pmin = 0, pmax = 0, vmin = 0, vmax = 0;
};
struct Record {
  float steering_input = 0;
  bool has_steering_transform = false;
  PartMatrix steering_transform{};
  InstalledPart state;
  std::shared_ptr<const PartConfig> config;
  std::unordered_map<int, Object *> children;
  // disableSlot status: 0 absent, 1 blocked, 2 blocked-and-hidden.
  std::unordered_map<int, int> slot_status;
  std::unordered_map<int, std::string> slot_damage;
  struct PoseOverride {
    bool pos = false, ypr = false;
    float p[3]{}, y[3]{};
  };
  std::unordered_map<int, PoseOverride> slot_pose;
  std::vector<Object *> renders;
  struct LinkVisual {
    Object *node = nullptr;
    uint32_t resource = 0;
    std::vector<PartLink> rules;
  };
  std::vector<LinkVisual> links;
  float max_wear = 1.f;
  int flap = 0;
  // Seventh field of the create string, an index into each render's colors.
  bool colour_set = false;
  int colour_index = 0;
  bool mesh_set = false, render_set = false;
  uint32_t mesh_id = 0, render_id = 0;
  float sfx_a = 0, sfx_b = 0;
  float torque = 0, torque_scale = 0, ackermann = 0, mileage = 0;
  float cooling[3] = {10.f, 50.f, 0.01f};
  float exhaust_min = 0, steer_radius = 0, steer_r = 0, steer_z = 0;
  int nitro_sfx = 0;
  float nitro_pitch = 1;
  struct Horn {
    int sfx = 0;
    float pitch = 1;
  };
  std::unordered_map<int, Horn> horns;
  struct Buck {
    int part = 0, id = 0;
    float freq = 0, prob = 0, rpm = 0, amp = 0;
  };
  std::vector<Buck> bucks;
  std::array<WheelPhys, 8> wheels{};
  std::array<std::string, 8> wheel_damage{};
  bool dynamic_frames = false;
  bool physics_pending_placement = true;
  // start releases the body. Until then a chassis only settles onto a floor.
  // The catalog preview has none, so integrating gravity walks it out of frame.
  bool physics_released = false;
  unsigned physics_quiet_ticks = 0;
  std::array<PartMatrix, 4> travel_frame{}, carrier_frame{}, hub_frame{};
  std::array<bool, 4> geometry_solved{};
  std::array<physics::Vec3, 4> base_wheel_center{};
  physics::VehicleDynamics dynamics;
  bool physics_ready = false, physics_dirty = true, physics_suspended = false;
  bool physics_reported = false;
  int reported_contacts = -1, physics_status_lines = 0;
  void *physics_parent = nullptr;
  PartMatrix published_frame{};
  std::array<Object *, 3> sfx{};
};
std::unordered_map<Object *, std::vector<SfxItem>> sfx_items;
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
PartMatrix node_local(Object *obj) {
  PartMatrix m{};
  m[0] = m[5] = m[10] = m[15] = 1;
  auto *n = gameref_get(obj);
  if (!n)
    return m;
  if (n->matrix_override) {
    std::copy(n->local_matrix, n->local_matrix + 16, m.begin());
    return m;
  }
  PartPose pose;
  std::copy(n->pos, n->pos + 3, pose.position.begin());
  std::copy(n->ypr, n->ypr + 3, pose.rotation.begin());
  return part_pose_matrix(pose);
}
PartMatrix world_matrix(Object *obj) {
  std::vector<PartMatrix> chain;
  for (int i = 0; obj && i < 32; ++i) {
    chain.push_back(node_local(obj));
    auto *rec = record(obj);
    obj = rec ? rec->state.parent : nullptr;
  }
  PartMatrix w{};
  w[0] = w[5] = w[10] = w[15] = 1;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    w = part_matrix_multiply(w, *it);
  return w;
}
bool slot_world(Object *parent, const PartSlot &slot, float out[3]) {
  auto *p = record(parent);
  if (!p)
    return false;
  PartMatrix local;
  if (!part_slot_matrix(*p->config, slot, local))
    return false;
  const PartMatrix world = part_matrix_multiply(world_matrix(parent), local);
  out[0] = world[12];
  out[1] = world[13];
  out[2] = world[14];
  return true;
}
// `hint` is a drop point. Without it the first compatible free slot wins,
// which is how a stock install walks the tree. With it, the slot whose
// mount is closest to the point wins.
Selection select(Object *child, Object *root, int own, int target,
                 bool free_only = true, const float *hint = nullptr) {
  auto *c = record(child);
  if (!c)
    return {};
  std::unordered_set<Object *> seen;
  Selection best;
  float best_d = 0;
  bool any = false;
  auto consider = [&](Object *parent, const PartSlot *mount,
                      const PartSlot *mate) {
    if (!hint) {
      best = {parent, mount, mate};
      return true;
    }
    float at[3]{};
    float d = 1e30f;
    if (slot_world(parent, *mount, at)) {
      const float dx = at[0] - hint[0], dy = at[1] - hint[1],
                  dz = at[2] - hint[2];
      d = dx * dx + dy * dy + dz * dz;
    }
    if (!any || d < best_d) {
      best = {parent, mount, mate};
      best_d = d;
      any = true;
    }
    return false;
  };
  auto visit = [&](auto &&visit, Object *parent) -> bool {
    auto *p = record(parent);
    if (!p || !seen.insert(parent).second || parent == child)
      return false;
    for (const auto &mount : p->config->slots) {
      const auto blocked = p->slot_status.find(mount.id);
      if ((target && mount.id != target) ||
          (free_only && p->children.count(mount.id)) ||
          (blocked != p->slot_status.end() && blocked->second) ||
          mount.id == p->state.own_slot)
        continue;
      for (const auto &mate : c->config->slots)
        if ((!own || mate.id == own) && matches(*p, mount, *c, mate))
          if (consider(parent, &mount, &mate))
            return true;
    }
    for (const auto &entry : p->children)
      if (visit(visit, entry.second))
        return true;
    return false;
  };
  visit(visit, canonical(root));
  return best;
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
  auto paint_of = [&](const std::vector<uint32_t> &colors) {
    uint32_t paint = 0xffffff;
    if (!rec.colour_set || colors.empty())
      return paint;
    int count = static_cast<int>(colors.size());
    int pick = rec.colour_index % count;
    if (pick < 0)
      pick += count;
    return colors[static_cast<size_t>(pick)];
  };
  auto add = [&](uint32_t resource, const PartMatrix &local,
                 const std::vector<PartLink> *rules,
                 const std::vector<uint32_t> *colors,
                 uint32_t light_flags = 0) {
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
    node->light_flags = light_flags;
    node->paint_texture = rec.state.texture;
    node->color = paint_of(colors ? *colors : std::vector<uint32_t>{});
    node->empty = false;
    gameref_set_parent(render, self);
    std::copy(local.begin(), local.end(), node->local_matrix);
    node->matrix_override = true;
    render->fields["id"] = Value::make_int(resource);
    rec.renders.push_back(render);
    if (rules && !rules->empty())
      rec.links.push_back({render, resource, *rules});
  };
  const PartRender *chosen =
      rec.config->renders.empty() ? nullptr : &rec.config->renders[0];
  for (const auto &r : rec.config->renders) {
    if (r.wheel >= 0 || r.flags)
      continue;
    if (!chosen || chosen->flags || chosen->wheel >= 0 ||
        r.lod_level >= chosen->lod_level)
      chosen = &r;
  }
  add(rec.config->render, part_pose_matrix(rec.config->render_pose),
      chosen ? &chosen->links : nullptr, chosen ? &chosen->colors : nullptr);
  // Wheel meshes are placed on their wheel below. The other LOD levels
  // are stand-ins for this mesh. A light-state flag is a separate mesh
  // (city, brake, reverse, indicator), shown only while that light is on.
  for (const auto &r : rec.config->renders) {
    if (r.flags || r.wheel >= 0 || r.lod_level >= 0 ||
        r.resource == rec.config->render)
      continue;
    add(r.resource, part_pose_matrix(r.pose), &r.links, &r.colors);
  }
  for (const auto &r : rec.config->renders) {
    if (r.wheel >= 0 || (r.flags & 0x40u) || (r.flags & 0x13fu) == 0)
      continue;
    add(r.resource, part_pose_matrix(r.pose), &r.links, &r.colors, r.flags);
  }
  // A car with no script draws its wheels from the cfg `render` lines that
  // follow each `wheel`. They stay on the wheel, not on the chassis origin.
  if (rec.config->native_kind == "car" &&
      self->class_name == "java.game.parts.Part") {
    for (const auto &r : rec.config->renders) {
      if (r.wheel < 0 || r.flags)
        continue;
      if (r.wheel >= static_cast<int>(rec.config->wheels.size()))
        continue;
      bool finer = false;
      for (const auto &other : rec.config->renders)
        if (&other != &r && other.wheel == r.wheel && !other.flags &&
            other.lod_level > r.lod_level)
          finer = true;
      if (finer)
        continue;
      PartMatrix local = part_matrix_multiply(
          part_pose_matrix(rec.config->wheels[static_cast<size_t>(r.wheel)]),
          part_pose_matrix(r.pose));
      add(r.resource, local, &r.links, &r.colors);
    }
  }
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
  // Part.addPart installs onto the car (`install 0 <car> 0 <caller> 0`).
  // The caller's own slots win, so a camshaft stays on the head. A part the
  // caller cannot hold, such as the camshaft belt added by the head and
  // attached to the crankshaft, is then sought on the rest of the car.
  Object *root = parent;
  if (!select(child, parent, 0, 0).parent)
    if (Object *chassis = car(parent); chassis && chassis != parent)
      root = chassis;
  if (!part_install(child, root)) {
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
void release_side(Record &rec) {
  for (Object *tab : rec.sfx)
    if (tab)
      sfx_items.erase(tab);
}
float comp(Object *o, const char *name) {
  if (!o)
    return 0;
  auto it = o->fields.find(name);
  if (it == o->fields.end())
    return 0;
  if (it->second.tag == ValueTag::Float)
    return it->second.v.f;
  if (it->second.tag == ValueTag::Int)
    return static_cast<float>(it->second.v.i);
  return 0;
}
int field_int(Object *o, const char *name, int fallback = 0) {
  if (!o)
    return fallback;
  auto it = o->fields.find(name);
  if (it == o->fields.end())
    return fallback;
  if (it->second.tag == ValueTag::Int)
    return it->second.v.i;
  if (it->second.tag == ValueTag::Float)
    return static_cast<int>(it->second.v.f);
  return fallback;
}
Object *as_part(Object *obj, int depth = 0) {
  if (!obj || depth > 4)
    return nullptr;
  Object *key = canonical(obj);
  if (record(key))
    return key;
  auto it = obj->fields.find("chassis");
  if (it != obj->fields.end() && it->second.tag == ValueTag::Obj)
    return as_part(it->second.v.o, depth + 1);
  return nullptr;
}
int res_id(Object *o) {
  if (!o)
    return 0;
  if (auto *st = gameref_get(o); st && st->type_id)
    return st->type_id;
  return field_int(o, "id");
}
void retarget(Record &rec, uint32_t id) {
  if (rec.renders.empty())
    return;
  auto *n = gameref_get(rec.renders[0]);
  if (!n)
    return;
  n->type_id = static_cast<int32_t>(id);
  n->empty = id == 0;
}
PartSlot posed_slot(const Record &rec, const PartSlot &slot) {
  PartSlot copy = slot;
  auto it = rec.slot_pose.find(slot.id);
  if (it == rec.slot_pose.end())
    return copy;
  if (it->second.pos)
    std::copy(it->second.p, it->second.p + 3, copy.pose.position.begin());
  if (it->second.ypr)
    std::copy(it->second.y, it->second.y + 3, copy.pose.rotation.begin());
  return copy;
}
PartMatrix eye_matrix() {
  PartMatrix m{};
  m[0] = m[5] = m[10] = m[15] = 1.f;
  return m;
}
// Root mount type 101–400 selects the corner. The CFG id is not the wheel
// index.
int mount_group(Object *part) {
  int type = 0;
  std::unordered_set<Object *> seen;
  for (Object *child = part; child && seen.insert(child).second;) {
    auto *rec = record(child);
    if (!rec || !rec->state.parent)
      break;
    auto *parent = record(rec->state.parent);
    if (!parent)
      break;
    for (const auto &slot : parent->config->slots)
      if (slot.id == rec->state.parent_slot) {
        type = slot.type;
        break;
      }
    child = rec->state.parent;
  }
  return type >= 101 && type <= 400 ? (type - 101) % 10 : 0;
}
bool linked_frame(Object *part, int base, PartMatrix &out,
                  bool follow_steering = true) {
  if (!base)
    return false;
  const int id = base + mount_group(part);
  std::unordered_set<Object *> seen;
  for (Object *owner = part; owner && seen.insert(owner).second;) {
    auto *rec = record(owner);
    if (!rec)
      break;
    const PartSlot *slot = nullptr;
    for (const auto &candidate : rec->config->slots)
      if (candidate.id == id) {
        slot = &candidate;
        break;
      }
    if (!slot) {
      owner = rec->state.parent;
      continue;
    }
    PartMatrix frame;
    if (!part_slot_matrix(*rec->config, posed_slot(*rec, *slot), frame))
      return false;
    const int type = slot->type;
    if (type >= 101 && type <= 400) {
      for (const auto &mount : rec->config->slots) {
        if (mount.type != type)
          continue;
        auto it = rec->children.find(mount.id);
        if (it == rec->children.end())
          continue;
        auto *child = record(it->second);
        if (!child)
          continue;
        const PartSlot *own = nullptr;
        for (const auto &s : child->config->slots)
          if (s.id == child->state.own_slot) {
            own = &s;
            break;
          }
        if (!own)
          continue;
        PartMatrix own_frame, mount_frame;
        if (!part_slot_matrix(*child->config, posed_slot(*child, *own),
                              own_frame) ||
            !part_slot_matrix(*rec->config, posed_slot(*rec, mount),
                              mount_frame))
          return false;
        auto carrier = node_local(it->second);
        if (!follow_steering && child->has_steering_transform)
          carrier = part_matrix_multiply(
              part_matrix_inverse_rigid(child->steering_transform), carrier);
        frame = part_matrix_multiply(
            part_matrix_multiply(part_matrix_multiply(carrier, own_frame),
                                 part_matrix_inverse_rigid(mount_frame)),
            frame);
        break;
      }
    }
    out = part_matrix_multiply(
        part_matrix_multiply(part_matrix_inverse_rigid(world_matrix(part)),
                             world_matrix(owner)),
        frame);
    return true;
  }
  return false;
}
bool mesh_span(uint32_t resource, float &min_y, float &max_y, float &cx,
               float &cz) {
  struct Box {
    bool ok = false;
    float min[3]{}, max[3]{};
  };
  static std::unordered_map<uint32_t, Box> cache;
  auto found = cache.find(resource);
  if (found == cache.end()) {
    Box box;
    if (index) {
      auto model = index->model_for_resource(RpkIndex::pack_from_full(resource),
                                             resource & 0xffffu);
      if (model.ok && !model.source.empty()) {
        std::replace(model.source.begin(), model.source.end(), '\\', '/');
        auto mesh = load_scx_geometry(
            (std::filesystem::path(index->game_root()) / model.source)
                .string());
        if (mesh.info.ok) {
          box.ok = true;
          for (int i = 0; i < 3; ++i) {
            box.min[i] = mesh.info.bbox_min[i] * 0.01f;
            box.max[i] = mesh.info.bbox_max[i] * 0.01f;
          }
        }
      }
    }
    found = cache.emplace(resource, box).first;
  }
  if (!found->second.ok || found->second.max[1] - found->second.min[1] < 1e-6f)
    return false;
  min_y = found->second.min[1];
  max_y = found->second.max[1];
  cx = 0.5f * (found->second.min[0] + found->second.max[0]);
  cz = 0.5f * (found->second.min[2] + found->second.max[2]);
  return true;
}
PartMatrix span_between(const PartMatrix &a, const PartMatrix &b, float min_y,
                        float max_y, float cx, float cz) {
  const float axis[3] = {b[12] - a[12], b[13] - a[13], b[14] - a[14]};
  const float length =
      std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  const float yv[3] = {axis[0] / length, axis[1] / length, axis[2] / length};
  const float ref[3] = {std::fabs(yv[1]) < 0.9f ? 0.f : 1.f,
                        std::fabs(yv[1]) < 0.9f ? 1.f : 0.f, 0.f};
  float xv[3] = {ref[1] * yv[2] - ref[2] * yv[1],
                 ref[2] * yv[0] - ref[0] * yv[2],
                 ref[0] * yv[1] - ref[1] * yv[0]};
  float xl = std::sqrt(xv[0] * xv[0] + xv[1] * xv[1] + xv[2] * xv[2]);
  if (xl < 1e-8f) {
    xv[0] = 1.f;
    xv[1] = xv[2] = 0.f;
    xl = 1.f;
  }
  xv[0] /= xl;
  xv[1] /= xl;
  xv[2] /= xl;
  const float zv[3] = {xv[1] * yv[2] - xv[2] * yv[1],
                       xv[2] * yv[0] - xv[0] * yv[2],
                       xv[0] * yv[1] - xv[1] * yv[0]};
  PartMatrix rotation = eye_matrix(), scale = eye_matrix(), to = eye_matrix(),
             center = eye_matrix();
  for (int i = 0; i < 3; ++i) {
    rotation[i] = xv[i];
    rotation[4 + i] = yv[i];
    rotation[8 + i] = zv[i];
  }
  scale[5] = length / (max_y - min_y);
  to[12] = a[12];
  to[13] = a[13];
  to[14] = a[14];
  center[12] = -cx;
  center[13] = -min_y;
  center[14] = -cz;
  return part_matrix_multiply(
      to, part_matrix_multiply(rotation, part_matrix_multiply(scale, center)));
}
void relink_one(Object *part) {
  auto *rec = record(part);
  if (!rec)
    return;
  for (const auto &vis : rec->links) {
    const PartLink *rule = nullptr;
    for (const auto &candidate : vis.rules)
      if (!candidate.filter ||
          candidate.parent_slot == rec->state.parent_slot) {
        rule = &candidate;
        break;
      }
    if (!rule)
      continue;
    PartMatrix first;
    if (!linked_frame(part, rule->first, first, rule->second != 0))
      continue;
    PartMatrix pose = first;
    if (rule->second) {
      PartMatrix second;
      float min_y = 0, max_y = 0, cx = 0, cz = 0;
      if (!linked_frame(part, rule->second, second) ||
          !mesh_span(vis.resource, min_y, max_y, cx, cz))
        continue;
      const float dx = second[12] - first[12], dy = second[13] - first[13],
                  dz = second[14] - first[14];
      if (dx * dx + dy * dy + dz * dz < 1e-12f)
        continue;
      pose = span_between(first, second, min_y, max_y, cx, cz);
    }
    auto *node = gameref_get(vis.node);
    if (!node)
      continue;
    std::copy(pose.begin(), pose.end(), node->local_matrix);
    node->matrix_override = true;
  }
}
void relink_assembly(Object *part) {
  Object *top = canonical(part);
  std::unordered_set<Object *> seen;
  while (auto *rec = record(top)) {
    if (!seen.insert(top).second)
      return;
    if (!rec->state.parent)
      break;
    top = rec->state.parent;
  }
  seen.clear();
  std::vector<Object *> pending{top};
  for (size_t i = 0; i < pending.size() && i < 512; ++i) {
    if (!seen.insert(pending[i]).second)
      continue;
    relink_one(pending[i]);
    if (auto *rec = record(pending[i]))
      for (const auto &child : rec->children)
        pending.push_back(child.second);
  }
}
void place_child(Object *parent, int slot_id, bool refresh_links = true) {
  auto *p = record(parent);
  if (!p)
    return;
  auto found = p->children.find(slot_id);
  if (found == p->children.end())
    return;
  auto *c = record(found->second);
  if (!c)
    return;
  const PartSlot *mount = nullptr;
  const PartSlot *mate = nullptr;
  for (const auto &s : p->config->slots)
    if (s.id == slot_id)
      mount = &s;
  for (const auto &s : c->config->slots)
    if (s.id == c->state.own_slot)
      mate = &s;
  if (!mount || !mate)
    return;
  PartMatrix parent_frame, child_frame;
  if (!part_slot_matrix(*p->config, posed_slot(*p, *mount), parent_frame) ||
      !part_slot_matrix(*c->config, posed_slot(*c, *mate), child_frame))
    return;
  auto transform = part_matrix_multiply(parent_frame,
                                        part_matrix_inverse_rigid(child_frame));
  const int type = mount->type;
  const bool gear = (type >= 101 && type <= 104) ||
                    (type >= 201 && type <= 204) ||
                    (type >= 301 && type <= 304);
  if (gear && p->dynamic_frames) {
    const int corner = type % 100 - 1;
    // The arm anchor follows travel; the wheel/hub also follows steering.
    const auto &delta = type >= 301   ? p->travel_frame[corner]
                        : type >= 201 ? p->hub_frame[corner]
                                      : p->carrier_frame[corner];
    transform = part_matrix_multiply(delta, transform);
    parent_frame = part_matrix_multiply(delta, parent_frame);
  }
  auto *node = gameref_get(found->second);
  if (!node)
    return;
  c->has_steering_transform = false;
  if (gear && type < 301) {
    const int corner = type % 100 - 1;
    const auto &wheel = p->wheels[corner];
    const float factor =
        wheel.steer_set
            ? wheel.steer
            : (static_cast<size_t>(corner) < p->config->wheel_steering.size()
                   ? p->config->wheel_steering[corner]
                   : 0.f);
    const double angle = p->steering_input * p->config->max_steer;
    double turn = angle * factor;
    const double length =
        p->ackermann != 0 ? p->ackermann : p->config->ackermann_length;
    if (std::fabs(angle) > 1e-7 && length > 0) {
      const double radius = length / std::tan(angle), x = wheel.pos[0];
      turn = (radius <= 0 ? -std::atan2(length, -radius - x)
                          : std::atan2(length, radius + x)) *
             factor;
    }
    if (std::fabs(turn) > 1e-7) {
      PartPose pivot, back, rotation;
      for (int axis = 0; axis < 3; ++axis) {
        pivot.position[axis] = wheel.pos[axis];
        back.position[axis] = -wheel.pos[axis];
      }
      rotation.rotation[0] = static_cast<float>(turn);
      c->steering_transform =
          part_matrix_multiply(part_matrix_multiply(part_pose_matrix(pivot),
                                                    part_pose_matrix(rotation)),
                               part_pose_matrix(back));
      c->has_steering_transform = true;
      transform = part_matrix_multiply(c->steering_transform, transform);
    }
  }
  // Open flap: the hinge lives on the parent slot. Closed is the CFG pose.
  if (mount->has_flap && c->flap) {
    float ax = mount->flap_axis[0], ay = mount->flap_axis[1],
          az = mount->flap_axis[2];
    const float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len > 1e-6f) {
      ax /= len;
      ay /= len;
      az /= len;
      const float c0 = std::cos(mount->flap_hi);
      const float s0 = std::sin(mount->flap_hi);
      const float t0 = 1.f - c0;
      PartMatrix rot{};
      rot[0] = t0 * ax * ax + c0;
      rot[1] = t0 * ax * ay + s0 * az;
      rot[2] = t0 * ax * az - s0 * ay;
      rot[4] = t0 * ax * ay - s0 * az;
      rot[5] = t0 * ay * ay + c0;
      rot[6] = t0 * ay * az + s0 * ax;
      rot[8] = t0 * ax * az + s0 * ay;
      rot[9] = t0 * ay * az - s0 * ax;
      rot[10] = t0 * az * az + c0;
      rot[15] = 1.f;
      PartMatrix to{};
      PartMatrix back{};
      to[0] = to[5] = to[10] = to[15] = 1.f;
      back[0] = back[5] = back[10] = back[15] = 1.f;
      to[12] = mount->flap_pivot[0];
      to[13] = mount->flap_pivot[1];
      to[14] = mount->flap_pivot[2];
      back[12] = -to[12];
      back[13] = -to[13];
      back[14] = -to[14];
      transform = part_matrix_multiply(
          to, part_matrix_multiply(
                  rot, part_matrix_multiply(back, transform)));
    }
  }
  std::copy(transform.begin(), transform.end(), node->local_matrix);
  node->matrix_override = true;
  if (type >= 101 && type <= 104) {
    auto &w = p->wheels[type - 101];
    w.pos[0] = parent_frame[12];
    w.pos[1] = parent_frame[13];
    w.pos[2] = parent_frame[14];
    w.has_pos = true;
    w.live = true;
  } else if (slot_id >= 101 && slot_id < 109) {
    auto &w = p->wheels[slot_id - 101];
    w.pos[0] = parent_frame[12];
    w.pos[1] = parent_frame[13];
    w.pos[2] = parent_frame[14];
    w.has_pos = true;
    w.live = true;
  }
  if (refresh_links)
    relink_assembly(parent);
}
void seed_wheel(Record &rec, int id) {
  if (id < 0 || id >= 8)
    return;
  auto &w = rec.wheels[id];
  w.live = true;
  w.contact = true;
  if (!w.has_pos && static_cast<size_t>(id) < rec.config->wheels.size()) {
    const auto &pose = rec.config->wheels[static_cast<size_t>(id)];
    std::copy(pose.position.begin(), pose.position.end(), w.pos);
    std::copy(pose.rotation.begin(), pose.rotation.end(), w.ypr);
    w.has_pos = true;
    w.has_ypr = true;
  }
}
int wheel_count(Object *self) {
  auto *p = record(self);
  if (!p)
    return 0;
  int n = field_int(self, "wheels");
  if (n <= 0)
    n = static_cast<int>(p->config->wheels.size());
  if (n <= 0 && p->config->native_kind == "car")
    n = 4;
  return std::clamp(n, 0, 8);
}
WheelPhys *wheel_of(Object *wr) {
  if (!wr)
    return nullptr;
  auto ch = wr->fields.find("chassis");
  if (ch == wr->fields.end() || ch->second.tag != ValueTag::Obj)
    return nullptr;
  const int i = field_int(wr, "wheel_id", -1);
  auto *rec = record(ch->second.v.o);
  if (!rec || i < 0 || i >= 8)
    return nullptr;
  return &rec->wheels[static_cast<size_t>(i)];
}
void name_pacejka(WheelPhys &w, int i, float val) {
  if (i == 0)
    w.sliction = val;
  else if (i == 2)
    w.friction = val;
  else if (i == 4)
    w.stiffness = val;
  else if (i == 11)
    w.max_load = val;
  else if (i == 12)
    w.frictn_x = val;
  else if (i == 14)
    w.load_smooth = val;
}
Object *vec3(float x, float y, float z) {
  auto *v = runtime ? runtime->new_object("java.lang.Vector3") : nullptr;
  if (!v)
    return nullptr;
  v->fields["x"] = Value::make_float(x);
  v->fields["y"] = Value::make_float(y);
  v->fields["z"] = Value::make_float(z);
  return v;
}
Object *ypr_obj(float y, float p, float r) {
  auto *v = runtime ? runtime->new_object("java.lang.Ypr") : nullptr;
  if (!v)
    return nullptr;
  v->fields["y"] = Value::make_float(y);
  v->fields["p"] = Value::make_float(p);
  v->fields["r"] = Value::make_float(r);
  return v;
}
bool corner_gear(int type) {
  return (type >= 101 && type <= 104) || (type >= 201 && type <= 204) ||
         (type >= 301 && type <= 304);
}
void assembly_mass_center(Object *chassis, float &mass, float com[3]) {
  mass = 0.f;
  com[0] = com[1] = com[2] = 0.f;
  const PartMatrix inv = part_matrix_inverse_rigid(world_matrix(chassis));
  const auto tree = part_installed_tree(chassis);
  bool physical = false;
  for (const auto &part : tree) {
    auto *rec = record(part.object);
    if (!rec)
      continue;
    for (const auto &body : rec->config->bodies)
      if (body.mass > 0.f)
        physical = true;
  }
  for (const auto &part : tree) {
    auto *rec = record(part.object);
    if (!rec)
      continue;
    const PartMatrix frame =
        part_matrix_multiply(inv, world_matrix(part.object));
    if (!rec->config->bodies.empty()) {
      for (const auto &body : rec->config->bodies) {
        const float b[3] = {body.pose.position[0], body.pose.position[1],
                            body.pose.position[2]};
        float p[3];
        for (int i = 0; i < 3; ++i)
          p[i] = frame[i] * b[0] + frame[4 + i] * b[1] + frame[8 + i] * b[2] +
                 frame[12 + i];
        mass += body.mass;
        for (int i = 0; i < 3; ++i)
          com[i] += p[i] * body.mass;
      }
    } else if (!physical && rec->config->mass > 0.f) {
      mass += rec->config->mass;
      for (int i = 0; i < 3; ++i)
        com[i] += frame[12 + i] * rec->config->mass;
    }
  }
  if (mass > 0.f)
    for (int i = 0; i < 3; ++i)
      com[i] /= mass;
}
// The exe calls Chassis.updatevariables from forceUpdate when the script
// has not set suspend_update. That walk is what fills dynodata and runs
// calcDyno. A part install asks for the same pass once the car is live.
int refresh_depth = 0;
void refresh_chassis(Object *chassis) {
  if (!runtime || !chassis || refresh_depth)
    return;
  chassis = canonical(chassis);
  if (!record(chassis) || field_int(chassis, "suspend_update"))
    return;
  ++refresh_depth;
  runtime->invoke_by_name(chassis->class_name, "updatevariables",
                          {Value::make_obj(chassis)}, false);
  record(chassis)->physics_dirty = true;
  --refresh_depth;
}
// A native car with no script (traffic, fake racers) is finished by its cfg:
// the stock parts, closed, in the create string's colour. A scripted chassis
// builds itself through addStockParts and must not be filled twice.
void install_cfg_stock(Jvm &jvm, Object *part, int depth = 0) {
  if (depth > 48 || records.size() > 2000)
    return;
  auto *current = record(part);
  if (!current)
    return;
  std::string params;
  if (current->colour_set)
    params = "0,0,0,0,0,0," + std::to_string(current->colour_index);
  auto *parent = gameref_get(part);
  for (uint32_t resource : current->config->stock) {
    Object *child = part_create(
        jvm, *index, parent ? static_cast<Object *>(parent->parent) : nullptr,
        resource, "stockpart", params);
    if (!child)
      continue;
    if (!part_install(child, part)) {
      part_destroy(child);
      continue;
    }
    install_cfg_stock(jvm, child, depth + 1);
  }
}

// The selector saves CopyCar, destroys the car and loads the file. That
// round-trip builds a second, thinner car. The live tree is handed back.
struct LiveHold {
  bool armed = false;
  bool sealing = false;
  std::vector<Object *> order;
  size_t next = 0;
};
LiveHold g_live_hold;

void collect_held(Object *part, std::vector<Object *> &out) {
  part = canonical(part);
  auto *rec = record(part);
  if (!rec)
    return;
  out.push_back(part);
  const int back = rec->state.own_slot;
  for (const auto &slot : rec->config->slots) {
    if (slot.id == back)
      continue;
    auto child = rec->children.find(slot.id);
    if (child != rec->children.end())
      collect_held(child->second, out);
  }
}
bool live_held(Object *part) {
  if (!g_live_hold.sealing)
    return false;
  part = canonical(part);
  return std::find(g_live_hold.order.begin(), g_live_hold.order.end(), part) !=
         g_live_hold.order.end();
}
Object *take_held_part(uint32_t resource) {
  if (!g_live_hold.sealing || g_live_hold.next >= g_live_hold.order.size())
    return nullptr;
  Object *have = canonical(g_live_hold.order[g_live_hold.next]);
  auto *rec = record(have);
  if (!rec || rec->state.resource != resource)
    return nullptr;
  ++g_live_hold.next;
  return have;
}
bool lamp_lit(uint32_t flags) {
  const uint32_t bits = flags & 0x13fu;
  if (!bits || (flags & 0x40u) || !runtime)
    return false;
  Value time = runtime->get_static("java.game.GameLogic", "time");
  float sec = 0.f;
  if (time.tag == ValueTag::Float)
    sec = time.v.f;
  else if (time.tag == ValueTag::Int)
    sec = static_cast<float>(time.v.i);
  sec = std::fmod(sec, 86400.f);
  if (sec < 0.f)
    sec += 86400.f;
  // Stock night: outside 6:00..21:00. The selector starts at midnight and
  // LIGHTS ON/OFF adds twelve hours.
  const bool night = sec < 21600.f || sec >= 75600.f;
  return (bits & 4u) != 0 && night;
}
} // namespace

bool part_lamp_lit(uint32_t flags) { return lamp_lit(flags); }

Object *part_create(Jvm &jvm, RpkIndex &resources, Object *parent,
                    uint32_t resource, const std::string &alias,
                    const std::string &params) {
  if (Object *kept = take_held_part(resource))
    return kept;
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
    // `script java.game.parts.accessories.stock_Battery_black` is the
    // class name. stem() would cut it at the last dot.
    const bool fqn = script.find('/') == std::string::npos &&
                     script.find('.') != std::string::npos &&
                     (script.size() < 6 ||
                      script.compare(script.size() - 6, 6, ".class") != 0);
    const std::string lookup =
        fqn ? script : std::filesystem::path(script).stem().string();
    if (!jvm.find_class(lookup) &&
        !jvm.load_class(
            fqn ? script
                : (std::filesystem::path(resources.game_root()) / script)
                      .string()))
      return nullptr;
    auto *cls = jvm.find_class(lookup);
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
  float pos[3] = {}, ypr[3] = {};
  int got = 0;
  if (!params.empty()) {
    int colour = 0;
    got = std::sscanf(params.c_str(), "%f,%f,%f,%f,%f,%f,%d", &pos[0], &pos[1],
                      &pos[2], &ypr[0], &ypr[1], &ypr[2], &colour);
    if (got >= 7) {
      rec.colour_set = true;
      rec.colour_index = colour;
    }
  }
  records.emplace(part, std::move(rec));
  auto *node = gameref_upsert(part, GameRefKind::GameType);
  node->type_id = resource;
  node->script = part;
  node->class_name = class_name;
  node->alias = alias;
  if (got >= 6) {
    std::copy(pos, pos + 3, node->pos);
    std::copy(ypr, ypr + 3, node->ypr);
  }
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
  if (script.empty() && cfg->native_kind == "car")
    install_cfg_stock(jvm, part);
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
    if (auto *chassis = car(child)) {
      // A rim put back on the car still carries its tyre. Register the
      // whole assembly, not only the part that was clicked into the slot.
      for (const auto &installed : part_installed_tree(child))
        runtime->invoke_by_name(
            chassis->class_name, "addpart",
            {Value::make_obj(chassis), Value::make_obj(installed.object)},
            false);
      refresh_chassis(chassis);
    }
  relink_assembly(child);
  return true;
}
// `filter <category> <mode>`: 1 engine, 2 body (the chassis), 3 running
// gear. Mode 2 hides that category's meshes; 0 and 1 show them again.
void apply_part_filter(Object *part, int category, int mode) {
  auto *rec = record(part);
  if (!rec || !rec->config)
    return;
  if (category == 0 || rec->config->category == category) {
    const bool hide = mode == 2;
    for (Object *render : rec->renders)
      if (auto *node = gameref_get(render))
        if (node->type_id)
          node->empty = hide;
  }
  for (const auto &child : rec->children)
    apply_part_filter(child.second, category, mode);
}
bool part_command(Object *part, const std::string &command) {
  part = canonical(part);
  auto *rec = record(part);
  if (!rec)
    return false;
  std::istringstream input(command);
  std::string verb;
  input >> verb;
  if (verb == "setsteer" && rec->config->native_kind == "car") {
    float value = 0;
    if (!(input >> value) || !std::isfinite(value))
      return false;
    rec->steering_input = std::clamp(value, -1.f, 1.f);
    for (const auto &slot : rec->config->slots)
      if (slot.type >= 101 && slot.type <= 304)
        place_child(part, slot.id, false);
    relink_assembly(part);
    return true;
  }
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
    Object *chassis = nullptr;
    if (runtime)
      if (chassis = car(part))
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
    refresh_chassis(chassis);
    return true;
  }
  if (verb == "reset" && rec->config->native_kind == "car") {
    // Stock reset zeroes the velocities and the steer, then sleeps the body.
    // It does not move the chassis. start is what releases it.
    rec->physics_suspended = true;
    rec->physics_pending_placement = false;
    rec->physics_released = false;
    rec->steering_input = 0.f;
    rec->dynamics.body.velocity = {};
    rec->dynamics.body.momentum = {};
    part_park_chassis(part, false);
    for (const auto &slot : rec->config->slots)
      if (slot.type >= 101 && slot.type <= 304)
        place_child(part, slot.id, false);
    relink_assembly(part);
    return true;
  }
  if (verb == "start" && rec->config->native_kind == "car") {
    rec->physics_suspended = false;
    rec->physics_released = true;
    return true;
  }
  if (verb == "suspend" && rec->config->native_kind == "car") {
    rec->physics_suspended = true;
    rec->physics_released = false;
    return true;
  }
  if (verb == "wakeup" && rec->config->native_kind == "car") {
    rec->physics_suspended = false;
    return true;
  }
  if (verb == "flap_toggle") {
    rec->flap ^= 1;
    if (rec->state.parent)
      place_child(rec->state.parent, rec->state.parent_slot);
    return true;
  }
  if (verb == "filter") {
    int category = 0, mode = 0;
    if (input >> category >> mode)
      apply_part_filter(part, category, mode);
    return true;
  }
  return false;
}
Object *part_render_owner(void *key) {
  for (int depth = 0; key && depth < 32; ++depth) {
    if (record(static_cast<Object *>(key)))
      return canonical(static_cast<Object *>(key));
    auto *node = gameref_get(key);
    if (!node)
      return nullptr;
    key = node->parent;
  }
  return nullptr;
}
void part_note_live_save(const std::string &path) {
  const auto name = std::filesystem::path(path).filename().string();
  std::string lower = name;
  for (char &c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower == "copycar")
    g_live_hold.armed = true;
}
void part_release_live_hold() {
  g_live_hold.armed = false;
  g_live_hold.sealing = false;
  g_live_hold.order.clear();
  g_live_hold.next = 0;
}
bool part_destroy(Object *part) {
  part = canonical(part);
  auto *rec = record(part);
  if (!rec)
    return false;
  if (g_live_hold.armed && !rec->state.parent &&
      rec->config->native_kind == "car") {
    g_live_hold.armed = false;
    g_live_hold.sealing = true;
    g_live_hold.order.clear();
    g_live_hold.next = 0;
    collect_held(part, g_live_hold.order);
    return true;
  }
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
  release_side(*rec);
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

namespace {
bool dynamics_configure(Object *chassis, Record &rec, const PartMatrix &frame) {
  auto unavailable = [&](const char *reason) {
    if (!rec.physics_reported) {
      std::printf("[phys] unavailable type=0x%x: %s\n", rec.state.resource,
                  reason);
      std::fflush(stdout);
      rec.physics_reported = true;
    }
    return false;
  };
  float mass = 0, com[3]{};
  assembly_mass_center(chassis, mass, com);
  if (!(mass > 0))
    return unavailable("missing positive CFG body mass");
  const uint32_t render = rec.render_set ? rec.render_id : rec.config->render;
  auto model = index->model_for_resource(RpkIndex::pack_from_full(render),
                                         render & 0xffff);
  if (!model.ok)
    return unavailable("missing chassis render");
  std::replace(model.source.begin(), model.source.end(), '\\', '/');
  auto path = std::filesystem::path(index->game_root()) / model.source;
  auto ext = path.extension().string();
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext == ".scy") {
    auto scx = path;
    scx.replace_extension(".scx");
    if (std::filesystem::exists(scx))
      path = scx;
  }
  auto geometry = load_scx_geometry(path.string());
  if (!geometry.info.ok)
    return unavailable("unreadable chassis geometry");
  const double scale = geometry.position_scale;
  physics::Vec3 lo{geometry.info.bbox_min[0] * scale,
                   geometry.info.bbox_min[1] * scale,
                   geometry.info.bbox_min[2] * scale};
  physics::Vec3 hi{geometry.info.bbox_max[0] * scale,
                   geometry.info.bbox_max[1] * scale,
                   geometry.info.bbox_max[2] * scale};
  const auto velocity = rec.dynamics.body.velocity,
             momentum = rec.dynamics.body.momentum;
  rec.dynamics.body.configure(mass, lo, hi, {com[0], com[1], com[2]});
  const bool held = rec.physics_ready;
  rec.dynamics.body.set_frame(frame);
  if (held) {
    rec.dynamics.body.velocity = velocity;
    rec.dynamics.body.momentum = momentum;
  } else {
    rec.dynamics.release_wheels();
    rec.dynamics.reset_clock();
  }
  rec.physics_ready = true;
  rec.physics_dirty = false;
  rec.physics_parent = gameref_get(chassis)->parent;
  if (!rec.physics_reported) {
    std::printf("[phys] dynamic type=0x%x mass=%.2f fixed=240Hz\n",
                rec.state.resource, mass);
    std::fflush(stdout);
    rec.physics_reported = true;
  }
  return true;
}
void dynamics_wheels(Record &rec) {
  for (size_t i = 0; i < 4; ++i) {
    auto &cfg = rec.dynamics.wheels[i];
    const auto &live = rec.wheels[i];
    cfg.enabled = false;
    if (i >= rec.config->wheels.size())
      continue;
    const PartSlot *mount = nullptr;
    for (const auto &slot : rec.config->slots)
      if (slot.type == 101 + static_cast<int>(i) &&
          rec.children.count(slot.id)) {
        mount = &slot;
        break;
      }
    if (!mount)
      continue;
    PartMatrix frame;
    if (!part_slot_matrix(*rec.config, posed_slot(rec, *mount), frame))
      continue;
    rec.base_wheel_center[i] = {frame[12], frame[13], frame[14]};
    const auto fallback = i < rec.config->wheel_springs.size()
                              ? rec.config->wheel_springs[i]
                              : PartWheelSpring{};
    // CFG wheel is the physical centre. setSlotPos edits the attachment
    // hardware frame; adding that offset here incorrectly widens the track.
    // Explicit WheelRef.setPos remains an authored physical override.
    const auto &physical = rec.config->wheels[i].position;
    cfg.center =
        live.authored_position
            ? physics::Vec3{live.input_position[0], live.input_position[1],
                            live.input_position[2]}
            : physics::Vec3{physical[0], physical[1], physical[2]};
    PartPose alignment;
    if (live.has_ypr)
      std::copy(live.ypr, live.ypr + 3, alignment.rotation.begin());
    auto orient = part_pose_matrix(alignment);
    cfg.down = {-orient[4], -orient[5], -orient[6]};
    cfg.radius = live.radius > 0 ? live.radius : fallback.radius;
    cfg.rest = live.rest;
    cfg.anchor = fallback.base + fallback.travel;
    cfg.minimum = std::max(0.f, live.min_len);
    cfg.maximum = live.max_len > 0 ? live.max_len : live.rest;
    cfg.spring = live.force;
    cfg.bump_damping = live.damp_bound;
    cfg.rebound_damping = live.damp_rebound;
    cfg.unsprung = fallback.mass;
    cfg.arm_length = 0;
    cfg.has_hub = false;
    cfg.hub = {};
    cfg.hub_alignment = {};
    if (live.has_arm && live.opp < 0 && live.arm[0] > 0) {
      cfg.arm_length = live.arm[0];
      cfg.arm_pivot = cfg.center + physics::Vec3{live.arm[1], live.arm[2],
                                                 live.arm[3]};
      cfg.arm_axis = {live.arm[4], live.arm[5], live.arm[6]};
      if (live.has_hub) {
        cfg.has_hub = true;
        for (int k = 0; k < 10; ++k)
          cfg.hub[static_cast<size_t>(k)] = live.hub[k];
        const auto &ypr = live.has_ypr ? live.ypr
                                       : rec.config->wheels[i].rotation.data();
        cfg.hub_alignment =
            (physics::Quaternion{0, std::sin(ypr[0] * .5), 0,
                                 std::cos(ypr[0] * .5)} *
             physics::Quaternion{std::sin(ypr[1] * .5), 0, 0,
                                 std::cos(ypr[1] * .5)} *
             physics::Quaternion{0, 0, std::sin(ypr[2] * .5),
                                 std::cos(ypr[2] * .5)})
                .normalized();
      }
    }
    cfg.enabled = cfg.radius > 0 && cfg.rest > 0 &&
                  cfg.maximum >= cfg.minimum && cfg.spring > 0;
  }
}
void pose_wheels(Object *chassis, Record &rec);
void finish_dynamics_sync(Object *chassis, Record &rec);
void dynamics_sync(Object *chassis, Record &rec,
                   const PartMatrix &parent_frame) {
  auto *node = gameref_get(chassis);
  const auto world = rec.dynamics.body.frame();
  const auto local =
      part_matrix_multiply(part_matrix_inverse_rigid(parent_frame), world);
  std::copy(local.begin(), local.end(), node->local_matrix);
  node->matrix_override = true;
  std::copy(local.begin() + 12, local.begin() + 15, node->pos);
  const double pitch = std::asin(std::clamp(-double(local[9]), -1.0, 1.0));
  node->ypr[1] = static_cast<float>(pitch);
  if (std::fabs(std::cos(pitch)) > 1e-6) {
    node->ypr[0] = std::atan2(local[8], local[10]);
    node->ypr[2] = std::atan2(local[1], local[5]);
  } else {
    node->ypr[0] = std::atan2(-local[2], local[0]);
    node->ypr[2] = 0;
  }
  auto velocity = rec.dynamics.body.velocity;
  node->vel[0] = static_cast<float>(parent_frame[0] * velocity.x +
                                    parent_frame[1] * velocity.y +
                                    parent_frame[2] * velocity.z);
  node->vel[1] = static_cast<float>(parent_frame[4] * velocity.x +
                                    parent_frame[5] * velocity.y +
                                    parent_frame[6] * velocity.z);
  node->vel[2] = static_cast<float>(parent_frame[8] * velocity.x +
                                    parent_frame[9] * velocity.y +
                                    parent_frame[10] * velocity.z);
  rec.published_frame = local;
  pose_wheels(chassis, rec);
  finish_dynamics_sync(chassis, rec);
}
void pose_wheels(Object *chassis, Record &rec) {
  rec.dynamic_frames = true;
  for (size_t i = 0; i < 4; ++i) {
    const auto &cfg = rec.dynamics.wheels[i];
    const auto &state = rec.dynamics.state[i];
    rec.travel_frame[i] = rec.carrier_frame[i] = rec.hub_frame[i] =
        eye_matrix();
    rec.geometry_solved[i] = false;
    if (!cfg.enabled)
      continue;
    const physics::Vec3 seat =
        cfg.center - cfg.down.normalized() * cfg.anchor_length();
    const physics::Vec3 center = seat + cfg.down.normalized() * state.length;
    const auto base = rec.base_wheel_center[i];
    const auto delta = center - base;
    rec.travel_frame[i][12] = static_cast<float>(delta.x);
    rec.travel_frame[i][13] = static_cast<float>(delta.y);
    rec.travel_frame[i][14] = static_cast<float>(delta.z);
    PartPose authored, live;
    authored.rotation = rec.config->wheels[i].rotation;
    if (rec.wheels[i].has_ypr)
      std::copy(rec.wheels[i].ypr, rec.wheels[i].ypr + 3,
                live.rotation.begin());
    else
      live.rotation = authored.rotation;
    auto rotation = part_matrix_multiply(
        part_pose_matrix(live),
        part_matrix_inverse_rigid(part_pose_matrix(authored)));
    PartPose from, to;
    from.position = {float(-base.x), float(-base.y), float(-base.z)};
    to.position = {float(center.x), float(center.y), float(center.z)};
    rec.carrier_frame[i] = part_matrix_multiply(
        part_matrix_multiply(part_pose_matrix(to), rotation),
        part_pose_matrix(from));
    rec.hub_frame[i] = rec.carrier_frame[i];
    if (rec.wheels[i].has_arm && rec.wheels[i].has_hub &&
        rec.wheels[i].opp < 0) {
      physics::CornerGeometry geometry;
      geometry.reference = cfg.center;
      std::copy(rec.wheels[i].arm, rec.wheels[i].arm + 7, geometry.arm.begin());
      std::copy(rec.wheels[i].hub, rec.wheels[i].hub + 10,
                geometry.hub.begin());
      const auto &ypr = live.rotation;
      geometry.alignment = (physics::Quaternion{0, std::sin(ypr[0] * .5), 0,
                                                std::cos(ypr[0] * .5)} *
                            physics::Quaternion{std::sin(ypr[1] * .5), 0, 0,
                                                std::cos(ypr[1] * .5)} *
                            physics::Quaternion{0, 0, std::sin(ypr[2] * .5),
                                                std::cos(ypr[2] * .5)})
                               .normalized();
      physics::CornerFrames frames;
      if (physics::solve_corner_geometry(geometry, center, frames)) {
        const auto inverse =
            part_matrix_inverse_rigid(part_pose_matrix(rec.config->wheels[i]));
        rec.travel_frame[i] = part_matrix_multiply(
            physics::corner_matrix(frames.arm_origin, frames.arm_rotation),
            inverse);
        rec.hub_frame[i] = rec.carrier_frame[i] = part_matrix_multiply(
            physics::corner_matrix(frames.hub_origin, frames.hub_rotation),
            inverse);
        rec.geometry_solved[i] = true;
      }
    }
    rec.wheels[i].contact = state.contact;
  }
  for (const auto &slot : rec.config->slots)
    if (corner_gear(slot.type))
      place_child(chassis, slot.id, false);
  relink_assembly(chassis);
}
void finish_dynamics_sync(Object *chassis, Record &rec) {
  auto *node = gameref_get(chassis);
  int contacts = 0;
  double support = 0;
  for (const auto &state : rec.dynamics.state) {
    contacts += state.contact;
    support += state.force;
  }
  if (contacts != rec.reported_contacts && rec.physics_status_lines < 8) {
    std::printf("[phys] contacts=%d/4 y=%.3f vy=%.3f support=%.1fN\n", contacts,
                node->pos[1], rec.dynamics.body.velocity.y, support);
    std::fflush(stdout);
    rec.reported_contacts = contacts;
    ++rec.physics_status_lines;
  }
}
// setMatrix of a car puts the midpoint of the four static tyre contacts on
// the given point and writes the loaded spring length. The body stays asleep.
void park_chassis(Object *chassis, bool shift) {
  chassis = canonical(chassis);
  auto *rec = record(chassis);
  auto *node = chassis ? gameref_get(chassis) : nullptr;
  if (!rec || !node || rec->config->native_kind != "car")
    return;
  dynamics_wheels(*rec);
  float mass = 0.f, com[3]{};
  assembly_mass_center(chassis, mass, com);
  int count = 0;
  for (const auto &wheel : rec->dynamics.wheels)
    if (wheel.enabled)
      ++count;
  if (!count || !(mass > 0.f))
    return;
  const float share = mass / static_cast<float>(count);
  physics::Vec3 contact;
  int used = 0;
  for (size_t i = 0; i < 4; ++i) {
    auto &cfg = rec->dynamics.wheels[i];
    auto &state = rec->dynamics.state[i];
    if (!cfg.enabled)
      continue;
    float length = static_cast<float>(cfg.rest);
    if (!part_parked_spring_length(static_cast<float>(cfg.rest),
                                   static_cast<float>(cfg.minimum),
                                   static_cast<float>(cfg.maximum),
                                   static_cast<float>(cfg.spring), share,
                                   length))
      length = static_cast<float>(cfg.rest);
    state.length = length;
    state.speed = 0;
    state.seeded = true;
    state.contact = false;
    state.force = 0;
    const auto down = cfg.down.normalized();
    const auto center = cfg.center + down * (state.length - cfg.anchor_length());
    contact += center + down * cfg.radius;
    ++used;
  }
  if (!used)
    return;
  contact = contact * (1.0 / used);
  if (shift) {
    PartPose pose;
    std::copy(node->ypr, node->ypr + 3, pose.rotation.begin());
    const auto turned = part_pose_matrix(pose);
    const double cx = turned[0] * contact.x + turned[4] * contact.y +
                      turned[8] * contact.z;
    const double cy = turned[1] * contact.x + turned[5] * contact.y +
                      turned[9] * contact.z;
    const double cz = turned[2] * contact.x + turned[6] * contact.y +
                      turned[10] * contact.z;
    node->pos[0] -= static_cast<float>(cx);
    node->pos[1] -= static_cast<float>(cy);
    node->pos[2] -= static_cast<float>(cz);
    node->matrix_override = false;
  }
  rec->physics_suspended = true;
  rec->physics_pending_placement = false;
  rec->physics_released = false;
  pose_wheels(chassis, *rec);
}
} // namespace
void part_park_chassis(Object *car, bool shift) { park_chassis(car, shift); }
void part_physics_advance(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0)
    throw std::invalid_argument("invalid vehicle elapsed time");
  scene_instances_prepare_physics();
  for (auto &entry : records) {
    Object *chassis = entry.first;
    auto &rec = entry.second;
    if (rec.config->native_kind != "car" || rec.state.parent ||
        (rec.physics_suspended && !rec.physics_pending_placement))
      continue;
    auto *node = gameref_get(chassis);
    if (!node)
      continue;
    PartMatrix frame, parent = eye_matrix();
    if (!scene_instances_script_matrix(chassis, frame.data()))
      continue;
    if (node->parent &&
        !scene_instances_script_matrix(node->parent, parent.data()))
      continue;
    const auto local = node_local(chassis);
    bool external = node->parent != rec.physics_parent;
    for (size_t i = 0; i < 16; ++i)
      external =
          external || std::fabs(local[i] - rec.published_frame[i]) > 1e-5f;
    if (external)
      rec.physics_ready = false;
    if (!rec.physics_ready || rec.physics_dirty)
      if (!dynamics_configure(chassis, rec, frame))
        continue;
    dynamics_wheels(rec);
    // An unreleased chassis (catalog, before start) may drop onto a floor.
    // With no floor the step only applies gravity, and the car leaves the shot.
    if (!rec.physics_released && rec.physics_pending_placement) {
      SceneFloorHit probe;
      if (!scene_instances_sample_floor(frame[12], frame[14], frame[13] + 4.f,
                                        chassis, probe))
        continue;
    }
    const auto terrain = [&](double x, double z, double ceiling,
                             physics::GroundHit &ground) {
      SceneFloorHit hit;
      if (!scene_instances_sample_floor(
              static_cast<float>(x), static_cast<float>(z),
              static_cast<float>(ceiling), chassis, hit))
        return false;
      ground.height = hit.height;
      ground.normal = {hit.normal[0], hit.normal[1], hit.normal[2]};
      return true;
    };
    const auto steps = rec.dynamics.advance(seconds, terrain);
    if (steps)
      dynamics_sync(chassis, rec, parent);
    else
      rec.published_frame = local;
    if (steps && rec.physics_pending_placement) {
      if (!rec.physics_suspended)
        rec.physics_pending_placement = false;
      else {
        bool supported = true;
        double force = 0;
        for (size_t i = 0; i < 4; ++i) {
          if (rec.dynamics.wheels[i].enabled)
            supported = supported && rec.dynamics.state[i].contact;
          force += rec.dynamics.state[i].force;
        }
        const bool quiet = supported &&
                           rec.dynamics.body.velocity.length() < .02 &&
                           rec.dynamics.body.omega().length() < .02 &&
                           std::fabs(force - rec.dynamics.body.mass * 9.81) <
                               rec.dynamics.body.mass * 9.81 * .02;
        rec.physics_quiet_ticks = quiet ? rec.physics_quiet_ticks + steps : 0;
        if (rec.physics_quiet_ticks >= 60) {
          rec.physics_pending_placement = false;
          rec.dynamics.body.velocity = {};
          rec.dynamics.body.momentum = {};
        }
      }
    }
  }
}
bool part_physics_state(Object *key, PartPhysicsState &state) {
  auto *rec = record(as_part(key));
  if (!rec)
    return false;
  PartMatrix world;
  state = {};
  state.active = rec->physics_ready && !rec->physics_suspended &&
                 scene_instances_script_matrix(as_part(key), world.data());
  if (!rec->physics_ready)
    return true;
  state.mass = rec->dynamics.body.mass;
  state.height = rec->dynamics.body.frame()[13];
  state.vertical_speed = rec->dynamics.body.velocity.y;
  state.ticks = rec->dynamics.ticks;
  state.backlog = rec->dynamics.backlog();
  for (size_t i = 0; i < 4; ++i) {
    state.length[i] = rec->dynamics.state[i].length;
    state.force[i] = rec->dynamics.state[i].force;
    state.contact[i] = rec->dynamics.state[i].contact;
    state.geometry_solved[i] = rec->geometry_solved[i];
    const auto &center = rec->dynamics.wheels[i].center;
    state.wheel_center[i] = {center.x, center.y, center.z};
  }
  return true;
}

void *part_shadow_source(void *render, float *frame) {
  for (int guard = 0; render && guard < 64; ++guard) {
    if (auto *rec = record(static_cast<Object *>(render));
        rec && rec->config->native_kind == "car")
      return scene_instances_script_matrix(render, frame) ? render : nullptr;
    auto *node = gameref_get(render);
    render = node ? node->parent : nullptr;
  }
  return nullptr;
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
    const float xs[2] = {geo.info.bbox_min[0] * scale,
                         geo.info.bbox_max[0] * scale};
    const float ys[2] = {geo.info.bbox_min[1] * scale,
                         geo.info.bbox_max[1] * scale};
    const float zs[2] = {geo.info.bbox_min[2] * scale,
                         geo.info.bbox_max[2] * scale};
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
  // Same bracket as Vehicle's constructor: stock parts install while the
  // chassis refresh is suspended, then one forceUpdate runs the dyno.
  part->fields["suspend_update"] = Value::make_int(1);
  stock_parts(jvm, part, descriptor);
  part->fields["suspend_update"] = Value::make_int(0);
  jvm.invoke_by_name(part->class_name, "forceUpdate", {Value::make_obj(part)},
                     false);
  auto *vehicle = jvm.new_object("java.game.Vehicle");
  vehicle->fields["chassis"] = Value::make_obj(part);
  gameref_bind_alias(vehicle, part);
  return vehicle;
}

void register_part_natives(Jvm &jvm, RpkIndex &resources) {
  runtime = &jvm;
  records.clear();
  sfx_items.clear();
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
    if (query == 52) {
      // GII_CAR_DRIVETYPE. Chassis bits DT_FWD=1, DT_RWD=2 become the
      // CarInfo codes: 0 none, 1 all, 2 front, 3 rear, 4 cross.
      Object *chassis = canonical(self);
      if (chassis && chassis->fields.find("drive_type") == chassis->fields.end())
        if (auto it = chassis->fields.find("chassis");
            it != chassis->fields.end() && it->second.tag == ValueTag::Obj)
          chassis = it->second.v.o;
      int bits = 0;
      if (chassis) {
        auto it = chassis->fields.find("drive_type");
        if (it != chassis->fields.end()) {
          if (it->second.tag == ValueTag::Int)
            bits = it->second.v.i;
          else if (it->second.tag == ValueTag::Float)
            bits = static_cast<int>(it->second.v.f);
        }
      }
      const int fwd = bits & 1;
      const int rwd = bits & 2;
      if (!fwd && !rwd)
        return Value::make_int(bits ? 4 : 0);
      if (fwd && rwd)
        return Value::make_int(1);
      return Value::make_int(fwd ? 2 : 3);
    }
    if (query == 24 || query == 43) {
      auto *parent = gameref_get(state->parent);
      return Value::make_int(parent ? parent->live_id : 0);
    }
    if (query == 41) {
      // GII_GETOUT_OK. The chassis stays. A rim leaves with the tyre still
      // mounted: children are part of the assembly, not a reason to refuse.
      if (!p || p->config->native_kind == "car")
        return Value::make_int(-1);
      return Value::make_int(0);
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
      // Stock answers the farthest corner from the origin, in centimetres
      // (FUN_005008c0). The full axis length sat the catalog camera too far,
      // so the part looked small in the cell.
      float lo[3] = {1e9f, 1e9f, 1e9f};
      float hi[3] = {-1e9f, -1e9f, -1e9f};
      bool any = false;
      for (const auto &instance :
           scene_instances_collect_root(canonical(self))) {
        if (!instance.geometry)
          continue;
        const auto &mesh = instance.geometry->info;
        const float s = instance.geometry->position_scale;
        any = true;
        for (int i = 0; i < 3; ++i) {
          lo[i] = std::min(lo[i], mesh.bbox_min[i] * s);
          hi[i] = std::max(hi[i], mesh.bbox_max[i] * s);
        }
      }
      if (!any)
        return Value::make_int(100);
      float near2 = 0.f, far2 = 0.f;
      for (int i = 0; i < 3; ++i) {
        const float l = std::min(lo[i], 0.f);
        const float h = std::max(hi[i], 0.f);
        near2 += l * l;
        far2 += h * h;
      }
      const float r = std::sqrt(std::max(near2, far2));
      int cm = 1 + static_cast<int>(r * 100.f);
      if (cm < 1)
        cm = 1;
      if (cm > 10000)
        cm = 10000;
      return Value::make_int(cm);
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
  auto dyno_fields = [](Object *self) {
    std::unordered_map<std::string, float> fields;
    if (!self)
      return fields;
    for (const auto &entry : self->fields) {
      if (entry.second.tag == ValueTag::Float)
        fields[entry.first] = entry.second.v.f;
      else if (entry.second.tag == ValueTag::Int)
        fields[entry.first] = static_cast<float>(entry.second.v.i);
    }
    return fields;
  };
  auto write_dyno = [](Object *self, const char *name, float value) {
    if (self)
      self->fields[name] = Value::make_float(value);
  };
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
        dyno_forget(self);
        if (self)
          self->fields.erase("_host_native_allocated");
        return Value::make_void();
      });
  jvm.natives().register_method(
      "java.game.parts.DynoData", "calcDyno", "(F)F",
      [dyno_fields, write_dyno](Object *self, const std::vector<Value> &args) {
        const auto fields = dyno_fields(self);
        DynoStats stats;
        if (!dyno_calculate(self, fields, scalar(args, 0), &stats))
          return Value::make_float(0);
        write_dyno(self, "Displacement", stats.displacement);
        write_dyno(self, "Compression", stats.compression);
        write_dyno(self, "maxTorque", stats.max_torque);
        write_dyno(self, "RPM_maxTorque", stats.torque_rpm);
        write_dyno(self, "maxHP", stats.max_hp);
        write_dyno(self, "RPM_maxHP", stats.hp_rpm);
        const auto rpm = fields.find("maxRPM");
        if (rpm == fields.end() || !(rpm->second > 0.f))
          write_dyno(self, "maxRPM", stats.maximum_rpm);
        return Value::make_float(0);
      });
  jvm.natives().register_method(
      "java.game.parts.DynoData", "getTorque", "(FF)F",
      [](Object *self, const std::vector<Value> &args) {
        return Value::make_float(
            dyno_sample(self, scalar(args, 0), scalar(args, 1), false));
      });
  jvm.natives().register_method(
      "java.game.parts.DynoData", "getHP", "(FF)F",
      [](Object *self, const std::vector<Value> &args) {
        return Value::make_float(
            dyno_sample(self, scalar(args, 0), scalar(args, 1), true));
      });
  jvm.natives().register_method(
      "java.game.parts.bodypart.Chassis", "getTorque", "(FF)F",
      [](Object *self, const std::vector<Value> &args) {
        Object *dyno = nullptr;
        auto take = [&](Object *part) {
          if (!part || dyno)
            return;
          for (const char *name : {"dynodata", "dyno"}) {
            auto it = part->fields.find(name);
            if (it != part->fields.end() && it->second.tag == ValueTag::Obj &&
                it->second.v.o) {
              dyno = it->second.v.o;
              return;
            }
          }
        };
        std::vector<Object *> pending{self};
        for (size_t i = 0; i < pending.size() && i < 64 && !dyno; ++i) {
          Object *part = pending[i];
          if (i)
            take(part);
          if (auto *rec = record(part))
            for (const auto &child : rec->children)
              pending.push_back(child.second);
        }
        if (!dyno)
          take(self);
        return Value::make_float(
            dyno ? dyno_sample(dyno, scalar(args, 0), scalar(args, 1), false)
                 : 0.f);
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
    if (live_held(s))
      return Value::make_int(old);
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
    auto *p = record(s);
    if (!p)
      return Value::make_void();
    const int slot = number(a, 0);
    const int status = number(a, 1);
    // 0 clears. 1 blocks the slot. 2 blocks it and hides the mate.
    if (status <= 0)
      p->slot_status.erase(slot);
    else
      p->slot_status[slot] = status >= 2 ? 2 : 1;
    return Value::make_void();
  });
  bind("isSlotDisabled", "(I)I", [](Object *s, const std::vector<Value> &a) {
    auto *p = record(s);
    if (!p)
      return Value::make_int(0);
    auto it = p->slot_status.find(number(a, 0));
    return Value::make_int(it == p->slot_status.end() ? 0 : it->second);
  });
  bind("getSlotIndex", "(I)I", [](Object *s, const std::vector<Value> &a) {
    auto *p = record(s);
    if (!p)
      return Value::make_int(0);
    const int id = number(a, 0);
    for (size_t i = 0; i < p->config->slots.size(); ++i)
      if (p->config->slots[i].id == id)
        return Value::make_int(static_cast<int>(i));
    return Value::make_int(0);
  });
  bind("getSlotDamage", "(I)Ljava.lang.String;",
       [&jvm](Object *s, const std::vector<Value> &a) {
         auto *p = record(s);
         const int i = number(a, 0);
         std::string text;
         if (p && i >= 0 && static_cast<size_t>(i) < p->config->slots.size()) {
           auto it = p->slot_damage.find(i);
           if (it != p->slot_damage.end())
             text = it->second;
         }
         return Value::make_obj(jvm.new_string(text));
       });
  bind("setSlotDamage", "(ILjava.lang.String;)V",
       [](Object *s, const std::vector<Value> &a) {
         auto *p = record(s);
         const int i = number(a, 0);
         if (!p || i < 0 || static_cast<size_t>(i) >= p->config->slots.size())
           return Value::make_void();
         auto *data = object(a, 1);
         const std::string text = data ? data->str : "";
         const int slot = p->config->slots[static_cast<size_t>(i)].id;
         if (text.size() <= 1) {
           p->slot_damage.erase(i);
           return Value::make_void();
         }
         float v[6]{};
         std::sscanf(text.c_str(), "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2],
                     &v[3], &v[4], &v[5]);
         char buf[96];
         std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", v[0],
                       v[1], v[2], v[3], v[4], v[5]);
         p->slot_damage[i] = buf;
         auto &ov = p->slot_pose[slot];
         ov.pos = ov.ypr = true;
         std::copy(v, v + 3, ov.p);
         std::copy(v + 3, v + 6, ov.y);
         if (p->state.parent && slot == p->state.own_slot)
           place_child(p->state.parent, p->state.parent_slot);
         else
           place_child(canonical(s), slot);
         return Value::make_void();
       });
  bind("setSlotPos", "(ILjava.lang.Vector3;Ljava.lang.Ypr;)V",
       [](Object *s, const std::vector<Value> &a) {
         auto *p = record(s);
         if (!p)
           return Value::make_void();
         const int slot = number(a, 0);
         auto *pos = object(a, 1);
         auto *ori = object(a, 2);
         // Both null restores the cfg pose (hoods and doors do this).
         if (!pos && !ori)
           p->slot_pose.erase(slot);
         else {
           auto &ov = p->slot_pose[slot];
           if (pos) {
             ov.p[0] = comp(pos, "x");
             ov.p[1] = comp(pos, "y");
             ov.p[2] = comp(pos, "z");
             ov.pos = true;
           }
           if (ori) {
             ov.y[0] = comp(ori, "y");
             ov.y[1] = comp(ori, "p");
             ov.y[2] = comp(ori, "r");
             ov.ypr = true;
           }
         }
         if (p->state.parent && slot == p->state.own_slot)
           place_child(p->state.parent, p->state.parent_slot);
         else
           place_child(canonical(s), slot);
         return Value::make_void();
       });
  bind("getMesh", "()I", [](Object *s, const std::vector<Value> &) {
    auto *p = record(s);
    if (!p)
      return Value::make_int(0);
    return Value::make_int(p->mesh_set ? static_cast<int>(p->mesh_id)
                                       : static_cast<int>(p->config->mesh));
  });
  bind("setMesh", "(I)I", [](Object *s, const std::vector<Value> &a) {
    if (live_held(s))
      return Value::make_int(1);
    if (auto *p = record(s)) {
      p->mesh_set = true;
      p->mesh_id = static_cast<uint32_t>(number(a, 0));
      retarget(*p, p->mesh_id);
    }
    return Value::make_int(1);
  });
  bind("getRenderType", "()I", [](Object *s, const std::vector<Value> &) {
    auto *p = record(s);
    if (!p)
      return Value::make_int(0);
    return Value::make_int(p->render_set ? static_cast<int>(p->render_id)
                                         : static_cast<int>(p->config->render));
  });
  bind("setRenderType", "(I)I", [](Object *s, const std::vector<Value> &a) {
    if (live_held(s)) {
      auto *p = record(s);
      return Value::make_int(p ? static_cast<int>(p->render_set ? p->render_id
                                                               : p->config->render)
                               : 0);
    }
    if (auto *p = record(s)) {
      p->render_set = true;
      p->render_id = static_cast<uint32_t>(number(a, 0));
      retarget(*p, p->render_id);
    }
    return Value::make_int(1);
  });
  bind("flap", "(I)I", [](Object *s, const std::vector<Value> &a) {
    auto *p = record(s);
    if (!p)
      return Value::make_int(-1);
    if (number(a, 0) == 1) {
      p->flap ^= 1;
      if (p->state.parent)
        place_child(p->state.parent, p->state.parent_slot);
    }
    return Value::make_int(p->flap);
  });
  bind("getLogo", "()I", [](Object *s, const std::vector<Value> &) {
    return Value::make_int(field_int(canonical(s), "manufacturer"));
  });
  bind("setSfxLoopParams", "(FF)I", [](Object *s, const std::vector<Value> &a) {
    auto *p = record(s);
    if (!p)
      return Value::make_int(0);
    p->sfx_a = scalar(a, 0, 0);
    p->sfx_b = scalar(a, 1, 0);
    return Value::make_int(1);
  });
  bind("install_OK",
       "(Ljava.util.resource.GameRef;ILjava.util.resource.GameRef;ILjava.lang."
       "Vector3;)[I",
       [&jvm](Object *s, const std::vector<Value> &a) {
         // slot (arg 1) is the parent's slot, slot2 (arg 3) is this part's.
         // A drop point picks the closest free mount; a null point keeps
         // the first match, which is the stock-install walk.
         float hint[3]{};
         const float *use = nullptr;
         if (auto *pos = object(a, 4)) {
           hint[0] = comp(pos, "x");
           hint[1] = comp(pos, "y");
           hint[2] = comp(pos, "z");
           use = hint;
         }
         auto found = select(canonical(s), as_part(object(a, 0)), number(a, 3),
                             number(a, 1), true, use);
         if (!found.parent)
           return Value::make_obj(nullptr);
         if (auto *out = object(a, 2))
           gameref_bind_alias(out, found.parent);
         auto *pair = jvm.new_object("[I");
         pair->is_array = true;
         pair->slots = {Value::make_int(found.mount->id),
                        Value::make_int(found.mate->id)};
         pair->fields["length"] = Value::make_int(2);
         return Value::make_obj(pair);
       });
  auto chassis = [&](const char *name, const char *signature, NativeFn fn) {
    jvm.natives().register_method("java.game.parts.bodypart.Chassis", name,
                                  signature, std::move(fn));
  };
  chassis("setTorque", "(F)V", [](Object *s, const std::vector<Value> &a) {
    auto *p = record(s);
    if (!p)
      return Value::make_void();
    const float t = scalar(a, 0, 0);
    const float wear = p->state.wear;
    const float k = wear >= 1.f ? 0.1f : (1.f - wear * wear * 0.6f);
    p->torque = t;
    p->torque_scale = k * t;
    return Value::make_void();
  });
  chassis("setAckermann", "(F)V", [](Object *s, const std::vector<Value> &a) {
    if (auto *p = record(s))
      p->ackermann = scalar(a, 0, 0);
    return Value::make_void();
  });
  chassis("getCM", "()Ljava.lang.Vector3;",
          [](Object *s, const std::vector<Value> &) {
            float c[3]{};
            int n = 0;
            if (auto *p = record(s)) {
              const int count = wheel_count(s);
              for (int i = 0; i < count; ++i) {
                seed_wheel(*p, i);
                if (!p->wheels[static_cast<size_t>(i)].has_pos)
                  continue;
                for (int k = 0; k < 3; ++k)
                  c[k] += p->wheels[static_cast<size_t>(i)].pos[k];
                ++n;
              }
            }
            if (n)
              for (float &v : c)
                v /= static_cast<float>(n);
            return Value::make_obj(vec3(c[0], c[1], c[2]));
          });
  chassis("getWheelPos", "(I)Ljava.lang.Vector3;",
          [](Object *s, const std::vector<Value> &a) {
            float p3[3]{};
            if (auto *p = record(s)) {
              const int i = number(a, 0);
              if (i >= 0 && i < wheel_count(s)) {
                seed_wheel(*p, i);
                std::copy(p->wheels[static_cast<size_t>(i)].pos,
                          p->wheels[static_cast<size_t>(i)].pos + 3, p3);
                if (static_cast<size_t>(i) < p->config->wheels.size()) {
                  p3[0] = p->config->wheels[i].position[0];
                  p3[2] = p->config->wheels[i].position[2];
                }
              }
            }
            return Value::make_obj(vec3(p3[0], p3[1], p3[2]));
          });
  chassis("forceUpdate", "()V", [](Object *s, const std::vector<Value> &) {
    Object *chassis_obj = canonical(s);
    refresh_chassis(chassis_obj);
    if (auto *p = record(chassis_obj))
      for (int i = 0; i < wheel_count(chassis_obj); ++i)
        seed_wheel(*p, i);
    return Value::make_void();
  });
  chassis("getWheels", "()I", [](Object *s, const std::vector<Value> &) {
    return Value::make_int(wheel_count(s));
  });
  chassis("getWheel", "(I)Ljava.game.parts.WheelRef;",
          [&jvm](Object *s, const std::vector<Value> &a) {
            auto *p = record(s);
            const int id = number(a, 0);
            if (!p || id < 0 || id >= wheel_count(s))
              return Value::make_obj(nullptr);
            seed_wheel(*p, id);
            auto *wr = jvm.new_object("java.game.parts.WheelRef");
            if (!wr)
              return Value::make_obj(nullptr);
            wr->fields["chassis"] = Value::make_obj(canonical(s));
            wr->fields["wheel_id"] = Value::make_int(id);
            return Value::make_obj(wr);
          });
  chassis("getWheelDamage", "(I)Ljava.lang.String;",
          [&jvm](Object *s, const std::vector<Value> &a) {
            std::string text;
            if (auto *p = record(s)) {
              const int i = number(a, 0);
              if (i >= 0 && i < 8)
                text = p->wheel_damage[static_cast<size_t>(i)];
            }
            return Value::make_obj(jvm.new_string(text));
          });
  chassis("setWheelDamage", "(ILjava.lang.String;)V",
          [](Object *s, const std::vector<Value> &a) {
            auto *p = record(s);
            const int i = number(a, 0);
            if (p && i >= 0 && i < 8) {
              auto *data = object(a, 1);
              p->wheel_damage[static_cast<size_t>(i)] = data ? data->str : "";
            }
            return Value::make_void();
          });
  chassis("setCooling", "(FFF)V", [](Object *s, const std::vector<Value> &a) {
    if (auto *p = record(s)) {
      p->cooling[0] = scalar(a, 0, 10);
      p->cooling[1] = scalar(a, 1, 50);
      p->cooling[2] = scalar(a, 2, 0.01f);
    }
    return Value::make_void();
  });
  chassis("setSfxExhaustMinVol", "(F)V",
          [](Object *s, const std::vector<Value> &a) {
            if (auto *p = record(s))
              p->exhaust_min = scalar(a, 0, 0);
            return Value::make_void();
          });
  chassis("setSteerWheelRadius", "(F)V",
          [](Object *s, const std::vector<Value> &a) {
            if (auto *p = record(s))
              p->steer_radius = scalar(a, 0, 0);
            return Value::make_void();
          });
  chassis("setSteerWheel", "(FF)V", [](Object *s, const std::vector<Value> &a) {
    if (auto *p = record(s)) {
      p->steer_r = scalar(a, 0, 0);
      p->steer_z = scalar(a, 1, 0);
    }
    return Value::make_void();
  });
  chassis("setHornSFX", "(Ljava.util.resource.ResourceRef;FI)V",
          [](Object *s, const std::vector<Value> &a) {
            auto *p = record(s);
            if (!p)
              return Value::make_void();
            Record::Horn horn;
            horn.sfx = res_id(object(a, 0));
            horn.pitch = scalar(a, 1, 1);
            p->horns[number(a, 2)] = horn;
            return Value::make_void();
          });
  chassis("setNitroSFX", "(Ljava.util.resource.ResourceRef;F)V",
          [](Object *s, const std::vector<Value> &a) {
            if (auto *p = record(s)) {
              p->nitro_sfx = res_id(object(a, 0));
              p->nitro_pitch = scalar(a, 1, 1);
            }
            return Value::make_void();
          });
  chassis("getMileage", "()F", [](Object *s, const std::vector<Value> &) {
    auto *p = record(s);
    return Value::make_float(p ? p->mileage : 0);
  });
  chassis("setMileage", "(F)V", [](Object *s, const std::vector<Value> &a) {
    if (auto *p = record(s))
      p->mileage = scalar(a, 0, 0);
    return Value::make_void();
  });
  chassis("setBuck", "(IIFFFF)V", [](Object *s, const std::vector<Value> &a) {
    auto *p = record(s);
    if (!p)
      return Value::make_void();
    const int part = number(a, 0), id = number(a, 1);
    for (auto &e : p->bucks)
      if (e.part == part && e.id == id) {
        e.freq = scalar(a, 2, 0);
        e.prob = scalar(a, 3, 0);
        e.rpm = scalar(a, 4, 0);
        e.amp = scalar(a, 5, 0);
        return Value::make_void();
      }
    p->bucks.push_back({part, id, scalar(a, 2, 0), scalar(a, 3, 0),
                        scalar(a, 4, 0), scalar(a, 5, 0)});
    return Value::make_void();
  });
  chassis("getSfxTable", "(I)Ljava.game.parts.SfxTable;",
          [&jvm](Object *s, const std::vector<Value> &a) {
            auto *p = record(s);
            const int id = number(a, 0);
            if (!p || id < 0 || id > 2)
              return Value::make_obj(nullptr);
            if (!p->sfx[static_cast<size_t>(id)]) {
              auto *tab = jvm.new_object("java.game.parts.SfxTable");
              p->sfx[static_cast<size_t>(id)] = tab;
              if (tab)
                sfx_items[tab];
            }
            return Value::make_obj(p->sfx[static_cast<size_t>(id)]);
          });
  auto sfx = [&](const char *name, const char *signature, NativeFn fn) {
    jvm.natives().register_method("java.game.parts.SfxTable", name, signature,
                                  std::move(fn));
  };
  sfx("finalize", "()V", [](Object *s, const std::vector<Value> &) {
    if (s)
      sfx_items.erase(s);
    return Value::make_void();
  });
  sfx("getItems", "()I", [](Object *s, const std::vector<Value> &) {
    auto it = sfx_items.find(s);
    return Value::make_int(
        it == sfx_items.end() ? 0 : static_cast<int>(it->second.size()));
  });
  sfx("clear", "()V", [](Object *s, const std::vector<Value> &) {
    auto it = sfx_items.find(s);
    if (it != sfx_items.end())
      it->second.clear();
    return Value::make_void();
  });
  sfx("addItem", "(Ljava.util.resource.ResourceRef;FFFFF)V",
      [](Object *s, const std::vector<Value> &a) {
        auto it = sfx_items.find(s);
        if (it == sfx_items.end() || it->second.size() >= 16)
          return Value::make_void();
        SfxItem item;
        item.sfx = res_id(object(a, 0));
        const float pitch = scalar(a, 1, 1);
        item.pitch = pitch;
        item.pmin = scalar(a, 2, 0);
        item.pmax = scalar(a, 3, 0);
        item.vmin = scalar(a, 4, 0);
        item.vmax = scalar(a, 5, 0);
        (void)pitch;
        it->second.push_back(item);
        return Value::make_void();
      });
  auto wheel = [&](const char *name, const char *signature, NativeFn fn) {
    jvm.natives().register_method("java.game.parts.WheelRef", name, signature,
                                  std::move(fn));
  };
  wheel("finalize", "()V", [](Object *, const std::vector<Value> &) {
    return Value::make_void();
  });
  wheel("getPos", "()Ljava.lang.Vector3;",
        [](Object *s, const std::vector<Value> &) {
          auto *w = wheel_of(s);
          if (!w)
            return Value::make_obj(nullptr);
          float x = w->pos[0], z = w->pos[2];
          if (auto *p = record(s->fields["chassis"].tag == ValueTag::Obj
                                   ? s->fields["chassis"].v.o
                                   : nullptr)) {
            const int i = field_int(s, "wheel_id", -1);
            if (i >= 0 && static_cast<size_t>(i) < p->config->wheels.size()) {
              // Native getPos 0x441B60 takes X/Z from the template, not
              // from any of the three moving visual attachment bodies.
              x = p->config->wheels[i].position[0];
              z = p->config->wheels[i].position[2];
            }
          }
          return Value::make_obj(vec3(x, w->pos[1], z));
        });
  wheel("getYpr", "()Ljava.lang.Ypr;",
        [](Object *s, const std::vector<Value> &) {
          auto *w = wheel_of(s);
          if (!w)
            return Value::make_obj(nullptr);
          return Value::make_obj(ypr_obj(w->ypr[0], w->ypr[1], w->ypr[2]));
        });
  wheel("getDrive", "()F", [](Object *s, const std::vector<Value> &) {
    auto *w = wheel_of(s);
    return Value::make_float(w ? w->drive : 0);
  });
  wheel("getSteer", "()F", [](Object *s, const std::vector<Value> &) {
    auto *w = wheel_of(s);
    return Value::make_float(w ? w->steer : 0);
  });
  wheel("getRadius", "()F", [](Object *s, const std::vector<Value> &) {
    auto *w = wheel_of(s);
    return Value::make_float(w ? w->radius : 0);
  });
  wheel("getBrake", "()F", [](Object *s, const std::vector<Value> &) {
    auto *w = wheel_of(s);
    return Value::make_float(w ? w->brake : 0);
  });
  wheel("getHBrake", "()F", [](Object *s, const std::vector<Value> &) {
    auto *w = wheel_of(s);
    return Value::make_float(w ? w->hbrake : 0);
  });
  auto put = [](Object *s, auto &&write) {
    if (auto *w = wheel_of(s))
      write(*w);
    return Value::make_void();
  };
  wheel("setPos", "(Ljava.lang.Vector3;)V",
        [put](Object *s, const std::vector<Value> &a) {
          return put(s, [&](WheelPhys &w) {
            auto *v = object(a, 0);
            if (!v)
              return;
            w.pos[0] = comp(v, "x");
            w.pos[1] = comp(v, "y");
            w.pos[2] = comp(v, "z");
            w.has_pos = true;
            w.authored_position = true;
            std::copy(w.pos, w.pos + 3, w.input_position);
          });
        });
  wheel("setYpr", "(Ljava.lang.Ypr;)V",
        [put](Object *s, const std::vector<Value> &a) {
          return put(s, [&](WheelPhys &w) {
            auto *v = object(a, 0);
            if (!v)
              return;
            w.ypr[0] = comp(v, "y");
            w.ypr[1] = comp(v, "p");
            w.ypr[2] = comp(v, "r");
            w.has_ypr = true;
          });
        });
  wheel("setDrive", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.drive = scalar(a, 0, 0); });
  });
  wheel("setSteer", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) {
      w.steer = scalar(a, 0, 0);
      w.steer_set = true;
    });
  });
  wheel("setRadius", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.radius = scalar(a, 0, 0); });
  });
  wheel("setWidth", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.width = scalar(a, 0, 0) * 0.5f; });
  });
  wheel("setCPatch", "(FFF)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) {
      if (!w.contact)
        return;
      w.cpatch_hw = std::fabs(scalar(a, 0, 0));
      w.cpatch_ang = std::sin(std::fabs(scalar(a, 1, 0)));
      w.cpatch_off = scalar(a, 2, 0);
    });
  });
  wheel("setFriction", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(
        s, [&](WheelPhys &w) { w.friction = w.pacejka[2] = scalar(a, 0, 0); });
  });
  wheel("setFrictn_x", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(
        s, [&](WheelPhys &w) { w.frictn_x = w.pacejka[12] = scalar(a, 0, 0); });
  });
  wheel("setSliction", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(
        s, [&](WheelPhys &w) { w.sliction = w.pacejka[0] = scalar(a, 0, 0); });
  });
  wheel("setStiffness", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(
        s, [&](WheelPhys &w) { w.stiffness = w.pacejka[4] = scalar(a, 0, 0); });
  });
  wheel("setRollRes", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.roll_res = scalar(a, 0, 0); });
  });
  wheel("setBearing", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.bearing = scalar(a, 0, 0); });
  });
  wheel("setMaxLoad", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(
        s, [&](WheelPhys &w) { w.max_load = w.pacejka[11] = scalar(a, 0, 0); });
  });
  wheel("setLoadSmooth", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) {
      w.load_smooth = w.pacejka[14] = scalar(a, 0, 0);
    });
  });
  wheel("setPacejka", "(IF)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) {
      const int i = number(a, 0);
      if (i < 0 || i >= 17)
        return;
      const float val = scalar(a, 1, 0);
      w.pacejka[i] = val;
      name_pacejka(w, i, val);
    });
  });
  wheel("setForce", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.force = scalar(a, 0, 0); });
  });
  wheel("setDamping", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) {
      w.damp_bound = scalar(a, 0, 0);
      w.damp_rebound = w.damp_bound * 0.7f;
    });
  });
  wheel("setDamping", "(FF)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) {
      w.damp_bound = scalar(a, 0, 0);
      w.damp_rebound = scalar(a, 1, 0);
    });
  });
  wheel("setRestLen", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.rest = scalar(a, 0, 0); });
  });
  wheel("setMinLen", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.min_len = scalar(a, 0, 0); });
  });
  wheel("setMaxLen", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.max_len = scalar(a, 0, 0); });
  });
  wheel("setInstantCenter", "(FFFFFF)V",
        [put](Object *s, const std::vector<Value> &a) {
          return put(s, [&](WheelPhys &w) {
            for (int i = 0; i < 6; ++i)
              w.ic[i] = scalar(a, static_cast<size_t>(i), 0) + w.pos[i % 3];
          });
        });
  wheel("setBrake", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.brake = scalar(a, 0, 0); });
  });
  wheel("setHBrake", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.hbrake = scalar(a, 0, 0); });
  });
  wheel("setOppWheel", "(I)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.opp = number(a, 0); });
  });
  wheel("setArm", "(FFFFFFF)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) {
      w.has_arm = true;
      for (int i = 0; i < 7; ++i)
        w.arm[i] = scalar(a, static_cast<size_t>(i), 0);
    });
  });
  wheel("setHub", "(FFFFFFFFFF)V",
        [put](Object *s, const std::vector<Value> &a) {
          return put(s, [&](WheelPhys &w) {
            for (int i = 0; i < 10; ++i)
              w.hub[i] = scalar(a, static_cast<size_t>(i), 0);
            w.has_hub = true;
          });
        });
}
} // namespace slrr
