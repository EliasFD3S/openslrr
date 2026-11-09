#include "slrr/gc_roots.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/dyno_model.hpp"
#include "slrr/gameref.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/host_input.hpp"
#include "slrr/deformation_cage.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/osd_registry.hpp"
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
#include <fstream>
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
  float friction = 1.49f, frictn_x = 1, sliction = 1.4f, stiffness = 15.2f;
  float roll_res = 0.001f, bearing = 20, max_load = 8000, load_smooth = .4f;
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
  std::string path;
  int32_t voice = 0;
  float pitch = 1, pmin = 0, pmax = 0, vmin = 0, vmax = 0;
};
struct Record {
  std::unique_ptr<physics::DeformationCage> deformation;
  std::unique_ptr<ScxGeometry> collision, deformed_collision;
  unsigned deformation_upload_ticks = 0;
  float steering_input = 0;
  float steering_help = 0, abs_strength = 0, launch_help = 0;
  int transmission = 1;
  uint64_t driving_options_revision = 0;
  float damage_multiplier = -1.f;
  physics::Vec3 cockpit_offset;
  bool horn_down = false;
  bool engine_running = false;
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
    PartMatrix rest_pose{};
  };
  std::vector<LinkVisual> links;
  float max_wear = 1.f;
  int flap = 0;
  float flap_progress = 0.f;
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
  std::array<PartMatrix, 4> travel_frame{}, steered_frame{}, rolling_frame{};
  std::array<bool, 4> geometry_solved{};
  std::array<float, 4> wheel_spin_sign{};
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
struct LoosePanel {
  physics::RigidBody body;
  PartMatrix carrier_frame{};
  int32_t vehicle = 0;
  unsigned quiet_ticks = 0;
};
std::unordered_map<Object *, LoosePanel> loose_panels;
int32_t controlled_vehicle = 0;
VehicleHudLayout hud_preview;
std::string hud_preview_directory;
constexpr double kPhysicsTick = 1.0 / physics::VehicleDynamics::update_frequency;
constexpr unsigned kMaxPhysicsTicksPerFrame = 4;
double physics_elapsed = 0;
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
bool wheel_on_suspension_service(const Record& parent, int slot, Object* child) {
  if (!runtime || parent.config->native_kind != "car" ||
      !((slot >= 301 && slot <= 304) || (slot >= 311 && slot <= 314)))
    return false;
  return runtime->tree_instance_of(child, "java.game.parts.rgearpart.Spring") ||
         runtime->tree_instance_of(child, "java.game.parts.rgearpart.ShockAbsorber") ||
         runtime->tree_instance_of(child, "java.game.parts.rgearpart.Pneumatic_ShockAbsorber");
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
          (free_only && blocked != p->slot_status.end() && blocked->second &&
           !(blocked->second == 1 && wheel_on_suspension_service(*p, mount.id, child))) ||
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
      rec.links.push_back({render, resource, *rules, local});
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
  for (Object *tab : rec.sfx) {
    auto table = sfx_items.find(tab);
    if (table == sfx_items.end()) continue;
    for (auto &item : table->second) host_sfx_stop(item.voice);
    sfx_items.erase(table);
  }
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
bool corner_gear(int type);
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
    if (rec->dynamic_frames && corner_gear(type)) {
      const int corner = type % 100 - 1;
      // A link follows the suspension, never the spin of the rim mesh.
      const auto& delta = type >= 301 || !follow_steering
                              ? rec->travel_frame[corner] : rec->steered_frame[corner];
      frame = part_matrix_multiply(delta, frame);
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
            box.min[i] = mesh.info.bbox_min[i] * mesh.position_scale;
            box.max[i] = mesh.info.bbox_max[i] * mesh.position_scale;
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
  using physics::Vec3;
  const Vec3 axis{b[12] - a[12], b[13] - a[13], b[14] - a[14]};
  const double length = axis.length();
  const Vec3 direction = axis / length;
  // Shortest rotation of local +Y onto the two linked anchors.
  const auto turn = direction.y < -1 + 1e-8
      ? physics::Quaternion{1, 0, 0, 0}
      : physics::Quaternion{direction.z, 0, -direction.x, 1 + direction.y};
  const auto rotation = physics::corner_matrix({}, turn);
  PartMatrix scale = eye_matrix(), to = eye_matrix(), center = eye_matrix();
  scale[5] = static_cast<float>(length / (max_y - min_y));
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
  const bool mounted = car(part) != nullptr;
  for (const auto &vis : rec->links) {
    auto *node = gameref_get(vis.node);
    if (!node)
      continue;
    // Linked suspension visuals overwrite the render matrix, not the mesh.
    // Always start from the authored pose; an inventory part has no chassis
    // anchors and must not keep its last compressed/tilted matrix.
    std::copy(vis.rest_pose.begin(), vis.rest_pose.end(), node->local_matrix);
    node->matrix_override = true;
    if (!mounted)
      continue;
    const PartLink *rule = nullptr;
    for (const auto &candidate : vis.rules)
      if (!candidate.filter || !candidate.parent_slot ||
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
double wheel_steering_angle(const Record& rec, size_t corner) {
  const auto& wheel = rec.wheels[corner];
  const double factor = wheel.steer_set ? wheel.steer
      : corner < rec.config->wheel_steering.size() ? rec.config->wheel_steering[corner] : 0;
  const double angle = rec.steering_input * rec.config->max_steer;
  const double length = rec.ackermann != 0 ? rec.ackermann : rec.config->ackermann_length;
  if (std::abs(angle) < 1e-7 || length <= 0) return angle * factor;
  const double radius = length / std::tan(angle);
  const double x = rec.dynamics.wheels[corner].center.x;
  return (radius <= 0 ? -std::atan2(length, -radius - x)
                      : std::atan2(length, radius + x)) * factor;
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
  const bool gear = corner_gear(type);
  if (gear && p->dynamic_frames) {
    const int corner = type % 100 - 1;
    // The arm anchor follows travel; the wheel/hub also follows steering.
    const auto &delta = type >= 301   ? p->travel_frame[corner]
                        : type >= 201 ? p->steered_frame[corner]
                                      : p->rolling_frame[corner];
    transform = part_matrix_multiply(delta, transform);
    parent_frame = part_matrix_multiply(
        type < 201 ? p->steered_frame[corner] : delta, parent_frame);
  }
  auto *node = gameref_get(found->second);
  if (!node)
    return;
  // Open flap: the hinge lives on the parent slot. Closed is the CFG pose.
  if (mount->has_flap && c->flap_progress > 0.f) {
    float ax = mount->flap_axis[0], ay = mount->flap_axis[1],
          az = mount->flap_axis[2];
    const float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len > 1e-6f) {
      ax /= len;
      ay /= len;
      az /= len;
      const float t = c->flap_progress;
      const float eased = t * t * (3.f - 2.f * t);
      const float angle = mount->flap_hi * eased;
      const float c0 = std::cos(angle);
      const float s0 = std::sin(angle);
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
  if (p->deformation && !gear && c->config->category == 2)
    transform = p->deformation->carry_frame(transform);
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
  auto *rec = record(chassis);
  if (!rec || field_int(chassis, "suspend_update"))
    return;
  ++refresh_depth;
  // Only installed parts write spring and damper settings during this pass.
  // Clear the old contributions so removing a part cannot leave it active.
  for (auto& wheel : rec->wheels) {
    wheel.force = wheel.rest = wheel.min_len = wheel.max_len = 0.f;
    wheel.damp_bound = wheel.damp_rebound = 0.f;
  }
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

} // namespace

bool part_lamp_lit(void *part, uint32_t flags, bool preview) {
  if (!(flags & 4u) || (flags & 0x40u) || !runtime)
    return false;
  if (!preview) {
    const auto *chassis = record(car(static_cast<Object *>(part)));
    if (!chassis || !chassis->engine_running) return false;
  }
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
  return sec < 21600.f || sec >= 75600.f;
}

Object *part_create(Jvm &jvm, RpkIndex &resources, Object *parent,
                    uint32_t resource, const std::string &alias,
                    const std::string &params) {
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
bool part_install(Object *child, Object *root, int own, int target,
                   const float *hint) {
  child = canonical(child);
  root = canonical(root);
  auto *c = record(child);
  if (!c || c->state.parent)
    return false;
  auto selected = select(child, root, own, target, true, hint);
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
  loose_panels.erase(child);
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
void paint_one(Object *part, uint32_t texture) {
  auto *rec = record(part);
  if (!rec || !rec->config->texture)
    return;
  rec->state.texture = texture;
  for (auto *render : rec->renders)
    if (auto *node = gameref_get(render))
      node->paint_texture = texture;
}
void paint_tree(Object *root, uint32_t texture) {
  std::vector<Object *> pending{root};
  std::unordered_set<Object *> seen;
  for (size_t i = 0; i < pending.size() && i < 512; ++i) {
    if (!seen.insert(pending[i]).second)
      continue;
    paint_one(pending[i], texture);
    if (auto *rec = record(pending[i]))
      for (const auto &child : rec->children)
        pending.push_back(child.second);
  }
}
static void apply_driving_options(Record& rec) {
  const auto& options = host_game_options().driving;
  rec.transmission = options.transmission;
  rec.steering_help = std::clamp(options.steering_help, 0.f, 1.f);
  rec.abs_strength = std::clamp(options.abs, 0.f, 1.f);
  rec.launch_help = std::clamp(options.asr, 0.f, 1.f);
  rec.driving_options_revision = host_driving_options_revision();
}

bool part_command(Object *part, const std::string &command) {
  part = canonical(part);
  std::istringstream input(command);
  std::string verb;
  input >> verb;
  if (part && part->class_name == "java.io.Controller" &&
      (verb == "controllable" || verb == "leave")) {
    int32_t id = 0;
    if (!(input >> id)) return false;
    if (auto* target = gameref_find_live(id); target && target->kind == GameRefKind::Camera) {
      target->camera_free = verb == "controllable";
      target->cam_pose_init = false;
      return true;
    }
    if (verb == "controllable") {
      controlled_vehicle = id;
      auto* vehicle = gameref_find_live(id);
      Object* chassis = vehicle ? as_part(static_cast<Object*>(vehicle->key)) : nullptr;
      auto* car = record(chassis);
      if (car) {
        apply_driving_options(*car);
        car->cockpit_offset = {};
      }
      const auto* node = gameref_get(chassis);
      const auto* parent = node ? gameref_get(node->parent) : nullptr;
      // Track.lockCar resets the chassis before attaching the controller.
      // A map with a ground shell is driveable. The garage engine test
      // attaches the same controller to a car on the lift.
      if (car && parent && static_cast<Object*>(parent->key)->fields.contains("_ground_shell")) {
        car->physics_suspended = false;
        car->physics_released = true;
      }
    } else if (controlled_vehicle == id)
      controlled_vehicle = 0;
    return true;
  }
  if (part && part->class_name == "java.io.Controller" && verb == "camera") {
    int id;
    if (!(input >> id)) return false;
    if (auto* cam = gameref_find_live(id); cam && cam->kind == GameRefKind::Camera &&
        controlled_vehicle) {
      cam->follow_vehicle = controlled_vehicle;
      cam->aim_look = cam->aim_move = cam->camera_attached = cam->camera_free = false;
      cam->camera_autozoom = false;
      cam->cam_pose_init = false;
    }
    return true;
  }
  if (verb == "render" || verb == "hide") {
    Object* chassis = as_part(part);
    const auto* car = record(chassis);
    int viewport_id;
    if (car && input >> viewport_id) {
      auto* viewport = gameref_find_live(viewport_id);
      if (viewport && viewport->kind == GameRefKind::Viewport &&
          viewport->vp_w >= .99f && viewport->vp_h >= .99f) {
        auto existing = chassis->fields.find("_hostCockpitCamera");
        Object* camera = existing != chassis->fields.end() ? existing->second.v.o : nullptr;
        if (verb == "hide") {
          if (auto* state = gameref_get(camera)) state->cam_active = false;
          return true;
        }
        int controller_id, camera_index;
        if (!(input >> controller_id >> camera_index) || camera_index < 0 ||
            static_cast<size_t>(camera_index) >= car->config->cameras.size()) return true;
        if (!camera) {
          camera = runtime->new_object("java.render.Camera");
          chassis->fields["_hostCockpitCamera"] = Value::make_obj(camera);
        }
        auto* state = gameref_upsert(camera, GameRefKind::Camera);
        state->cockpit_index = camera_index;
        state->follow_vehicle = gameref_make_live(chassis);
        state->half_aov = 42.f;
        state->dmin = .03f;
        state->dmax = 2000.f;
        gameref_activate_camera(*state, *viewport, 1);
        return true;
      }
    }
  }
  if (verb == "transmission") {
    auto* rec = record(as_part(part));
    int mode;
    if (!rec || !(input >> mode)) return false;
    rec->transmission = mode;
    return true;
  }
  if (verb == "damage_multiplier") {
    auto* rec = record(as_part(part));
    float value;
    if (!rec || !(input >> value) || !std::isfinite(value)) return false;
    rec->damage_multiplier = std::max(0.f, value);
    return true;
  }
  if (verb == "difflock") {
    auto* chassis = as_part(part);
    float value;
    if (!record(chassis) || !(input >> value) || !std::isfinite(value)) return false;
    chassis->fields["diff_lock"] = Value::make_float(std::clamp(value, 0.f, 1.f));
    return true;
  }
  if (verb == "steerhelp" || verb == "abs" || verb == "asr") {
    auto* rec = record(as_part(part));
    float value;
    if (!rec || !(input >> value) || !std::isfinite(value)) return false;
    (verb == "abs" ? rec->abs_strength : verb == "asr" ? rec->launch_help :
                                                       rec->steering_help) = std::clamp(value, 0.f, 1.f);
    return true;
  }
  if (verb == "texture") {
    // Catalog and the car selector send this on the Vehicle, which is the
    // chassis's owner. The second number, 1, repaints every painted part.
    Object *root = part;
    if (!record(root) && part) {
      auto it = part->fields.find("chassis");
      if (it != part->fields.end() && it->second.tag == ValueTag::Obj &&
          it->second.v.o)
        root = canonical(it->second.v.o);
    }
    if (!record(root))
      return false;
    unsigned long id = 0;
    int mode = 1;
    if (!(input >> id))
      return false;
    input >> mode;
    if (mode)
      paint_tree(root, static_cast<uint32_t>(id));
    else
      paint_one(root, static_cast<uint32_t>(id));
    return true;
  }
  auto *rec = record(part);
  if (!rec)
    return false;
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
    // `install <cursor> <root> <ownSlot> <target> <slot> [x y z]`.
    // Loading a car names the exact parent slot. A drop also sends the
    // point, and the nearest free slot there wins. The own slot is unused,
    // as in the original command.
    int cursor = 0, root_id = 0, own_slot = 0, target_id = 0, slot = 0;
    if (!(input >> cursor >> root_id))
      return true;
    const bool have_own = static_cast<bool>(input >> own_slot);
    (void)own_slot;
    const bool have_target = have_own && static_cast<bool>(input >> target_id);
    const bool have_slot = have_target && static_cast<bool>(input >> slot);
    float hint[3];
    const bool have_hint = have_slot && (input >> hint[0] >> hint[1] >> hint[2]);
    const int object_id = have_target && target_id ? target_id : root_id;
    if (auto *node = gameref_find_live(object_id))
      part_install(part, static_cast<Object *>(node->key), 0,
                   have_slot ? slot : 0, have_hint ? hint : nullptr);
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
    relink_assembly(part);
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
    rec->dynamics.reset_drive();
    rec->engine_running = false;
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
  if (verb == "stop" && rec->config->native_kind == "car") {
    rec->physics_released = false;
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
bool part_destroy(Object *part) {
  part = canonical(part);
  auto *rec = record(part);
  if (!rec)
    return false;
  loose_panels.erase(part);
  if (auto camera = part->fields.find("_hostCockpitCamera"); camera != part->fields.end())
    gameref_remove(camera->second.v.o);
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
    std::printf("[phys] dynamic type=0x%x mass=%.2f fixed=%uHz\n",
                rec.state.resource, mass, physics::VehicleDynamics::frequency);
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
    cfg.rigid_contact = false;
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
    PartMatrix mount_frame;
    if (!part_slot_matrix(*rec.config, posed_slot(rec, *mount), mount_frame))
      continue;
    rec.wheel_spin_sign[i] = mount_frame[0] < 0 ? 1.f : -1.f;
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
    alignment.rotation = rec.config->wheels[i].rotation;
    if (live.has_ypr)
      std::copy(live.ypr, live.ypr + 3, alignment.rotation.begin());
    auto orient = part_pose_matrix(alignment);
    cfg.down = {-orient[4], -orient[5], -orient[6]};
    const auto& ypr = alignment.rotation;
    cfg.hub_alignment =
        (physics::wheel_steering(ypr[0]) *
         physics::Quaternion{std::sin(ypr[1] * .5), 0, 0, std::cos(ypr[1] * .5)} *
         physics::Quaternion{0, 0, std::sin(ypr[2] * .5), std::cos(ypr[2] * .5)}).normalized();
    cfg.radius = live.radius > 0 ? live.radius : fallback.radius;
    cfg.rest = live.rest;
    cfg.anchor = fallback.base + fallback.travel;
    cfg.minimum = std::max(0.f, live.min_len);
    cfg.maximum = std::max(cfg.minimum, double(live.max_len > 0 ? live.max_len : live.rest));
    cfg.spring = live.force;
    cfg.bump_damping = live.damp_bound;
    cfg.rebound_damping = live.damp_rebound;
    cfg.unsprung = fallback.mass;
    cfg.arm_length = 0;
    cfg.has_hub = false;
    cfg.hub = {};
    if (live.has_arm && live.opp < 0 && live.arm[0] > 0) {
      cfg.arm_length = live.arm[0];
      cfg.arm_pivot = cfg.center + physics::Vec3{live.arm[1], live.arm[2],
                                                 live.arm[3]};
      cfg.arm_axis = {live.arm[4], live.arm[5], live.arm[6]};
      if (live.has_hub) {
        cfg.has_hub = true;
        for (int k = 0; k < 10; ++k)
          cfg.hub[static_cast<size_t>(k)] = live.hub[k];
      }
    }
    cfg.enabled = cfg.radius > 0;
    cfg.rigid_contact = cfg.enabled &&
                        !(cfg.rest > 0 && cfg.maximum > cfg.minimum &&
                          cfg.spring > 0);
    if (cfg.rigid_contact) {
      // A mounted wheel without a working spring rests at its bump stop.
      const double stop = live.min_len > 0 ? live.min_len : fallback.base;
      cfg.minimum = cfg.maximum = cfg.rest = stop;
      cfg.spring = 0;
    }
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
    rec.travel_frame[i] = rec.steered_frame[i] = rec.rolling_frame[i] =
        eye_matrix();
    rec.geometry_solved[i] = false;
    rec.wheels[i].contact = cfg.enabled && state.contact;
    if (!cfg.enabled) continue;
    const auto base = part_pose_matrix(rec.config->wheels[i]);
    const auto inverse = part_matrix_inverse_rigid(base);
    const auto frames = physics::wheel_pose(cfg, state.length, &rec.geometry_solved[i]);
    const auto hub_steered = frames.hub_rotation *
                             physics::wheel_steering(wheel_steering_angle(rec, i));
    rec.travel_frame[i] = part_matrix_multiply(
        physics::corner_matrix(frames.arm_origin, rec.geometry_solved[i]
            ? frames.arm_rotation : frames.hub_rotation), inverse);
    rec.steered_frame[i] = part_matrix_multiply(
        physics::corner_matrix(frames.hub_origin, hub_steered), inverse);
    const double spin = rec.physics_released ? state.rotation * rec.wheel_spin_sign[i] : 0;
    const physics::Quaternion rolling{std::sin(spin * .5), 0, 0, std::cos(spin * .5)};
    rec.rolling_frame[i] = part_matrix_multiply(
        physics::corner_matrix(frames.hub_origin, hub_steered * rolling), inverse);
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
bool chassis_rest_contact(const Record& rec, physics::Vec3& contact) {
  float low, high, x, z;
  const uint32_t render = rec.render_set ? rec.render_id : rec.config->render;
  if (!mesh_span(render, low, high, x, z)) return false;
  contact = {x, low, z};
  return true;
}
// setMatrix of a car puts the midpoint of the four static tyre contacts on
// the given point and writes the loaded spring length. The body stays asleep.
float parked_wheel_load(const Record& rec, size_t i, float mass, const float (&com)[3],
                        int count) {
  if (count != 4) return mass / static_cast<float>(std::max(1, count));
  const auto& wheels = rec.dynamics.wheels;
  const auto fraction = [](double a, double b, double center) {
    return std::abs(b - a) > 1e-6 ? std::clamp((b - center) / (b - a), 0., 1.) : .5;
  };
  const double front = fraction((wheels[0].center.z + wheels[1].center.z) * .5,
                                (wheels[2].center.z + wheels[3].center.z) * .5, com[2]);
  const size_t axle = i & ~size_t{1};
  const double left = fraction(wheels[axle].center.x, wheels[axle + 1].center.x, com[0]);
  return static_cast<float>(mass * (i < 2 ? front : 1 - front) * (i % 2 ? 1 - left : left));
}

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
  if (!(mass > 0.f))
    return;
  physics::Vec3 contact;
  int used = 0;
  if (!count) {
    // Bare shells have no tyre contact frame. Use their own bottom instead
    // of returning with the model origin (often inside the shell) on the floor.
    if (!chassis_rest_contact(*rec, contact)) return;
    used = 1;
  }
  for (size_t i = 0; i < 4; ++i) {
    auto &cfg = rec->dynamics.wheels[i];
    auto &state = rec->dynamics.state[i];
    if (!cfg.enabled)
      continue;
    float length = static_cast<float>(cfg.rest);
    if (!part_parked_spring_length(static_cast<float>(cfg.rest),
                                   static_cast<float>(cfg.minimum),
                                   static_cast<float>(cfg.maximum),
                                   static_cast<float>(cfg.spring),
                                   parked_wheel_load(*rec, i, mass, com, count),
                                   length))
      length = static_cast<float>(cfg.rest);
    state.length = length;
    state.speed = 0;
    state.seeded = true;
    state.contact = false;
    state.force = 0;
    contact += physics::wheel_pose(cfg, state.length).hub_origin -
               physics::Vec3{0, cfg.radius, 0};
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
bool script_pos(void *key, float *x, float *y, float *z) {
  if (!key || !x || !y || !z)
    return false;
  Object *chassis = canonical(static_cast<Object *>(key));
  auto *rec = record(chassis);
  auto *node = chassis ? gameref_get(chassis) : nullptr;
  if (!rec || !node || rec->config->native_kind != "car")
    return false;
  dynamics_wheels(*rec);
  float mass = 0.f, com[3]{};
  assembly_mass_center(chassis, mass, com);
  int count = 0;
  for (const auto &wheel : rec->dynamics.wheels)
    if (wheel.enabled)
      ++count;
  if (!(mass > 0.f))
    return false;
  physics::Vec3 contact;
  int used = 0;
  if (!count) {
    if (!chassis_rest_contact(*rec, contact)) return false;
    used = 1;
  }
  for (size_t i = 0; i < 4; ++i) {
    const auto &cfg = rec->dynamics.wheels[i];
    const auto &state = rec->dynamics.state[i];
    if (!cfg.enabled)
      continue;
    float length = static_cast<float>(cfg.rest);
    if (state.seeded)
      length = static_cast<float>(state.length);
    else if (!part_parked_spring_length(static_cast<float>(cfg.rest),
                                        static_cast<float>(cfg.minimum),
                                        static_cast<float>(cfg.maximum),
                                        static_cast<float>(cfg.spring),
                                        parked_wheel_load(*rec, i, mass, com, count),
                                        length))
      length = static_cast<float>(cfg.rest);
    contact += physics::wheel_pose(cfg, length).hub_origin -
               physics::Vec3{0, cfg.radius, 0};
    ++used;
  }
  if (!used)
    return false;
  contact = contact * (1.0 / used);
  PartPose pose;
  std::copy(node->ypr, node->ypr + 3, pose.rotation.begin());
  const auto turned = part_pose_matrix(pose);
  const double cx = turned[0] * contact.x + turned[4] * contact.y +
                    turned[8] * contact.z;
  const double cy = turned[1] * contact.x + turned[5] * contact.y +
                    turned[9] * contact.z;
  const double cz = turned[2] * contact.x + turned[6] * contact.y +
                    turned[10] * contact.z;
  *x = node->pos[0] + static_cast<float>(cx);
  *y = node->pos[1] + static_cast<float>(cy);
  *z = node->pos[2] + static_cast<float>(cz);
  return true;
}
} // namespace
void part_park_chassis(Object *car, bool shift) { park_chassis(car, shift); }
bool part_car_script_pos(void *key, float *x, float *y, float *z) {
  return script_pos(key, x, y, z);
}
namespace {
Object *part_field(Object *part, const char *name) {
  auto value = part->fields.find(name);
  return value != part->fields.end() && value->second.tag == ValueTag::Obj
             ? value->second.v.o : nullptr;
}

void configure_drive(Object *chassis, Record &rec, bool controlled) {
  auto &drive = rec.dynamics.drive;
  drive.torque = {};
  if (!controlled) return;
  if (rec.driving_options_revision != host_driving_options_revision())
    apply_driving_options(rec);
  auto *ratios = part_field(chassis, "ratio");
  if (!ratios || ratios->slots.size() < 8 || field_int(chassis, "gears") < 1)
    return;
  Object *dyno = nullptr;
  for (const auto &[slot, child] : rec.children) {
    if ((dyno = part_field(child, "dynodata"))) break;
  }
  if (!dyno || comp(chassis, "maxRPM") <= 0) return;
  const auto &in = host_input();
  const bool controls = !osd_has_modal();
  const float steer = controls ? in.drive_steer : 0.f;
  const float max_steer = std::max(.01f, rec.config->max_steer);
  const float speed = float(rec.dynamics.body.velocity.length() / 15);
  const bool unwinding = steer * rec.steering_input <= 0 ||
                         std::abs(steer) < std::abs(rec.steering_input);
  const float helped_rate = std::max(.2f, 2.f / (1 + speed * speed));
  const float rate = std::lerp(2.f, helped_rate,
      unwinding || in.drive_steer_analog ? 0.f : rec.steering_help);
  const float max_delta = rate / (max_steer * physics::VehicleDynamics::update_frequency);
  rec.steering_input += std::clamp(steer - rec.steering_input, -max_delta, max_delta);
  drive.throttle = controls ? in.drive_throttle : 0.f;
  drive.brake = controls ? in.drive_brake : 0.f;
  drive.handbrake = controls && in.drive_handbrake ? .3 : 0;
  drive.abs_strength = rec.abs_strength;
  drive.launch_help = rec.launch_help;
  drive.differential_lock = std::clamp(comp(chassis, "diff_lock"), 0.f, 1.f);
  drive.clutch = controls ? 1.f - std::clamp(in.drive_clutch, 0.f, 1.f) : 0.f;
  drive.idle = std::max(400.f, comp(chassis, "engine_rpm_idle"));
  drive.limit = std::max(drive.idle + 100.0, double(comp(chassis, "RPM_limit")));
  drive.inertia = .28 + .01 * std::max(0.f, comp(chassis, "engine_inertia"));
  drive.torque = [dyno](double rpm) { return dyno_sample(dyno, float(rpm), 0, false); };
  drive.engaged = rec.physics_released;
  drive.reverse = controls && in.drive_reverse;
  drive.shift = !controls ? 0 : in.drive_shift > .5f ? 1 : in.drive_shift < -.5f ? -1 : 0;
  drive.automatic_shift = rec.transmission == 1;
  drive.automatic_first = rec.transmission == 5;
  drive.auto_clutch = rec.transmission != 2;
  drive.starter = std::max(0.f, comp(chassis, "starter_torque"));
  const double brake_inertia = std::max(.01f, rec.config->brake_inertia);
  drive.bearing_scale = .1 / brake_inertia;
  // DiscBrake supplies torque. Normalize the CFG gains before using the
  // physical wheel inertia, which is already expressed in SI units.
  const double inertia_gain = 50 / brake_inertia;
  drive.brake_scale = inertia_gain * std::max(0.f, rec.config->brake_power) / 1000;
  drive.handbrake_scale = inertia_gain * std::max(0.f, rec.config->handbrake_power) / 2000;
  drive.gears = std::clamp(field_int(chassis, "gears"), 1, 6);
  const double final_drive = comp(chassis, "rearend_ratio");
  drive.ratios[0] = 0;
  for (int i = 1; i < 8; ++i)
    drive.ratios[i] = scalar(ratios->slots, i, 0) * final_drive;
  const bool horn = controls && in.drive_horn;
  if (horn && !rec.horn_down && index) {
    auto it = rec.horns.find(1);
    if (it != rec.horns.end()) {
      auto path = index->payload_prop(it->second.sfx, "sourcefile");
      if (!path.empty()) {
        std::replace(path.begin(), path.end(), '\\', '/');
        host_sfx_play_file((std::filesystem::path(index->game_root()) / path).string(),
                           it->second.pitch);
      }
    }
  }
  rec.horn_down = horn;
  for (size_t i = 0; i < drive.wheels.size(); ++i) {
    auto &out = drive.wheels[i];
    const auto &wheel = rec.wheels[i];
    out.weight = std::max(0.f, wheel.drive);
    out.steer = wheel_steering_angle(rec, i);
    out.brake = std::max(0.f, wheel.brake);
    out.handbrake = std::max(0.f, wheel.hbrake);
    out.bearing = std::max(0.f, wheel.bearing);
    out.rolling_resistance = std::max(0., wheel.roll_res - .003625);
    out.tyre = {wheel.friction, wheel.stiffness, wheel.sliction,
                wheel.max_load, wheel.frictn_x};
  }
}

void update_engine_audio(Record &rec, bool active) {
  const float rpm = static_cast<float>(rec.dynamics.engine_omega * 9.54929658551372);
  for (size_t i : {0u, 2u}) {
    const auto table = sfx_items.find(rec.sfx[i]);
    if (table == sfx_items.end()) continue;
    auto &items = table->second;
    for (size_t j = 0; j < items.size(); ++j) {
      auto &item = items[j];
      float gain = 0;
      if (active && rpm >= item.pmin && rpm <= item.pmax) {
        const float t = (rpm - item.pmin) / (item.pmax - item.pmin);
        gain = std::lerp(item.vmin, item.vmax, t);
        if (j > 0 && items[j - 1].pmax > item.pmin)
          gain *= std::clamp((rpm - item.pmin) / (items[j - 1].pmax - item.pmin), 0.f, 1.f);
        if (j + 1 < items.size() && items[j + 1].pmin < item.pmax)
          gain *= std::clamp((item.pmax - rpm) / (item.pmax - items[j + 1].pmin), 0.f, 1.f);
        if (i == 2)
          gain *= std::lerp(std::clamp(rec.exhaust_min, 0.f, 1.f), 1.f,
                            static_cast<float>(rec.dynamics.drive.throttle));
      }
      if (gain > .001f) {
        const float pitch = std::clamp(rpm / item.pitch, .58f, 3.f);
        if (!item.voice)
          item.voice = host_sfx_play_file(item.path, pitch, gain, true);
        else
          host_sfx_update_loop(item.voice, pitch, gain);
      } else if (item.voice) {
        host_sfx_stop(item.voice);
        item.voice = 0;
      }
    }
  }
}

physics::VehicleDynamics::Impact collide_chassis(Record& rec, physics::Vec3 before) {
  using physics::Vec3;
  auto& body = rec.dynamics.body;
  const Vec3 delta = body.position - before;
  if (delta.dot(delta) < 1e-6) return {};
  const float movement[] = {float(delta.x), float(delta.y), float(delta.z)};
  constexpr float radius = .18f;
  struct SweepContact {
    SceneSphereHit hit;
    Vec3 arm;
  };
  std::array<SweepContact, 4> contacts;
  size_t contact_count = 0;
  SceneSphereHit closest;
  Vec3 closest_arm;
  for (double x : {body.minimum.x + radius, body.maximum.x - radius})
    for (double z : {body.minimum.z + radius, body.maximum.z - radius}) {
      Vec3 point{x, std::lerp(body.minimum.y, body.maximum.y, .35), z};
      if (rec.deformation) point += rec.deformation->displacement(point);
      const Vec3 offset = body.orientation.rotate(point - body.center);
      const Vec3 position = before + offset;
      const float start[] = {float(position.x), float(position.y), float(position.z)};
      SceneSphereHit hit;
      if (scene_instances_sweep_sphere(start, movement, radius, hit, .65f)) {
        contacts[contact_count++] = {hit, offset};
        if (hit.fraction < closest.fraction) {
          closest = hit;
          closest_arm = offset;
        }
      }
    }
  if (closest.fraction >= 1.f) return {};
  const Vec3 normal{closest.normal[0], closest.normal[1], closest.normal[2]};
  Vec3 arm;
  size_t simultaneous = 0;
  for (size_t i = 0; i < contact_count; ++i) {
    const auto& [hit, offset] = contacts[i];
    const Vec3 hit_normal{hit.normal[0], hit.normal[1], hit.normal[2]};
    if (normal.dot(hit_normal) > .95 &&
        (hit.fraction - closest.fraction) * -delta.dot(normal) < .035) {
      arm += offset;
      ++simultaneous;
    }
  }
  arm = simultaneous ? arm / static_cast<double>(simultaneous) : closest_arm;
  body.position = before + delta * closest.fraction + normal * .002;
  const Vec3 point = body.position + arm - normal * radius;
  const double speed = -body.point_velocity(point).dot(normal);
  body.contact(point, normal, .12, .35);
  return {point, normal, speed};
}

void update_collision_bounds(ScxGeometry& mesh) {
  if (mesh.positions.empty()) return;
  for (int axis = 0; axis < 3; ++axis) {
    mesh.info.bbox_min[axis] = mesh.info.bbox_max[axis] = mesh.positions[axis];
    for (size_t i = axis + 3; i < mesh.positions.size(); i += 3) {
      mesh.info.bbox_min[axis] = std::min(mesh.info.bbox_min[axis], mesh.positions[i]);
      mesh.info.bbox_max[axis] = std::max(mesh.info.bbox_max[axis], mesh.positions[i]);
    }
  }
}

// SLRR_rev part-collision.ts. Bake the original CFG body poses into metres.
const ScxGeometry& part_collision_mesh(Record& rec) {
  if (!rec.collision) {
    rec.collision = std::make_unique<ScxGeometry>();
    auto& mesh = *rec.collision;
    mesh.position_scale = 1.f;
    for (const auto& body : rec.config->bodies) {
      if (body.collision_mesh.empty()) continue;
      const auto path = std::filesystem::path(index->game_root()) / body.collision_mesh;
      const auto source = load_scx_geometry(path.string());
      if (!source.info.ok || source.indices.empty()) continue;
      const auto frame = part_pose_matrix(body.pose);
      const auto first = static_cast<uint32_t>(mesh.positions.size() / 3);
      for (size_t i = 0; i + 2 < source.positions.size(); i += 3) {
        for (int axis = 0; axis < 3; ++axis)
          mesh.positions.push_back(frame[12 + axis] + source.position_scale *
              (frame[axis] * source.positions[i] +
               frame[4 + axis] * source.positions[i + 1] +
               frame[8 + axis] * source.positions[i + 2]));
      }
      for (uint32_t i : source.indices) mesh.indices.push_back(first + i);
    }
    update_collision_bounds(mesh);
  }
  return rec.deformed_collision ? *rec.deformed_collision : *rec.collision;
}

void publish_deformation(Object* part, Record& rec) {
  using physics::Vec3;
  part_collision_mesh(rec);
  if (!rec.collision->indices.empty()) {
    if (!rec.deformed_collision)
      rec.deformed_collision = std::make_unique<ScxGeometry>(*rec.collision);
    const auto& source = rec.collision->positions;
    auto& target = rec.deformed_collision->positions;
    for (size_t i = 0; i + 2 < source.size(); i += 3) {
      const Vec3 point{source[i], source[i + 1], source[i + 2]};
      const Vec3 moved = point + rec.deformation->displacement(point);
      target[i] = float(moved.x);
      target[i + 1] = float(moved.y);
      target[i + 2] = float(moved.z);
    }
    update_collision_bounds(*rec.deformed_collision);
  }
  for (auto* render : rec.renders) {
    auto* node = gameref_get(render);
    if (!node || node->light_flags) continue;
    auto mesh = scene_instances_geometry(static_cast<uint32_t>(node->type_id));
    if (!mesh) continue;
    const auto m = node_local(render);
    const Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    const Vec3 origin{m[12], m[13], m[14]};
    auto deformed = std::make_shared<ScxGeometry>(*mesh);
    const double scale = mesh->position_scale;
    if (!(scale > 0)) continue;
    double maximum = 0;
    for (size_t i = 0; i + 2 < mesh->positions.size(); i += 3) {
      const Vec3 p = origin + (x * mesh->positions[i] + y * mesh->positions[i + 1] +
                               z * mesh->positions[i + 2]) * scale;
      const Vec3 movement = rec.deformation->displacement(p);
      maximum = std::max(maximum, movement.dot(movement));
      const Vec3 delta = movement / scale;
      deformed->positions[i] += float(delta.dot(x));
      deformed->positions[i + 1] += float(delta.dot(y));
      deformed->positions[i + 2] += float(delta.dot(z));
      if (movement.dot(movement) > 1e-10 && i + 2 < mesh->normals.size()) {
        const Vec3 n = (x * mesh->normals[i] + y * mesh->normals[i + 1] +
                        z * mesh->normals[i + 2]).normalized();
        const Vec3 u = n.cross(std::abs(n.y) < .9 ? Vec3{0, 1, 0}
                                                 : Vec3{1, 0, 0}).normalized();
        const Vec3 v = n.cross(u);
        constexpr double offset = .005;
        const Vec3 du = u * offset + rec.deformation->displacement(p + u * offset) - movement;
        const Vec3 dv = v * offset + rec.deformation->displacement(p + v * offset) - movement;
        const Vec3 normal = du.cross(dv).normalized();
        if (normal.dot(normal) > .5) {
          deformed->normals[i] = float(normal.dot(x));
          deformed->normals[i + 1] = float(normal.dot(y));
          deformed->normals[i + 2] = float(normal.dot(z));
        }
      }
    }
    node->deformed_geometry = maximum > 1e-6 ? std::move(deformed) : nullptr;
    node->deformed_type = node->type_id;
  }
  for (const auto& [slot, child] : rec.children)
    if (const auto* installed = record(child); installed && installed->config->category == 2)
      place_child(part, slot, false);
}

void restore_deformation(Object* part, Record& rec) {
  if (!rec.deformation) return;
  rec.deformation.reset();
  rec.deformed_collision.reset();
  rec.deformation_upload_ticks = 0;
  for (auto* render : rec.renders)
    if (auto* node = gameref_get(render)) node->deformed_geometry.reset();
  for (const auto& [slot, child] : rec.children)
    if (const auto* installed = record(child); installed && installed->config->category == 2)
      place_child(part, slot, false);
}

struct PanelContact {
  physics::Vec3 minimum, maximum, point;
  double distance = 1e9;
};

PanelContact panel_contact(Record& rec, physics::Vec3 impact) {
  using physics::Vec3;
  PanelContact result;
  result.minimum = {1e9, 1e9, 1e9};
  result.maximum = -result.minimum;
  const auto grow = [&](Vec3 p) {
    result.minimum = {std::min(result.minimum.x, p.x), std::min(result.minimum.y, p.y),
                      std::min(result.minimum.z, p.z)};
    result.maximum = {std::max(result.maximum.x, p.x), std::max(result.maximum.y, p.y),
                      std::max(result.maximum.z, p.z)};
  };
  const auto& physical = part_collision_mesh(rec);
  if (!physical.indices.empty()) {
    result.distance = scene_mesh_nearest(physical, impact, result.point);
    const auto& box = physical.info;
    grow({box.bbox_min[0], box.bbox_min[1], box.bbox_min[2]});
    grow({box.bbox_max[0], box.bbox_max[1], box.bbox_max[2]});
    return result;
  }
  for (auto* render : rec.renders) {
    const auto* node = gameref_get(render);
    if (!node || node->light_flags) continue;
    const auto mesh = node->deformed_geometry && node->deformed_type == node->type_id
                          ? node->deformed_geometry
                          : scene_instances_geometry(static_cast<uint32_t>(node->type_id));
    if (!mesh) continue;
    const auto m = node_local(render);
    const Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    const Vec3 origin{m[12], m[13], m[14]};
    for (size_t i = 0; i + 2 < mesh->positions.size(); i += 3) {
      const Vec3 p = origin + (x * mesh->positions[i] + y * mesh->positions[i + 1] +
                               z * mesh->positions[i + 2]) * mesh->position_scale;
      grow(p);
      const Vec3 difference = p - impact;
      const double distance = difference.dot(difference);
      if (distance < result.distance) {
        result.distance = distance;
        result.point = p;
      }
    }
  }
  result.distance = std::sqrt(result.distance);
  return result;
}

void damage_chassis(Object* chassis, Record& rec, const physics::VehicleDynamics::Impact& hit,
                    bool player, std::vector<Object*>& released) {
  using physics::Vec3;
  if (hit.speed < 2) return;
  const auto& options = host_game_options();
  const double multiplier = rec.damage_multiplier >= 0 ? rec.damage_multiplier
                            : player ? options.player_damage : 1.f;
  if (multiplier <= 0) return;
  const double deformation = multiplier * options.deformation / 1.6;
  const double internal = multiplier * options.internal_damage;
  const auto& body = rec.dynamics.body;
  const Vec3 local_hit = body.center + body.orientation.inverse_rotate(hit.point - body.position);
  const Vec3 normal = body.orientation.inverse_rotate(hit.normal);
  if (deformation > 0) {
    if (!rec.deformation)
      rec.deformation = std::make_unique<physics::DeformationCage>(
          body.minimum, body.maximum, body.mass * .08);
    rec.deformation->impact(local_hit, normal, hit.speed * deformation, .85);
  }
  rec.state.tear = std::max(0.f, rec.state.tear - float((hit.speed - 2) * .014 * internal));
  const auto inverse = part_matrix_inverse_rigid(world_matrix(chassis));
  for (const auto& installed : part_installed_tree(chassis)) {
    auto* part = record(installed.object);
    if (part == &rec || part->config->category != 2) continue;
    std::string file = std::filesystem::path(part->config->source).stem().string();
    std::transform(file.begin(), file.end(), file.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool hood = file.find("hood") != std::string::npos;
    const bool panel = hood || file.find("bumper") != std::string::npos ||
                       file.find("door") != std::string::npos ||
                       file.find("trunk") != std::string::npos ||
                       file.find("quarterpanel") != std::string::npos ||
                       file.find("sideskirt") != std::string::npos;
    const bool rigid_shell = file.find("headlight") != std::string::npos ||
                             file.find("taillight") != std::string::npos ||
                             file.find("window") != std::string::npos ||
                             file.find("windshield") != std::string::npos;
    if (!panel && (!rigid_shell || part_collision_mesh(*part).indices.empty())) continue;
    const auto m = part_matrix_multiply(inverse, world_matrix(installed.object));
    const Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    const Vec3 offset = local_hit - Vec3{m[12], m[13], m[14]};
    const auto contact = panel_contact(*part, {offset.dot(x), offset.dot(y), offset.dot(z)});
    const double speed = hit.speed * std::clamp(1 - contact.distance / .65, 0., 1.);
    if (speed < 2) continue;
    if (!panel) {
      const double force = part->config->mass * speed * internal / physics::VehicleDynamics::timestep;
      part->state.tear = std::min(part->state.tear, float(std::max(0., 1 - force / 1800)));
      installed.object->fields["tear"] = Value::make_float(part->state.tear);
      if (part->state.tear <= 0) released.push_back(installed.object);
      continue;
    }
    if (deformation <= 0) continue;
    if (!part->deformation) {
      Vec3 mount;
      for (const auto& slot : part->config->slots)
        if (slot.id == installed.own_slot) {
          mount = {slot.pose.position[0], slot.pose.position[1], slot.pose.position[2]};
          break;
        }
      part->deformation = std::make_unique<physics::DeformationCage>(
          contact.minimum, contact.maximum, std::max(2.f, part->config->mass), mount, hood);
    }
    part->deformation->impact(contact.point, {normal.dot(x), normal.dot(y), normal.dot(z)},
                              speed * deformation, .65);
  }
}

void detach_panel(Object* part, Record& rec, Object* chassis, Record& vehicle) {
  using physics::Vec3;
  const auto* chassis_node = gameref_get(chassis);
  auto* node = gameref_get(part);
  PartMatrix frame, parent_frame;
  if (!chassis_node || !node || !chassis_node->parent ||
      !scene_instances_script_matrix(part, frame.data()) ||
      !scene_instances_script_matrix(chassis_node->parent, parent_frame.data()))
    return;
  const auto bounds = panel_contact(rec, {});
  if (bounds.distance > 1e4) return;
  void* const world_parent = chassis_node->parent;
  auto& loose = loose_panels[part];
  loose.vehicle = gameref_make_live(chassis);
  auto& body = loose.body;
  double mass = 0;
  for (const auto& item : part_installed_tree(part)) mass += record(item.object)->config->mass;
  const Vec3 padding{.005, .005, .005};
  body.configure(std::max(2., mass), bounds.minimum - padding, bounds.maximum + padding,
                 (bounds.minimum + bounds.maximum) * .5);
  body.set_frame(frame);
  const auto& carrier = vehicle.dynamics.body;
  loose.carrier_frame = carrier.frame();
  body.velocity = carrier.point_velocity(body.position);
  const Vec3 angular_velocity = carrier.omega();
  const Vec3 size = body.maximum - body.minimum;
  const Vec3 angular = body.orientation.inverse_rotate(angular_velocity);
  body.momentum = body.orientation.rotate(Vec3{
      angular.x * (size.y * size.y + size.z * size.z),
      angular.y * (size.x * size.x + size.z * size.z),
      angular.z * (size.x * size.x + size.y * size.y)} * (body.mass / 12));
  if (runtime)
    for (const auto& descendant : part_installed_tree(part))
      runtime->invoke_by_name(chassis->class_name, "rempart",
          {Value::make_obj(chassis), Value::make_obj(descendant.object)}, false);
  if (auto* parent = record(rec.state.parent))
    parent->children.erase(rec.state.parent_slot);
  rec.state.parent = nullptr;
  rec.state.parent_slot = rec.state.own_slot = 0;
  rec.state.tear = 0;
  part->fields["tear"] = Value::make_float(0);
  gameref_set_parent(part, world_parent);
  const auto local = part_matrix_multiply(part_matrix_inverse_rigid(parent_frame), frame);
  node = gameref_get(part);
  std::copy(local.begin(), local.end(), node->local_matrix);
  node->matrix_override = true;
  relink_assembly(chassis);
  refresh_chassis(chassis);
}

// SLRR_rev triangle-surface.ts. Sweep in the carrier's moving frame so a
// stationary fragment can also be hit by the car.
bool collide_loose_with_car(physics::RigidBody& body,
                            const std::array<physics::Vec3, 8>& previous,
                            physics::Vec3 before, Object* chassis, Record& vehicle,
                            const PartMatrix& previous_carrier) {
  using physics::Vec3;
  constexpr double radius = .035;
  const auto transform = [](const PartMatrix& frame, Vec3 p, double w) {
    return Vec3{frame[0] * p.x + frame[4] * p.y + frame[8] * p.z + frame[12] * w,
                frame[1] * p.x + frame[5] * p.y + frame[9] * p.z + frame[13] * w,
                frame[2] * p.x + frame[6] * p.y + frame[10] * p.z + frame[14] * w};
  };
  auto& carrier = vehicle.dynamics.body;
  const auto carrier_frame = carrier.frame();
  const auto inverse_carrier = part_matrix_inverse_rigid(carrier_frame);
  const auto inverse_previous = part_matrix_inverse_rigid(previous_carrier);
  const auto inverse_script = part_matrix_inverse_rigid(world_matrix(chassis));
  const auto corners = body.corners();
  SceneSphereHit hit;
  Vec3 point, normal;
  for (const auto& installed : part_installed_tree(chassis)) {
    auto* part = record(installed.object);
    if (part != &vehicle && part->config->category != 2) continue;
    const auto& mesh = part_collision_mesh(*part);
    if (mesh.indices.empty()) continue;
    const auto relative = part_matrix_multiply(inverse_script, world_matrix(installed.object));
    const auto inverse_relative = part_matrix_inverse_rigid(relative);
    const auto frame = part_matrix_multiply(carrier_frame, relative);
    const auto from_frame = part_matrix_multiply(inverse_relative, inverse_previous);
    const auto to_frame = part_matrix_multiply(inverse_relative, inverse_carrier);
    for (size_t i = 0; i < corners.size(); ++i) {
      const auto start = transform(from_frame, previous[i], 1);
      const auto travel = transform(to_frame, corners[i], 1) - start;
      if (!scene_mesh_sweep_sphere(mesh, start, travel, radius, hit)) continue;
      normal = transform(frame, {hit.normal[0], hit.normal[1], hit.normal[2]}, 0);
      point = transform(frame, start + travel * hit.fraction, 1) -
              normal * (radius - hit.penetration);
    }
  }
  if (hit.fraction >= 1) return false;
  body.position = before + (body.position - before) * hit.fraction +
                  normal * (hit.penetration + .002);
  body.contact(point, normal, .12, .55, &carrier);
  return true;
}

void advance_loose_panels() {
  using physics::Vec3;
  for (auto it = loose_panels.begin(); it != loose_panels.end();) {
    auto* part = it->first;
    auto& loose = it->second;
    auto* node = gameref_get(part);
    auto* owner = gameref_find_live(loose.vehicle);
    PartMatrix parent_frame;
    if (!node || !owner || !node->parent || owner->parent != node->parent ||
        !scene_instances_script_matrix(node->parent, parent_frame.data())) {
      it = loose_panels.erase(it);
      part_destroy(part);
      continue;
    }
    ++it;
    auto* chassis = static_cast<Object*>(owner->key);
    auto* vehicle = record(chassis);
    if (!vehicle || !vehicle->physics_ready) continue;
    const auto previous_carrier = loose.carrier_frame;
    loose.carrier_frame = vehicle->dynamics.body.frame();
    constexpr unsigned rest_ticks = physics::VehicleDynamics::update_frequency / 2;
    const bool sleeping = loose.quiet_ticks >= rest_ticks;
    if (sleeping && previous_carrier == loose.carrier_frame) continue;
    auto& body = loose.body;
    const Vec3 before = body.position;
    const auto previous = body.corners();
    if (!sleeping) body.integrate(kPhysicsTick);
    const bool carrier_contact = collide_loose_with_car(
        body, previous, before, chassis, *vehicle, previous_carrier);
    if (sleeping && !carrier_contact) continue;
    const Vec3 movement = body.position - before;
    const Vec3 size = body.maximum - body.minimum;
    const float radius = static_cast<float>(std::clamp(std::min({size.x, size.y, size.z}) * .5,
                                                       .03, .2));
    const float from[] = {float(before.x), float(before.y), float(before.z)};
    const float delta[] = {float(movement.x), float(movement.y), float(movement.z)};
    SceneSphereHit wall;
    if (scene_instances_sweep_sphere(from, delta, radius, wall, .65f)) {
      const Vec3 normal{wall.normal[0], wall.normal[1], wall.normal[2]};
      body.position = before + movement * wall.fraction + normal * .002;
      body.contact(body.position - normal * radius, normal);
    }
    double depth = 0;
    Vec3 correction;
    for (const auto& point : body.corners()) {
      SceneFloorHit floor;
      if (!scene_instances_sample_floor(float(point.x), float(point.z),
          float(std::max(before.y, body.position.y) + .25), part, floor)) continue;
      const double overlap = floor.height - point.y;
      if (overlap <= 0) continue;
      const Vec3 normal{floor.normal[0], floor.normal[1], floor.normal[2]};
      body.contact(point, normal, .12, .75);
      if (overlap > depth) {
        depth = overlap;
        correction = normal;
      }
    }
    if (depth > 0) {
      body.position += correction * (depth + .001);
    }
    const bool quiet = !carrier_contact && depth > 0 &&
        body.velocity.dot(body.velocity) < .0025 && body.omega().dot(body.omega()) < .01;
    loose.quiet_ticks = quiet ? loose.quiet_ticks + 1 : 0;
    if (loose.quiet_ticks >= rest_ticks) body.velocity = body.momentum = {};
    const auto local = part_matrix_multiply(part_matrix_inverse_rigid(parent_frame), body.frame());
    std::copy(local.begin(), local.end(), node->local_matrix);
  }
}

bool advance_deformation(Object* part, Record& rec, unsigned steps) {
  using physics::Vec3;
  PartMatrix frame;
  if (!scene_instances_script_matrix(part, frame.data())) return false;
  const Vec3 x{frame[0], frame[1], frame[2]}, y{frame[4], frame[5], frame[6]},
             z{frame[8], frame[9], frame[10]}, origin{frame[12], frame[13], frame[14]};
  return rec.deformation->advance(steps, [&](Vec3 position, Vec3& point, Vec3& normal) {
    const Vec3 world = origin + x * position.x + y * position.y + z * position.z;
    SceneFloorHit floor;
    // Use a narrow band so overhead surfaces cannot snap a node through a deck.
    if (!scene_instances_sample_floor(float(world.x), float(world.z),
                                      float(world.y + .12), part, floor))
      return false;
    const Vec3 n{floor.normal[0], floor.normal[1], floor.normal[2]};
    const Vec3 surface = Vec3{world.x, floor.height, world.z} + n * .015 - origin;
    point = {surface.dot(x), surface.dot(y), surface.dot(z)};
    normal = {n.dot(x), n.dot(y), n.dot(z)};
    return true;
  });
}

void advance_chassis_tick() {
  advance_loose_panels();
  std::vector<Object*> released;
  const auto* target = gameref_find_live(controlled_vehicle);
  Object* controlled = target ? as_part(static_cast<Object*>(target->key)) : nullptr;
  for (auto &entry : records) {
    Object *chassis = entry.first;
    auto &rec = entry.second;
    if (rec.config->native_kind != "car" && rec.deformation && rec.deformation->active()) {
      if (advance_deformation(chassis, rec, physics::VehicleDynamics::frequency /
                                           physics::VehicleDynamics::update_frequency)) {
        if (++rec.deformation_upload_ticks >= 3 || !rec.deformation->active()) {
          publish_deformation(chassis, rec);
          rec.deformation_upload_ticks = 0;
        }
        rec.state.tear = std::min(rec.state.tear, rec.deformation->condition());
        chassis->fields["tear"] = Value::make_float(rec.state.tear);
        if (rec.state.parent && rec.deformation->released()) released.push_back(chassis);
      }
    }
    rec.engine_running = false;
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
    configure_drive(chassis, rec, chassis == controlled);
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
    // Two fixed 120 Hz substeps per host vehicle update.
    const auto before = rec.dynamics.body.position;
    const auto velocity_before = rec.dynamics.body.velocity;
    const auto steps = rec.dynamics.advance(kPhysicsTick, terrain);
    if (rec.physics_released && rec.dynamics.drive.torque) {
      const auto wall = collide_chassis(rec, before);
      const auto& ground = rec.dynamics.ground_impact;
      damage_chassis(chassis, rec, wall.speed >= ground.speed ? wall : ground,
                     chassis == controlled, released);
      if (chassis == controlled && steps) {
        const auto& options = host_game_options();
        const auto& body = rec.dynamics.body;
        const auto local_acceleration = body.orientation.inverse_rotate(
            (body.velocity - velocity_before) / (steps * physics::VehicleDynamics::timestep));
        const physics::Vec3 lean{
            std::clamp(rec.steering_input * .045 * options.head_steer -
                           local_acceleration.x * .003 * options.head_acceleration, -.15, .15),
            0,
            std::clamp(body.velocity.length() * .0015 * options.head_velocity -
                           local_acceleration.z * .004 * options.head_acceleration, -.15, .15)};
        rec.cockpit_offset += (lean - rec.cockpit_offset) * -std::expm1(-kPhysicsTick / .14);
      }
      const auto delta = rec.dynamics.body.position - before;
      rec.mileage += static_cast<float>(std::hypot(delta.x, delta.z) * .1);
    }
    rec.engine_running = rec.dynamics.drive.torque && rec.dynamics.engine_running;
    if (steps)
      dynamics_sync(chassis, rec, parent);
    else
      rec.published_frame = local;
    if (rec.deformation && rec.deformation->active() && advance_deformation(chassis, rec, steps)) {
      rec.deformation_upload_ticks += steps;
      if (rec.deformation_upload_ticks >= physics::VehicleDynamics::frequency / 20 ||
          !rec.deformation->active()) {
        publish_deformation(chassis, rec);
        chassis->fields["tear"] = Value::make_float(rec.state.tear);
        rec.deformation_upload_ticks = 0;
      }
    }
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
        // Keep the quarter-second settling window across solver frequencies.
        if (rec.physics_quiet_ticks >= physics::VehicleDynamics::frequency / 4) {
          rec.physics_pending_placement = false;
          rec.dynamics.body.velocity = {};
          rec.dynamics.body.momentum = {};
        }
      }
    }
  }
  for (auto* part : released)
    if (auto* panel = record(part))
      if (auto* chassis = car(part))
        if (auto* vehicle = record(chassis))
          detach_panel(part, *panel, chassis, *vehicle);
}
} // namespace

void part_physics_advance(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0)
    throw std::invalid_argument("invalid vehicle elapsed time");
  // Hinges also animate while the chassis physics is suspended in the garage.
  // Advance a normalized phase so reversing mid-motion preserves the pose.
  const float hinge_step = static_cast<float>(seconds / 0.5);
  for (auto& entry : records) {
    auto& rec = entry.second;
    const float target = rec.flap ? 1.f : 0.f;
    if (!rec.state.parent || rec.flap_progress == target) continue;
    PartMatrix world;
    if (!scene_instances_script_matrix(entry.first, world.data())) continue;
    rec.flap_progress = rec.flap
                            ? std::min(1.f, rec.flap_progress + hinge_step)
                            : std::max(0.f, rec.flap_progress - hinge_step);
    place_child(rec.state.parent, rec.state.parent_slot);
  }

  // Ignore excess time after a long stall so the next frame can still render.
  physics_elapsed += std::min(seconds, kPhysicsTick * kMaxPhysicsTicksPerFrame);
  constexpr double tolerance = kPhysicsTick * 1e-10;
  if (physics_elapsed + tolerance < kPhysicsTick) return;

  scene_instances_prepare_physics();
  while (physics_elapsed + tolerance >= kPhysicsTick) {
    advance_chassis_tick();
    physics_elapsed = std::max(0.0, physics_elapsed - kPhysicsTick);
  }
  for (auto &[key, rec] : records) {
    if (rec.config->native_kind == "car")
      update_engine_audio(rec, rec.engine_running);
  }
}
void part_set_hud_preview(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "[hud] cannot open %s\n", path.c_str());
    return;
  }
  std::ostringstream text;
  text << input.rdbuf();
  hud_preview = parse_part_config(text.str()).hud;
  hud_preview_directory = std::filesystem::absolute(path).parent_path().string();
}

bool part_vehicle_hud(VehicleHudState &state) {
  if (!runtime) return false;
  const Value player = runtime->get_static("java.game.GameLogic", "player");
  if (player.tag != ValueTag::Obj || !player.v.o) return false;
  Object *osd = part_field(player.v.o, "osd");
  const auto *viewport = osd ? gameref_get(part_field(osd, "vp")) : nullptr;
  if (!field_int(player.v.o, "_hostVehicleHud", viewport && viewport->vp_active))
    return false;
  state.owner = osd;
  const auto *target = gameref_find_live(controlled_vehicle);
  const auto *rec = target ? record(as_part(static_cast<Object *>(target->key))) : nullptr;
  if (!rec) return true; // The GPS frame also belongs to the empty HUD.
  state.layout = hud_preview_directory.empty() ? &rec->config->hud : &hud_preview;
  state.preview_directory = hud_preview_directory.empty() ? nullptr : &hud_preview_directory;
  state.rpm = rec->dynamics.drive.torque ? rec->dynamics.engine_omega * 9.54929658551372 : 0;
  state.speed = rec->dynamics.body.velocity.length() * 3.6;
  state.mileage = rec->mileage * .01;
  state.unit_scale = 1000.f / host_game_options().unit_meters;
  state.condition = std::min(rec->state.wear, rec->state.tear);
  state.nitro = comp(as_part(static_cast<Object *>(target->key)), "tank_nitro") > 0 ? 1.f : 0.f;
  for (const auto &[slot, child] : rec->children) {
    const auto *body = record(child);
    if (body && body->config->category == 2)
      state.condition = std::min({state.condition, body->state.wear, body->state.tear});
    if (rec->engine_running)
      if (Object *dyno = part_field(child, "dynodata"))
        state.turbo = dyno_boost_fraction(dyno, static_cast<float>(state.rpm)) *
                      static_cast<float>(rec->dynamics.drive.throttle);
  }
  state.gear = rec->physics_released ? rec->dynamics.gear : 0;
  state.handbrake = rec->dynamics.drive.handbrake > 0;
  return true;
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
bool part_camera_pose(void* car, int camera, float* matrix) {
  auto* chassis = as_part(static_cast<Object*>(car));
  const auto* rec = record(chassis);
  PartMatrix frame;
  if (!rec || !scene_instances_script_matrix(chassis, frame.data())) return false;
  if (camera >= 0) {
    if (static_cast<size_t>(camera) >= rec->config->cameras.size()) return false;
    frame = part_matrix_multiply(frame, part_pose_matrix(rec->config->cameras[camera]));
    const auto* controlled = gameref_find_live(controlled_vehicle);
    if (controlled && rec->physics_released &&
        as_part(static_cast<Object*>(controlled->key)) == chassis) {
      const auto offset = rec->dynamics.body.orientation.rotate(rec->cockpit_offset);
      frame[12] += float(offset.x);
      frame[13] += float(offset.y);
      frame[14] += float(offset.z);
    }
  }
  std::copy(frame.begin(), frame.end(), matrix);
  return true;
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
  for (auto &[key, rec] : records) release_side(rec);
  records.clear();
  loose_panels.clear();
  controlled_vehicle = 0;
  hud_preview = {};
  hud_preview_directory.clear();
  physics_elapsed = 0;
  sfx_items.clear();
  configurations.clear();
  slot_aliases.clear();
  index = &resources;
  jvm.natives().register_method("java.game.Vehicle", "getSpeedSquare", "()F",
    [](Object *self, const std::vector<Value> &) {
      const auto *rec = record(as_part(self));
      return Value::make_float(rec && rec->physics_ready
          ? static_cast<float>(rec->dynamics.body.velocity.dot(rec->dynamics.body.velocity))
          : 0.f);
    });
  const std::string owner = "java.game.parts.Part";
  auto bind = [&](const char *name, const char *signature, NativeFn fn) {
    jvm.natives().register_method(owner, name, signature, std::move(fn));
  };
  auto info = [](Object *self, const std::vector<Value> &args) {
    auto *state = gameref_get(self);
    auto *p = record(self);
    const int query = number(args, 0);
    if (query == 25 && state && state->alias == "controller") {
      const float axis = host_input_controller_axis(
          static_cast<Object*>(state->key), number(args, 1));
      return Value::make_int(static_cast<int>(std::clamp(axis, -1.f, 1.f) * 128.f));
    }
    if (!state)
      return Value::make_int(0);
    if (query == 48) {
      const auto* chassis = record(as_part(self));
      return Value::make_int(chassis ? static_cast<int>(chassis->config->cameras.size()) : 0);
    }
    if (query == 24 && as_part(self) && runtime) {
      auto* target = gameref_find_live(controlled_vehicle);
      auto* controller = host_input_player_controller(*runtime);
      if (controller && target && as_part(static_cast<Object*>(target->key)) == as_part(self))
        return Value::make_int(gameref_make_live(controller));
    }
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
      // Use the same service exception as installation for springs and
      // dampers, so a wheel left on the car cannot prevent their return.
      std::unordered_set<Object *> seen;
      auto *current = p;
      while (current && current->state.parent) {
        if (!seen.insert(current->state.object).second)
          return Value::make_int(-1);
        auto *parent = record(current->state.parent);
        if (!parent)
          break;
        auto blocked = parent->slot_status.find(current->state.parent_slot);
        if (blocked != parent->slot_status.end() && blocked->second &&
            !(blocked->second == 1 && wheel_on_suspension_service(
                                         *parent, current->state.parent_slot,
                                         current->state.object)))
          return Value::make_int(-1);
        current = parent;
      }
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
          if (auto *p = record(s); p && std::isfinite(value)) {
            p->state.*field = value;
            if (field == &InstalledPart::tear && value >= 1)
              restore_deformation(canonical(s), *p);
          }
          return Value::make_float(value);
        });
    jvm.natives().register_method(
        owner, "set" + property.first, "(F)V",
        [field = property.second](Object *s, const std::vector<Value> &a) {
          if (auto *p = record(s)) {
            p->state.*field = std::clamp(scalar(a, 0), 0.f, 1.f);
            if (field == &InstalledPart::tear && p->state.tear >= 1)
              restore_deformation(canonical(s), *p);
          }
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
    if (!p || !p->config->texture)
      return Value::make_int(old);
    const auto id = static_cast<uint32_t>(number(a, 0));
    // 0, and anything that is not a texture record, leaves the part as it
    // is. A loaded car writes 0 for an unpainted piece and a factory color
    // for the rest; clearing the render there made the panels vanish.
    const RpkEntry *entry = id && index ? index->resolve(id) : nullptr;
    if (!entry || entry->typeof_ != 7)
      return Value::make_int(old);
    p->state.texture = static_cast<int>(id);
    for (auto *render : p->renders)
      if (auto *node = gameref_get(render))
        node->paint_texture = p->state.texture;
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
    auto *p = record(s);
    if (!p)
      return Value::make_int(1);
    const auto id = static_cast<uint32_t>(number(a, 0));
    // The cfg mesh is already the render's mesh. Pointing the render at
    // the bare scx drops its textures and its flags, so a loaded hood or
    // door is no longer drawn. 0 and a non-mesh id are ignored.
    const RpkEntry *entry = id && index ? index->resolve(id) : nullptr;
    if (!entry || entry->typeof_ != 5 || id == p->config->mesh)
      return Value::make_int(1);
    p->mesh_set = true;
    p->mesh_id = id;
    retarget(*p, p->mesh_id);
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
         if (!found.parent) {
           auto* child = record(s);
           Object* root = as_part(object(a, 0));
           std::fprintf(stderr, "[part] install_OK refused class=%s cfg=%s root=%s\n",
                        s ? s->class_name.c_str() : "null",
                        child ? child->config->source.c_str() : "missing",
                        root ? root->class_name.c_str() : "null");
           int candidates = 0;
           if (child)
             for (const auto& installed : part_installed_tree(root)) {
               auto* parent = record(installed.object);
               if (!parent) continue;
               for (const auto& mount : parent->config->slots)
                 for (const auto& mate : child->config->slots)
                   if (matches(*parent, mount, *child, mate) && candidates++ < 12) {
                     auto blocked = parent->slot_status.find(mount.id);
                     std::fprintf(stderr, "[part] candidate cfg=%s slot=%d mate=%d occupied=%d disabled=%d\n",
                                  parent->config->source.c_str(), mount.id, mate.id,
                                  parent->children.count(mount.id) != 0,
                                  blocked == parent->slot_status.end() ? 0 : blocked->second);
                   }
             }
           std::fprintf(stderr, "[part] compatible candidates=%d\n", candidates);
           return Value::make_obj(nullptr);
         }
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
    const auto found = sfx_items.find(s);
    if (found != sfx_items.end()) {
      for (auto &item : found->second) host_sfx_stop(item.voice);
      sfx_items.erase(found);
    }
    return Value::make_void();
  });
  sfx("getItems", "()I", [](Object *s, const std::vector<Value> &) {
    auto it = sfx_items.find(s);
    return Value::make_int(
        it == sfx_items.end() ? 0 : static_cast<int>(it->second.size()));
  });
  sfx("clear", "()V", [](Object *s, const std::vector<Value> &) {
    auto it = sfx_items.find(s);
    if (it != sfx_items.end()) {
      for (auto &item : it->second) host_sfx_stop(item.voice);
      it->second.clear();
    }
    return Value::make_void();
  });
  sfx("addItem", "(Ljava.util.resource.ResourceRef;FFFFF)V",
      [](Object *s, const std::vector<Value> &a) {
        auto it = sfx_items.find(s);
        if (it == sfx_items.end() || it->second.size() >= 16)
          return Value::make_void();
        SfxItem item;
        if (!index) return Value::make_void();
        auto path = index->payload_prop(res_id(object(a, 0)), "sourcefile");
        if (path.empty()) return Value::make_void();
        std::replace(path.begin(), path.end(), '\\', '/');
        item.path = (std::filesystem::path(index->game_root()) / path).string();
        item.pitch = scalar(a, 1, 1);
        item.pmin = scalar(a, 2, 0);
        item.pmax = scalar(a, 3, 0);
        item.vmin = scalar(a, 4, 0);
        item.vmax = scalar(a, 5, 0);
        if (item.pitch > 0 && item.pmax > item.pmin)
          it->second.push_back(std::move(item));
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

void part_visit_gc_roots(const GcRootVisitor& visit) {
  for (const auto& entry : records) {
    const auto& r = entry.second;
    visit(entry.first); visit(r.state.object); visit(r.state.parent); visit(r.physics_parent);
    for (const auto& child : r.children) visit(child.second);
    for (auto* render : r.renders) visit(render);
    for (const auto& link : r.links) visit(link.node);
    for (auto* sfx : r.sfx) visit(sfx);
  }
  for (const auto& entry : sfx_items) visit(entry.first);
}
} // namespace slrr
