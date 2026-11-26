#include "slrr/race_runtime.hpp"
#include "slrr/frame_profiler.hpp"
#include "slrr/gc_roots.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/pedestrian_lines.hpp"
#include "slrr/dyno_model.hpp"
#include "slrr/flap_hinge.hpp"
#include "slrr/gameref.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/host_input.hpp"
#include "slrr/deformation_cage.hpp"
#include "slrr/mass_properties.hpp"
#include "slrr/wheel_mount.hpp"
#include "slrr/host_sfx.hpp"
#include "slrr/osd_registry.hpp"
#include "slrr/paint_texture.hpp"
#include "slrr/jvm.hpp"
#include "slrr/light_resource.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/scene_ambience.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/scene_collision.hpp"
#include "slrr/scx_mesh.hpp"
#include "slrr/skeleton.hpp"
#include "slrr/brake_balance.hpp"
#include "slrr/skid_marks.hpp"
#include "slrr/steering_feedback.hpp"
#include "slrr/suspension_geometry.hpp"
#include "slrr/vehicle_dynamics.hpp"
#include "slrr/vehicle_contacts.hpp"
#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <thread>
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
  float relaxation = .3f;  // setRelaxation (patched Tyre), metres
  float pacejka[17] = {1.4f, 0, 1.49f,  0,   15.2f,  0,    0, 0, -1.f,
                       0,    0, 8000.f, 1.f, 0.015f, 0.4f, 0, 0};
  float force = 0, damp_bound = 0, damp_rebound = 0;
  float rest = 0, min_len = 0, max_len = 0;
  float ic[6]{};
  // Raw setInstantCenter arguments: offsets from the wheel centre. 10000 is
  // the stock "no instant centre" value Chassis.updatevariables writes.
  float ic_offset[6] = {10000, 10000, 10000, 10000, 10000, 10000};
  float brake = 0, hbrake = 0;
  // setBrakeSpec (patched DiscBrake, #446): pad compound, rotor type and
  // rotor diameter (m). A zero diameter: the stock scripts, no spec.
  float brake_pad = 0, brake_rotor = 0, brake_diameter = 0;
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
  bool external_motion = false;
  // SLRR_rev TrafficNative/TrafficVehicle distinguishes native stock templates
  // from full Java mechanical vehicles, independently of wheel mesh layout.
  bool native_vehicle = false;
  struct InlineWheel {
    Object *node;
    PartMatrix base, mesh;
    float radius, steer_factor;
    size_t corner;
    double spin = 0;
  };
  std::vector<InlineWheel> inline_wheels;
  // Chassis `cockpit_rpm` / `cockpit_speed` pins (#301). Not in `renders`:
  // they turn every frame, so deformation, lamp snapping and bounds skip them.
  struct Needle {
    Object *node = nullptr;
    CockpitGauge gauge = CockpitGauge::Rpm;
  };
  std::vector<Needle> needles;
  std::unique_ptr<physics::DeformationCage> deformation;
  // The floor under the chassis, kept across solver steps (#380). Only the
  // step that uses it touches it; the batch holds the same object.
  std::shared_ptr<SceneFloorPatch> floor_patch = std::make_shared<SceneFloorPatch>();
  std::unique_ptr<ScxGeometry> collision, deformed_collision;
  unsigned deformation_upload_ticks = 0;
  float steering_input = 0;
  float steering_request = 0, steering_correction = 0;
  float steering_help = 0, abs_strength = 0, asr_strength = 0;
  int transmission = 1;
  uint64_t driving_options_revision = 0;
  float damage_multiplier = -1.f;
  physics::Vec3 cockpit_offset;
  bool horn_down = false;
  int horn_slot = 0;
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
  // Latest intentional setSlotPos values, kept separate from damage overlays so
  // repair can restore a user pose even when both target the same slot.
  std::unordered_map<int, PoseOverride> slot_script_pose;
  std::vector<Object *> renders;
  struct LinkVisual {
    Object *node = nullptr;
    uint32_t resource = 0;
    std::vector<PartLink> rules;
    PartMatrix rest_pose{};
    // Skinned bone00/bone01 rig of the render type `rig_type`, -1 when absent.
    int32_t rig_type = 0;
    int bone_from = -1, bone_to = -1;
  };
  std::vector<LinkVisual> links;
  float max_wear = 1.f;
  // Car lights placed by this part's cfg: one headlight/brake group (#279)
  // and every authored NOS glow placement (#304), in part-local script space.
  struct Lamp {
    CarLightKind kind = CarLightKind::Headlight;
    uint32_t resource = 0, flags = 0;
    LightResource light;
    std::array<float, 3> position{}, direction{};
  };
  std::vector<Lamp> lamps;
  // Hinge of this part on its parent's CFG `flap` slot (flap_hinge.hpp).
  // The state belongs to one mount; a part moved or taken off starts latched.
  physics::FlapState flap;
  physics::FlapGeometry flap_geometry;
  Object *flap_parent = nullptr;
  int flap_slot = 0;
  bool flap_sized = false;
  // Hit directly this tick: the obstacle, not the carrier, sets its motion.
  bool flap_struck = false;
  bool flap_carrier_valid = false;
  physics::Vec3 flap_carrier_velocity, flap_carrier_omega;
  // Part-local bounds of the panel, its world collision box (flap_world_contact).
  bool flap_box = false;
  physics::Vec3 flap_box_min, flap_box_max;
  // Last world centre of that box, so a fast pass is swept, not stepped.
  bool flap_world_valid = false;
  physics::Vec3 flap_world_centre;
  // Seventh field of the create string, an index into each render's colors.
  bool colour_set = false;
  int colour_index = 0;
  bool mesh_set = false, render_set = false;
  uint32_t mesh_id = 0, render_id = 0;
  float sfx_a = 0, sfx_b = 0;
  float torque = 0, torque_scale = 0, ackermann = 0, mileage = 0;
  float cooling[3] = {10.f, 50.f, 0.01f};
  float exhaust_min = 0, steer_radius = 0, steer_r = 0, steer_z = 0;
  int nitro_sfx = 0, nitro_voice = 0;
  float nitro_pitch = 1;
  // Tyre slide loop and collision one-shots (update_slide_audio, play_impact_audio).
  int32_t slide_voice = 0;
  bool sharp_skid = false;
  std::chrono::steady_clock::time_point last_impact_sound{};
  // MWM keeps a native aggregate, initially empty; `reload` fills it.
  // Canister.actual is script save data, not the engine's consumption owner.
  double nitro_remaining = 0;
  float nitro_input = 0;
  bool nitro_requested = false, nitro_active = false, nitro_flame = false;
  bool nitro_sound_down = false;
  uint32_t nitro_random = 1;
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
  // Chassis only, 1 intact: each corner's wheel mount against settled
  // hardpoint travel (settle_wheel_mounts) and its strut against impact
  // energy (damage_wheel_mounts). Either at zero breaks the wheels off.
  std::array<float, 4> wheel_mount{1.f, 1.f, 1.f, 1.f};
  std::array<float, 4> wheel_strut{1.f, 1.f, 1.f, 1.f};
  // Settled cage displacement of each corner's hardpoints when its wheel was
  // fitted; only crush beyond it counts against the mount.
  std::array<double, 4> wheel_shift_base{};
  // Chassis only: the engine mount against impact energy
  // (damage_engine_mount). At zero the engine breaks off as a loose body,
  // thrown with `engine_release_velocity`. `engine_lost` keeps the chassis
  // colliding after its engine (and so its drive torque) is gone.
  float engine_mount = 1.f;
  Object *engine_release = nullptr;
  physics::Vec3 engine_release_velocity;
  bool engine_lost = false;
  bool dynamic_frames = false;
  bool physics_pending_placement = true;
  // start releases the body. Until then a chassis only settles onto a floor.
  // The catalog preview has none, so integrating gravity walks it out of frame.
  bool physics_released = false;
  unsigned physics_quiet_ticks = 0;
  std::array<PartMatrix, 4> travel_frame{}, steered_frame{}, rolling_frame{};
  std::array<bool, 4> geometry_solved{};
  physics::VehicleDynamics dynamics;
  bool physics_ready = false, physics_dirty = true, physics_suspended = false;
  bool body_support_dirty = true;
  bool physics_reported = false, physics_fault_reported = false;
  int reported_contacts = -1, physics_status_lines = 0;
  void *physics_parent = nullptr;
  PartMatrix published_frame{};
  // Body pose before the last host tick and the parent frame of that tick.
  // The drawn pose blends from it to the published one (publish_render_poses).
  bool render_blend = false;
  physics::Vec3 render_from_position;
  physics::Quaternion render_from_orientation;
  PartMatrix render_parent{};
  std::array<Object *, 3> sfx{};
  // Host engine loop of a native (script-less) car: one voice per diesel
  // sample, cross-faded by a speed-modelled RPM. See update_traffic_engine_audio.
  std::array<int32_t, 3> traffic_voice{};
  int32_t radio_voice = 0; // CFG `sfx_loop` of a native car (its radio)
  float traffic_rpm = 0;
  uint32_t traffic_audio_epoch = 0;
};
std::unordered_map<Object *, std::vector<SfxItem>> sfx_items;
std::unordered_map<Object *, Record> records;
// Pedal travel above which brake lamps light (#279).
constexpr float kBrakeLampMin = .05f;
// drive.handbrake at full pull: what a handbrake key has always given.
constexpr double kHandbrakeFull = .3;
// Weak, one-shot repair work consumed by the next part update. Stock Java may
// repair descendants before their parents, so links must not be relinked early.
std::unordered_set<Object *> repair_relink_pending;
struct LoosePanel {
  physics::RigidBody body;
  PartMatrix carrier_frame{};
  int32_t vehicle = 0;
  unsigned quiet_ticks = 0;
  // Native map placement: each render slot's CFG pose in the object frame,
  // parallel to Record::renders.
  std::vector<PartMatrix> render_offsets;
  physics::Vec3 rest_anchor;  // see map_object_quiet_ticks
  unsigned far_awake_ticks = 0;  // awake beyond car reach; see sim_loose_bodies
  // Ticks left during which a body released from inside the car (the engine)
  // skips the car contacts while it leaves; see leaving_carrier.
  unsigned exit_ticks = 0;
};
std::unordered_map<Object *, LoosePanel> loose_panels;
// Wheel parts whose corner mount failed when a chassis cage settled during
// a batch commit; commit_batch detaches them after the cages.
std::vector<Object *> settled_wheel_releases;
// Script PhysicsRef bodies released by setStatic(0), keyed by their GameRef.
struct FreeBody {
  bool configured = false;
  int32_t live_id = 0;  // guards against a reused key
  physics::RigidBody body;
  PartMatrix carrier_frame{};
  unsigned quiet_ticks = 0;
  physics::Vec3 rest_anchor;  // see map_object_quiet_ticks
  float pos[3] = {}, ypr[3] = {};  // last published pose; a change is setMatrix
};
std::unordered_map<void *, FreeBody> free_bodies;
std::unordered_set<uint32_t> invalid_dynamic_types;
int32_t controlled_vehicle = 0;

struct DriverSeat {
  Object* render = nullptr;
  // False when the render is the script's player.render. That object stays
  // alive after the car is released; only a render this host created is removed.
  bool owned = false;
  // Other copies of the same driver (the bind-pose Mitch parented to the map)
  // are hidden while this seat is active, then shown again on release.
  GameRefRenderMask duplicate_mask;
  // Gear-shift gesture: last gear seen and the physics tick it changed.
  int gear = -1;
  uint64_t shift_tick = 0;
  bool shifting = false;
};
std::unordered_map<Object*, DriverSeat> drivers;

struct PedKind {
  uint32_t render = 0;
  uint32_t anim = 0;
  PartPose pose{};
  float sole = 0.f;
  bool ready = false;
};
struct PedAgent {
  Object* render = nullptr;
  int kind = 0;
  float x = 0.f, z = 0.f, yaw = 0.f;
  bool placed = false;
  uint32_t sim_gen = 0;
  PedestrianWalker line;
};
struct PedCrowd {
  float density = 0.f;
  // Map value before the options slider (day 0.003, night 0.0005).
  float map_density = 0.f;
  std::vector<void*> types;
  std::vector<PedKind> kinds;
  std::vector<PedAgent> agents;
  bool announced = false;
  bool preloaded = false;
  // The whole radius is created once, when the map comes up. Later gaps
  // refill a few at a time as the car moves.
  bool stocked = false;
  int next_kind = 0;
  double refill_at = 0;
  double prune_at = 0;
  double report_at = 0;
};
std::unordered_map<void*, PedCrowd> crowds;
uint32_t ped_rng = 0x51ee5u;
float ped_backlog = 0.f;
VehicleHudLayout hud_preview;
std::string hud_preview_directory;
constexpr double kPhysicsTick = 1.0 / physics::VehicleDynamics::update_frequency;
// Host ticks run since start: places each tick on the solver-step grid (#380).
uint64_t host_tick_clock = 0;
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
// Painter textures belong to the parts that show them. One no live part
// uses any more is freed (a copied texture may be shared by two parts).
bool texture_in_use(uint32_t texture, const Object *except = nullptr) {
  for (const auto &[part, rec] : records)
    if (part != except && rec.state.texture == texture)
      return true;
  return false;
}
void assign_texture(Object *, Record &rec, uint32_t texture) {
  rec.state.texture = texture;
  for (auto *render : rec.renders)
    if (auto *node = gameref_get(render))
      node->paint_texture = texture;
  // Reclaim after script callbacks, not in the middle of a part rebuild.
}
void repair_part(Object *part, Record &rec);
bool flap_geometry_for(Object *part, Record &rec,
                       physics::FlapGeometry *out = nullptr);
void pose_wheels(Object *chassis, Record &rec);
bool contact_visible(Object *chassis);
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
// Part-assembly transform (not a generic scene-node transform). A RenderRef
// has no Record; compose its node_local matrix with its owning part instead.
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
    // Children in the order the parent's CFG declares their slots, then any
    // other slot by id (#383). `children` is a hash map: walking it as is
    // gave the first free slot in an arbitrary order, so the stock lists
    // (front left, front right, rear left, rear right) put a GT III's rear
    // rims and tyres on its left side and the front ones on its right.
    std::vector<Object *> ordered;
    ordered.reserve(p->children.size());
    for (const auto &mount : p->config->slots)
      if (const auto it = p->children.find(mount.id); it != p->children.end())
        ordered.push_back(it->second);
    if (ordered.size() < p->children.size()) {
      std::vector<std::pair<int, Object *>> rest;
      for (const auto &entry : p->children)
        if (std::find_if(p->config->slots.begin(), p->config->slots.end(),
                         [&](const auto &slot) { return slot.id == entry.first; }) ==
            p->config->slots.end())
          rest.push_back(entry);
      std::sort(rest.begin(), rest.end(),
                [](const auto &a, const auto &b) { return a.first < b.first; });
      for (const auto &entry : rest) ordered.push_back(entry.second);
    }
    for (Object *next : ordered)
      if (visit(visit, next))
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
// The part's own car lights (#279), from its cfg `render` light children.
void collect_lamps(Record &rec) {
  rec.lamps.clear();
  if (!index)
    return;
  bool head = false, brake = false;
  for (const auto &r : rec.config->renders) {
    if (r.wheel >= 0 || (r.flags & 0x40u) || !(r.flags & 0x105u) ||
        !is_light_resource(*index, r.resource))
      continue;
    const auto light = load_light_resource(*index, r.resource);
    if (!light)
      continue;
    Record::Lamp lamp;
    if (r.flags & 256u) {
      lamp.kind = CarLightKind::Nitro;
    } else if (!head && (r.flags & 4u) && light->type == LightResourceType::Spot) {
      lamp.kind = CarLightKind::Headlight;
      head = true;
    } else if (!brake && (r.flags & 1u)) {
      lamp.kind = CarLightKind::Brake;
      brake = true;
    } else {
      continue;
    }
    const auto frame = part_pose_matrix(r.pose);
    const auto &p = light->position, &d = light->direction;
    for (int i = 0; i < 3; ++i) {
      lamp.position[i] = frame[i] * p[0] + frame[4 + i] * p[1] +
                         frame[8 + i] * p[2] + frame[12 + i];
      lamp.direction[i] = frame[i] * d[0] + frame[4 + i] * d[1] + frame[8 + i] * d[2];
    }
    lamp.resource = r.resource;
    lamp.flags = r.flags;
    lamp.light = *light;
    rec.lamps.push_back(lamp);
  }
}
// Lamp glass (#284): the stock `_CITY`/`_BRAKE` meshes are authored about
// 1 cm in front of the lamp body, so the lit glass floated off the lens.
// Each glass vertex moves onto the nearest body triangle within
// kGlassSnapReach (measured along that triangle's plane, oriented by the
// glass normal) and kGlassSnapGap in front of it; vertices with no body
// triangle in reach take the mean move of the others, so the glass keeps its
// shape. The copy is the node's own geometry (deformed_geometry, which the
// deformation pass leaves alone on lamp nodes); the game files and the shared
// mesh cache are unchanged.
constexpr double kGlassSnapReach = .03, kGlassSnapGap = .0005;
// Lens triangles face within 60 degrees of the glass normal.
constexpr double kGlassLensAlignment = .5;
void snap_lamp_glass(Record &rec) {
  using physics::Vec3;
  struct Tri { Vec3 a, n, e0, e1; double d00, d01, d11, inv; };
  std::vector<Tri> body;
  for (auto *render : rec.renders) {
    const auto *node = gameref_get(render);
    if (!node || node->light_flags || node->empty) continue;
    const auto mesh = scene_instances_geometry(static_cast<uint32_t>(node->type_id));
    if (!mesh || !(mesh->position_scale > 0)) continue;
    const auto m = node_local(render);
    const double k = mesh->position_scale;
    const auto at = [&](uint32_t i) {
      const double x = mesh->positions[i * 3] * k, y = mesh->positions[i * 3 + 1] * k,
                   z = mesh->positions[i * 3 + 2] * k;
      return Vec3{m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
                  m[2] * x + m[6] * y + m[10] * z + m[14]};
    };
    for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
      Tri tri;
      tri.a = at(mesh->indices[t]);
      tri.e0 = at(mesh->indices[t + 1]) - tri.a;
      tri.e1 = at(mesh->indices[t + 2]) - tri.a;
      const Vec3 n = tri.e0.cross(tri.e1);
      const double length = std::sqrt(n.dot(n));
      if (!(length > 1e-10)) continue;
      tri.n = n / length;
      tri.d00 = tri.e0.dot(tri.e0); tri.d01 = tri.e0.dot(tri.e1); tri.d11 = tri.e1.dot(tri.e1);
      const double det = tri.d00 * tri.d11 - tri.d01 * tri.d01;
      if (!(std::abs(det) > 1e-20)) continue;
      tri.inv = 1 / det;
      body.push_back(tri);
    }
  }
  if (body.empty()) return;
  for (auto *render : rec.renders) {
    auto *node = gameref_get(render);
    if (!node || !node->light_flags || (node->light_flags & 256u)) continue;
    const auto mesh = scene_instances_geometry(static_cast<uint32_t>(node->type_id));
    if (!mesh || !(mesh->position_scale > 0) || mesh->normals.size() != mesh->positions.size())
      continue;
    const auto m = node_local(render);
    const Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    const Vec3 origin{m[12], m[13], m[14]};
    const double k = mesh->position_scale;
    const size_t count = mesh->positions.size() / 3;
    std::vector<Vec3> points(count), normals(count);
    for (size_t v = 0; v < count; ++v) {
      points[v] = origin + (x * mesh->positions[v * 3] + y * mesh->positions[v * 3 + 1] +
                            z * mesh->positions[v * 3 + 2]) * k;
      normals[v] = (x * mesh->normals[v * 3] + y * mesh->normals[v * 3 + 1] +
                    z * mesh->normals[v * 3 + 2]).normalized();
    }
    const std::vector<Vec3> stock = points;
    // The move that puts p kGlassSnapGap in front of the nearest lens
    // triangle: one roughly parallel to the glass (housing walls are not
    // lenses), within reach, whose plane projection holds p.
    const auto snap = [&](const Vec3 &p, const Vec3 &normal, Vec3 &move) {
      double best = kGlassSnapReach;
      bool hit = false;
      for (const auto &tri : body) {
        // Front of the lens is the side the glass faces.
        const Vec3 n = tri.n.dot(normal) < 0 ? tri.n * -1. : tri.n;
        if (n.dot(normal) < kGlassLensAlignment) continue;
        const double d = (p - tri.a).dot(n);
        if (std::abs(d) >= best) continue;
        const Vec3 q = p - n * d - tri.a;
        const double d20 = q.dot(tri.e0), d21 = q.dot(tri.e1);
        const double b1 = (tri.d11 * d20 - tri.d01 * d21) * tri.inv;
        const double b2 = (tri.d00 * d21 - tri.d01 * d20) * tri.inv;
        if (b1 < -.02 || b2 < -.02 || b1 + b2 > 1.04) continue;
        best = std::abs(d);
        move = n * (kGlassSnapGap - d);
        hit = true;
      }
      return hit;
    };
    // Pass 1 moves the vertices over a lens and the others by their mean;
    // later passes settle vertices that the mean brought over a lens.
    bool any = false;
    for (int pass = 0; pass < 3; ++pass) {
      Vec3 mean;
      size_t snapped = 0;
      std::vector<Vec3> moves(count);
      std::vector<char> found(count, 0);
      for (size_t v = 0; v < count; ++v)
        if ((found[v] = snap(points[v], normals[v], moves[v]))) { mean = mean + moves[v]; ++snapped; }
      if (!snapped) break;
      any = true;
      mean = mean / double(snapped);
      for (size_t v = 0; v < count; ++v)
        if (found[v]) points[v] = points[v] + moves[v];
        else if (pass == 0) points[v] = points[v] + mean;
    }
    if (!any) continue;
    auto copy = std::make_shared<ScxGeometry>(*mesh);
    for (size_t v = 0; v < count; ++v) {
      const Vec3 delta = (points[v] - stock[v]) / k;
      copy->positions[v * 3] += float(delta.dot(x));
      copy->positions[v * 3 + 1] += float(delta.dot(y));
      copy->positions[v * 3 + 2] += float(delta.dot(z));
    }
    node->deformed_geometry = std::move(copy);
    node->deformed_type = node->type_id;
  }
}
// The pin's pose with the gauge angle added to its roll, as build 606 does
// (0x0043C0A0): with part_pose_matrix's yaw-pitch-roll order this turns it
// about its own posed Z axis.
PartMatrix cockpit_needle_matrix(const CockpitNeedle &needle, double value) {
  PartPose pose = needle.pose;
  pose.rotation[2] += cockpit_needle_angle(needle, value);
  return part_pose_matrix(pose);
}
void render_part(Jvm &jvm, Object *self, Record &rec) {
  rec.inline_wheels.clear();
  collect_lamps(rec);
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
  auto create = [&](uint32_t resource, const PartMatrix &local,
                    const std::vector<uint32_t> *colors,
                    uint32_t light_flags) -> Object * {
    if (!resource ||
        !index
             ->model_for_resource(RpkIndex::pack_from_full(resource),
                                  resource & 0xffff)
             .ok)
      return nullptr;
    Object *render = jvm.new_object("java.util.resource.RenderRef");
    auto *node = gameref_upsert(render, GameRefKind::RenderRef);
    gameref_set_type(*node, static_cast<int32_t>(resource));
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
    return render;
  };
  auto add = [&](uint32_t resource, const PartMatrix &local,
                 const std::vector<PartLink> *rules,
                 const std::vector<uint32_t> *colors,
                 uint32_t light_flags = 0) {
    Object *render = create(resource, local, colors, light_flags);
    if (!render)
      return;
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
      const auto count = rec.renders.size();
      add(r.resource, local, &r.links, &r.colors);
      if (rec.renders.size() > count) {
        const auto i = static_cast<size_t>(r.wheel);
        const float radius = i < rec.config->wheel_springs.size()
                                 ? rec.config->wheel_springs[i].radius
                                 : 0;
        const float factor = i < rec.config->wheel_steering.size()
                                 ? rec.config->wheel_steering[i]
                                 : 0;
        rec.inline_wheels.push_back(
            {rec.renders.back(), part_pose_matrix(rec.config->wheels[i]),
             part_pose_matrix(r.pose), radius > 0 ? radius : .33f, factor, i});
      }
    }
  }
  snap_lamp_glass(rec);
  for (const auto &needle : rec.needles)
    gameref_remove(needle.node);
  rec.needles.clear();
  if (rec.config->native_kind == "car")
    for (size_t i = 0; i < rec.config->cockpit.size(); ++i) {
      const auto &cfg = rec.config->cockpit[i];
      if (!cfg.defined)
        continue;
      const auto gauge = static_cast<CockpitGauge>(i);
      if (Object *pin = create(cfg.resource, cockpit_needle_matrix(cfg, 0), nullptr, 0))
        rec.needles.push_back({pin, gauge});
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
    // A resource id, read as an integer: a Painter id (0xFFFFxxxx) is
    // negative and does not survive a trip through float.
    const auto color = descriptor->fields.find("color");
    const uint32_t texture =
        color == descriptor->fields.end() ? 0u
        : color->second.tag == ValueTag::Int
            ? static_cast<uint32_t>(color->second.v.i)
            : static_cast<uint32_t>(field("color", 0));
    assign_texture(child, *rec, texture);
    stock_parts(jvm, child, descriptor);
  }
  return child;
}
void release_side(Record &rec) {
  host_sfx_stop(rec.nitro_voice);
  rec.nitro_voice = 0;
  for (auto &voice : rec.traffic_voice) {
    host_sfx_stop(voice);
    voice = 0;
  }
  host_sfx_stop(rec.radio_voice);
  rec.radio_voice = 0;
  host_sfx_stop(rec.slide_voice);
  rec.slide_voice = 0;
  skid_marks_forget(reinterpret_cast<uintptr_t>(rec.state.object));
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
// Stock Swaybar fields belong to the currently installed part, not an
// accumulating add/remove command. Mount IDs choose the axle; CFG compatibility
// remains authoritative, including the absence of a mount on some chassis.
void configure_anti_roll(Record &rec) {
  rec.dynamics.anti_roll = {};
  if (!runtime || rec.config->native_kind != "car") return;
  for (size_t axle = 0; axle < rec.dynamics.anti_roll.size(); ++axle) {
    const int slot = 701 + static_cast<int>(axle);
    const auto found = rec.children.find(slot);
    const auto *bar = found == rec.children.end() ? nullptr : record(found->second);
    if (!bar || bar->state.parent != rec.state.object || bar->state.parent_slot != slot ||
        !runtime->tree_instance_of(bar->state.object, "java.game.parts.rgearpart.Swaybar"))
      continue;
    const double stiffness = comp(bar->state.object, "force");
    const double damping = comp(bar->state.object, "damping");
    const double wear = bar->state.wear, tear = bar->state.tear;
    if (!std::isfinite(stiffness) || stiffness < 0 || !std::isfinite(damping) || damping < 0 ||
        !std::isfinite(wear) || !std::isfinite(tear))
      continue;
    const double health = std::clamp(wear, 0., 1.) * std::clamp(tear, 0., 1.);
    rec.dynamics.anti_roll[axle] = {stiffness * health, damping * health};
  }
}
double nitro_capacity(Object *chassis) {
  const float capacity = comp(chassis, "tank_nitro");
  return std::isfinite(capacity) ? std::max(0.f, capacity) : 0.;
}
void clamp_nitro(Object *chassis, Record &rec) {
  rec.nitro_remaining = std::clamp(rec.nitro_remaining, 0., nitro_capacity(chassis));
  const float rate = comp(chassis, "consumption_nitro");
  if (!(rec.nitro_remaining > 0 && std::isfinite(rate) && rate > 0))
    rec.nitro_active = rec.nitro_flame = false;
}
void stop_nitro(Record &rec) {
  rec.nitro_requested = rec.nitro_active = rec.nitro_flame = false;
  rec.nitro_input = 0;
  rec.dynamics.drive.nitro = 0;
  rec.nitro_sound_down = false;
  host_sfx_stop(rec.nitro_voice);
  rec.nitro_voice = 0;
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
  gameref_set_type(*n, static_cast<int32_t>(id));
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
// Local +Y turned onto the two linked anchors (shortest rotation), origin on
// the first. Shared by the bone rig and the stretched fallback.
PartMatrix anchor_axis(const PartMatrix &a, const PartMatrix &b) {
  using physics::Vec3;
  const Vec3 axis{b[12] - a[12], b[13] - a[13], b[14] - a[14]};
  const Vec3 direction = axis / axis.length();
  const auto turn = direction.y < -1 + 1e-8
      ? physics::Quaternion{1, 0, 0, 0}
      : physics::Quaternion{direction.z, 0, -direction.x, 1 + direction.y};
  auto frame = physics::corner_matrix({}, turn);
  frame[12] = a[12];
  frame[13] = a[13];
  frame[14] = a[14];
  return frame;
}
// Fallback for a two-anchor render without a bone rig (a CV shaft): the
// authored +Y bounds are scaled to span the anchors.
PartMatrix span_between(const PartMatrix &a, const PartMatrix &b, float min_y,
                        float max_y, float cx, float cz) {
  const float dx = b[12] - a[12], dy = b[13] - a[13], dz = b[14] - a[14];
  PartMatrix scale = eye_matrix(), center = eye_matrix();
  scale[5] = std::sqrt(dx * dx + dy * dy + dz * dz) / (max_y - min_y);
  center[12] = -cx;
  center[13] = -min_y;
  center[14] = -cz;
  return part_matrix_multiply(anchor_axis(a, b),
                              part_matrix_multiply(scale, center));
}
// The original add_linked binds bone00 of a linked render to its first
// anchor and bone01 to its second. Only a skinned mesh with both bones can
// follow them; the lookup is redone when the render changes type.
bool link_rig(Record::LinkVisual &vis, const GameRefState &node) {
  if (vis.rig_type == node.type_id)
    return vis.bone_from >= 0;
  vis.rig_type = node.type_id;
  vis.bone_from = vis.bone_to = -1;
  if (!index || node.type_id == 0)
    return false;
  const auto mesh = scene_instances_geometry(static_cast<uint32_t>(node.type_id));
  if (!mesh || mesh->skin_weights.empty())
    return false;
  const int from = character_find_bone(vis.node, node.type_id, *index, "bone00");
  const int to = character_find_bone(vis.node, node.type_id, *index, "bone01");
  if (from < 0 || to < 0 || from == to)
    return false;
  vis.bone_from = from;
  vis.bone_to = to;
  return true;
}
void relink_one(Object *part) {
  auto *rec = record(part);
  if (!rec || rec->links.empty())
    return;
  const bool mounted = car(part) != nullptr;
  for (auto &vis : rec->links) {
    auto *node = gameref_get(vis.node);
    if (!node)
      continue;
    // Always start from the authored pose and the bind skeleton; an
    // inventory part has no chassis anchors and must not keep its last
    // compressed/tilted matrix or bone pose.
    std::copy(vis.rest_pose.begin(), vis.rest_pose.end(), node->local_matrix);
    node->matrix_override = true;
    const bool rigged = link_rig(vis, *node);
    bool posed_bones = false;
    const PartLink *rule = nullptr;
    if (mounted)
      for (const auto &candidate : vis.rules)
        if (!candidate.filter || !candidate.parent_slot ||
            candidate.parent_slot == rec->state.parent_slot) {
          rule = &candidate;
          break;
        }
    PartMatrix first{}, second{};
    if (rule && linked_frame(part, rule->first, first, rule->second != 0)) {
      PartMatrix pose = first;
      bool placed = true;
      if (rule->second) {
        float min_y = 0, max_y = 0, cx = 0, cz = 0;
        const bool found = linked_frame(part, rule->second, second);
        const float dx = second[12] - first[12], dy = second[13] - first[13],
                    dz = second[14] - first[14];
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!found || length < 1e-6f) {
          placed = false;
        } else if (rigged) {
          // The rig's frame sits on the first anchor with +Y toward the
          // second; the skin weights deform the mesh between the bones.
          pose = anchor_axis(first, second);
          const float origin[3] = {0, 0, 0}, end[3] = {0, length, 0};
          posed_bones =
              character_place_bone(vis.node, node->type_id, *index, vis.bone_from, origin) &&
              character_place_bone(vis.node, node->type_id, *index, vis.bone_to, end);
        } else if (mesh_span(vis.resource, min_y, max_y, cx, cz)) {
          pose = span_between(first, second, min_y, max_y, cx, cz);
        } else {
          placed = false;
        }
      }
      if (placed)
        std::copy(pose.begin(), pose.end(), node->local_matrix);
    }
    // One anchor moves the whole render (bone00 is its root); a rig left
    // unposed keeps its bind shape.
    if (rigged && !posed_bones)
      character_reset_pose(vis.node);
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
double chassis_steering_angle(const Record& rec) {
  const double max_steer = std::max(0.f, rec.config->max_steer);
  return std::clamp(rec.steering_input * max_steer + rec.steering_correction,
                    -max_steer, max_steer);
}

// The steering constants of a chassis, copied so the physics thread can
// steer between host ticks without reading the record (#203).
struct SteeringGeometry {
  float max_steer = 0, ackermann = 0, ackermann_length = 0, help = 0;
  float speed_sensitivity = .5f;  // analog lock narrowing with speed, 0-1
  std::array<double, 4> factor{}, center_x{};
};
SteeringGeometry steering_geometry(const Record& rec) {
  SteeringGeometry out;
  out.max_steer = rec.config->max_steer;
  out.ackermann = rec.ackermann;
  out.ackermann_length = rec.config->ackermann_length;
  out.help = rec.steering_help;
  out.speed_sensitivity = std::clamp(host_input_tuning().steer_speed_sensitivity, 0, 100) / 100.f;
  for (size_t corner = 0; corner < 4; ++corner) {
    const auto& wheel = rec.wheels[corner];
    out.factor[corner] = wheel.steer_set ? wheel.steer
        : corner < rec.config->wheel_steering.size() ? rec.config->wheel_steering[corner] : 0;
    out.center_x[corner] = rec.dynamics.wheels[corner].center.x;
  }
  return out;
}
double steered_wheel_angle(const SteeringGeometry& geometry, float input, float correction,
                           size_t corner) {
  const double max_steer = std::max(0.f, geometry.max_steer);
  const double angle = std::clamp(input * max_steer + correction, -max_steer, max_steer);
  const double factor = geometry.factor[corner];
  const double length = geometry.ackermann != 0 ? geometry.ackermann : geometry.ackermann_length;
  if (std::abs(angle) < 1e-7 || length <= 0) return angle * factor;
  const double radius = length / std::tan(angle);
  const double x = geometry.center_x[corner];
  return (radius <= 0 ? -std::atan2(length, -radius - x)
                      : std::atan2(length, radius + x)) * factor;
}
double wheel_steering_angle(const Record& rec, size_t corner) {
  return steered_wheel_angle(steering_geometry(rec), rec.steering_input,
                             rec.steering_correction, corner);
}

// The hinge of a parent slot's CFG `flap` line, in the authored parent frame.
bool flap_hinge_authored(const PartSlot &mount, physics::FlapGeometry &out) {
  return mount.has_flap && !corner_gear(mount.type) &&
         physics::flap_geometry_from_cfg(mount.flap_pivot, mount.flap_axis,
                                         mount.flap_dir, mount.flap_lo,
                                         mount.flap_hi, out);
}
// Moves a hinge from the authored slot frame to the slot's current setSlotPos
// or damage pose, like the mount itself.
void flap_follow_slot(const Record &parent, const PartSlot &mount,
                      physics::FlapGeometry &hinge) {
  if (parent.slot_pose.find(mount.id) == parent.slot_pose.end()) return;
  PartMatrix authored, moved;
  if (!part_slot_matrix(*parent.config, mount, authored) ||
      !part_slot_matrix(*parent.config, posed_slot(parent, mount), moved))
    return;
  const auto delta = part_matrix_multiply(moved, part_matrix_inverse_rigid(authored));
  physics::flap_transform(hinge, delta.data());
}
bool flap_hinge(const Record &parent, const PartSlot &mount,
                physics::FlapGeometry &out) {
  if (!flap_hinge_authored(mount, out)) return false;
  flap_follow_slot(parent, mount, out);
  return true;
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
  // A part on a `flap` slot turns about that hinge by its simulated angle
  // (advance_flaps). Closed and latched is the CFG pose.
  physics::FlapGeometry hinge;
  if (c->flap.angle != 0 && c->flap_parent == canonical(parent) &&
      c->flap_slot == slot_id && flap_hinge(*p, *mount, hinge)) {
    PartMatrix turn;
    physics::flap_matrix(hinge, c->flap.angle, turn.data());
    transform = part_matrix_multiply(turn, transform);
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
// One CFG `body` line as a solid in its part's frame. The line's mass is
// spread over its own shape: a box or sphere as written, a phys SCX hull
// when it is a closed surface, else that hull's bounds as a box, else a point.
// The stock game only sums the masses at the body poses (EXTERNAL_RESEARCH
// §4); the inertia this yields is the host model of ENGINE_CONTRACT §11.
physics::MassProperties body_mass_properties(const PartBody &body) {
  physics::MassProperties shape;
  if (body.shape == PartBodyShape::Box) {
    shape = physics::box_mass(body.mass, {},
                              {body.box_half[0], body.box_half[1], body.box_half[2]});
  } else if (body.shape == PartBodyShape::Sphere) {
    shape = physics::sphere_mass(body.mass, {}, body.sphere_radius);
  } else if (body.shape == PartBodyShape::Mesh && !body.collision_mesh.empty() && index) {
    const auto mesh = load_scx_geometry(
        (std::filesystem::path(index->game_root()) / body.collision_mesh).string());
    if (mesh.info.ok &&
        !physics::mesh_mass(body.mass, mesh.positions.data(), mesh.positions.size(),
                            mesh.position_scale, mesh.indices.data(), mesh.indices.size(),
                            shape)) {
      const double s = mesh.position_scale;
      const physics::Vec3 lo{mesh.info.bbox_min[0] * s, mesh.info.bbox_min[1] * s,
                             mesh.info.bbox_min[2] * s};
      const physics::Vec3 hi{mesh.info.bbox_max[0] * s, mesh.info.bbox_max[1] * s,
                             mesh.info.bbox_max[2] * s};
      if (lo.finite() && hi.finite())
        shape = physics::box_mass(body.mass, (lo + hi) * .5, (hi - lo) * .5);
    }
  }
  if (shape.empty())
    shape = physics::point_mass(body.mass, {});
  return physics::transformed(shape, part_pose_matrix(body.pose));
}
// A part's own mass distribution in its frame, from its bodies only. Cached
// per configuration; the entry keeps the configuration alive so its address
// cannot be reused by another one.
struct CachedMass {
  std::shared_ptr<const PartConfig> config;
  physics::MassProperties local;
};
std::unordered_map<const PartConfig *, CachedMass> part_masses;
const physics::MassProperties &
part_mass_properties(const std::shared_ptr<const PartConfig> &config) {
  static const physics::MassProperties none;
  if (!config)
    return none;
  const auto found = part_masses.find(config.get());
  if (found != part_masses.end())
    return found->second.local;
  physics::MassProperties sum;
  for (const auto &body : config->bodies)
    if (body.mass > 0.f)
      physics::accumulate(sum, body_mass_properties(body));
  return part_masses.emplace(config.get(), CachedMass{config, sum}).first->second.local;
}
// Mass distribution of `root`'s installed subtree in root's frame. When no
// installed part has a positive body mass, each body-less part with a CFG
// mass counts as a point mass at its origin, as the previous point-mass sum
// did (PartConfig::mass is currently the sum of the body masses, so this
// only matters if a configuration ever carries a mass of its own).
physics::MassProperties subtree_mass_properties(Object *root) {
  physics::MassProperties sum;
  const PartMatrix inverse = part_matrix_inverse_rigid(world_matrix(root));
  const auto tree = part_installed_tree(root);
  for (const auto &part : tree) {
    auto *rec = record(part.object);
    if (!rec)
      continue;
    const auto &local = part_mass_properties(rec->config);
    if (local.empty())
      continue;
    physics::accumulate(sum, physics::transformed(
        local, part_matrix_multiply(inverse, world_matrix(part.object))));
  }
  if (!sum.empty())
    return sum;
  for (const auto &part : tree) {
    auto *rec = record(part.object);
    if (!rec || !rec->config || !rec->config->bodies.empty() || !(rec->config->mass > 0.f))
      continue;
    const PartMatrix frame = part_matrix_multiply(inverse, world_matrix(part.object));
    physics::accumulate(sum, physics::point_mass(rec->config->mass,
                                                 {frame[12], frame[13], frame[14]}));
  }
  return sum;
}
// Mass properties of the assembled car in the chassis frame: total mass,
// centre of mass, and the principal moments about that centre along the
// chassis axes (kg m^2; the products of inertia are dropped). All zero when
// the installed parts carry no mass at all; a moment is zero (unknown) when
// the fallback point masses leave that axis without spread.
void assembly_mass_properties(Object *chassis, float &mass, float com[3],
                              float inertia[3]) {
  const auto sum = subtree_mass_properties(chassis);
  const auto moments = sum.moments();
  mass = static_cast<float>(sum.mass);
  com[0] = static_cast<float>(sum.center.x);
  com[1] = static_cast<float>(sum.center.y);
  com[2] = static_cast<float>(sum.center.z);
  inertia[0] = static_cast<float>(moments.x);
  inertia[1] = static_cast<float>(moments.y);
  inertia[2] = static_cast<float>(moments.z);
  if (!(mass > 0.f)) {
    mass = 0.f;
    com[0] = com[1] = com[2] = 0.f;
    inertia[0] = inertia[1] = inertia[2] = 0.f;
  }
}
void assembly_mass_center(Object *chassis, float &mass, float com[3]) {
  float inertia[3];
  assembly_mass_properties(chassis, mass, com, inertia);
}
// How far the chassis cage currently moves a corner's wheel centre or strut
// top (the CFG spring base plus travel above the centre), in metres.
double corner_hardpoint_shift(const Record &rec, size_t corner) {
  if (!rec.deformation || corner >= rec.config->wheels.size())
    return 0;
  const auto &position = rec.config->wheels[corner].position;
  const physics::Vec3 center{position[0], position[1], position[2]};
  const auto &wheel = rec.dynamics.wheels[corner];
  const physics::Vec3 up =
      wheel.down.length() > .5 ? -wheel.down.normalized() : physics::Vec3{0, 1, 0};
  const physics::Vec3 hardpoint =
      center + up * (wheel.anchor_length() > 0 ? wheel.anchor_length() : .39);
  return std::max(rec.deformation->displacement(center).length(),
                  rec.deformation->displacement(hardpoint).length());
}
// The parts on a corner's wheel slots: the rim and any other wheel of the
// corner (an outer dually wheel). Each subtree breaks off with its tyre.
void corner_wheel_parts(const Record &rec, size_t corner, std::vector<Object *> &out) {
  for (const auto &slot : rec.config->slots)
    if (slot.type == 101 + static_cast<int>(corner))
      if (const auto child = rec.children.find(slot.id); child != rec.children.end())
        out.push_back(child->second);
}
// The chassis's engine: the direct child carrying the DynoData, as
// configure_drive finds it (stock Chassis.updatevariables takes the child that
// is a Block). Everything installed on it goes with it.
Object *chassis_engine(const Record &rec) {
  for (const auto &[slot, child] : rec.children) {
    const auto dyno = child->fields.find("dynodata");
    if (dyno != child->fields.end() && dyno->second.tag == ValueTag::Obj && dyno->second.v.o)
      return child;
  }
  return nullptr;
}
// The chassis cage has settled after an impact, so its node positions are the
// plastic shape rather than the transient crush peak. SLRR_rev 77b0fb6 ends a
// mount at 0.3 m of hardpoint travel (vehicle-3d.ts moveMounts,
// suspension-3d.ts deform); only travel beyond the dent the corner already had
// when its wheel was fitted counts.
void settle_wheel_mounts(Record &rec, std::vector<Object *> &released) {
  for (size_t corner = 0; corner < 4 && corner < rec.config->wheels.size(); ++corner) {
    const double shift =
        std::max(0.0, corner_hardpoint_shift(rec, corner) - rec.wheel_shift_base[corner]);
    auto &condition = rec.wheel_mount[corner];
    condition = physics::wheel_mount_condition(condition, shift, 0, 0);
    if (condition <= 0)
      corner_wheel_parts(rec, corner, released);
  }
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
  if (!rec) return;
  configure_anti_roll(*rec);
  if (field_int(chassis, "suspend_update")) return;
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
  clamp_nitro(chassis, *record(chassis));
  configure_anti_roll(*record(chassis));
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

int32_t part_controlled_vehicle() { return controlled_vehicle; }

namespace {
void detach_panel(Object* part, Record& rec, Object* chassis, Record& vehicle);
} // namespace

float part_lamp_power(float condition) {
  if (!(condition > kLampBrokenCondition))
    return 0.f;
  const float t = std::min(1.f, (condition - kLampBrokenCondition) /
                                    (kLampFullCondition - kLampBrokenCondition));
  return .4f + .6f * t;
}

bool part_tear_off(Object *part) {
  part = canonical(part);
  auto *rec = record(part);
  Object *chassis = rec ? car(part) : nullptr;
  auto *vehicle = chassis && chassis != part ? record(chassis) : nullptr;
  if (!vehicle)
    return false;
  detach_panel(part, *rec, chassis, *vehicle);
  return !rec->state.parent;
}

std::vector<CarLight> part_car_lights(bool lit_only) {
  std::vector<CarLight> out;
  for (auto &[part, rec] : records) {
    if (rec.lamps.empty())
      continue;
    Object *chassis = car(part);
    if (!chassis || chassis == part)
      continue;
    float m[16];
    if (!scene_instances_render_matrix(part, m))
      continue;
    const float power = part_lamp_power(rec.state.wear * rec.state.tear);
    if (lit_only && power <= 0.f)
      continue;
    for (const auto &lamp : rec.lamps) {
      if (lit_only && !part_lamp_lit(part, lamp.flags, false))
        continue;
      CarLight light;
      light.part = part;
      light.car = chassis;
      light.kind = lamp.kind;
      light.resource = lamp.resource;
      const auto &p = lamp.position, &d = lamp.direction;
      for (int i = 0; i < 3; ++i) {
        light.position[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
        light.direction[i] = m[i] * d[0] + m[4 + i] * d[1] + m[8 + i] * d[2];
      }
      light.position[2] = -light.position[2];
      light.direction[2] = -light.direction[2];
      const float length = std::sqrt(light.direction[0] * light.direction[0] +
                                     light.direction[1] * light.direction[1] +
                                     light.direction[2] * light.direction[2]);
      if (length > 1e-6f)
        for (auto &v : light.direction) v /= length;
      light.diffuse = lamp.light.diffuse;
      light.attenuation = lamp.light.attenuation;
      light.range = lamp.light.range;
      light.theta = lamp.light.theta;
      light.phi = lamp.light.phi;
      light.power = power;
      light.light = lamp.light;
      out.push_back(light);
    }
  }
  // Records are a hash map: a stable order keeps the renderer's light budget
  // from shuffling between frames.
  std::sort(out.begin(), out.end(), [](const CarLight &a, const CarLight &b) {
    return std::tie(a.car, a.part, a.kind) < std::tie(b.car, b.part, b.kind);
  });
  return out;
}

bool part_lamp_lit(void *part, uint32_t flags, bool preview) {
  if (!(flags & 0x105u) || (flags & 0x40u) || !runtime)
    return false;
  const Record *chassis = nullptr;
  if (!preview) {
    chassis = record(car(static_cast<Object *>(part)));
    if (!chassis || !chassis->engine_running) return false;
    // A broken lamp (#279): its part worn or torn past the threshold.
    if (const auto *own = record(static_cast<Object *>(part));
        own && part_lamp_power(own->state.wear * own->state.tear) <= 0.f)
      return false;
  }
  // Both authored NOS mesh and fake-light children use the same simulation
  // sample. Never demonstrate nitrous in the showroom or on a loose exhaust.
  if ((flags & 256u) && chassis && chassis->physics_released &&
      !chassis->physics_suspended && chassis->nitro_flame &&
      contact_visible(chassis->state.object))
    return true;
  // Brake lamps (#279): the pedal of the car the part is installed on.
  if ((flags & 1u) && chassis && chassis->dynamics.drive.brake > kBrakeLampMin)
    return true;
  if (!(flags & 4u))
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
  // Headlights from 17:00 to 6:00 (#313). The stock night starts at 21:00,
  // but dusk is already dark on the default sun cycle. The selector starts at
  // midnight and LIGHTS ON/OFF adds twelve hours.
  constexpr float kLampOn = 17 * 3600.f, kLampOff = 6 * 3600.f;
  return sec < kLampOff || sec >= kLampOn;
}

Object *part_create(Jvm &jvm, RpkIndex &resources, Object *parent,
                    uint32_t resource, const std::string &alias,
                    const std::string &params, bool native_only) {
  index = &resources;
  auto cfg = configuration(resource);
  if (!cfg->valid) {
    for (const auto &error : cfg->errors)
      std::fprintf(stderr, "[part] invalid 0x%x %s: %s\n", resource,
                   cfg->source.c_str(), error.c_str());
    return nullptr;
  }
  std::string script =
      native_only ? std::string{} : resources.payload_prop(resource, "script");
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
  rec.native_vehicle = script.empty() && cfg->native_kind == "car";
  if (cfg->native_kind == "car")
    rec.nitro_sfx = RpkIndex::make_full_id(resources.open_lib("sound.rpk"), 0x25);
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
  gameref_set_type(*node, resource);
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
  gameref_set_type(*node, resource);
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
  // A wheel fitted to a corner whose mount failed starts on a sound mount; a
  // corner that is still dented keeps that dent as its new baseline.
  if (selected.mount->type >= 101 && selected.mount->type <= 104) {
    const auto corner = static_cast<size_t>(selected.mount->type - 101);
    p->wheel_mount[corner] = p->wheel_strut[corner] = 1.f;
    p->wheel_shift_base[corner] = corner_hardpoint_shift(*p, corner);
  }
  // An engine fitted again starts on a sound mount.
  if (p->config->native_kind == "car" && chassis_engine(*p) == child) {
    p->engine_mount = 1.f;
    p->engine_lost = false;
  }
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
  assign_texture(canonical(part), *rec, texture);
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
  rec.asr_strength = std::clamp(options.asr, 0.f, 1.f);
  rec.driving_options_revision = host_driving_options_revision();
}

bool part_command(Object *part, const std::string &command) {
  if (Object* body = as_part(part)) part = body;
  else part = canonical(part);
  std::istringstream input(command);
  std::string verb;
  input >> verb;
  if (verb == "sethorn") {
    auto* rec = record(as_part(part));
    int slot = 0;
    if (!rec || !(input >> slot)) return false;
    slot = (slot % 4 + 4) % 4;
    if (slot != rec->horn_slot && slot && index) {
      const auto horn = rec->horns.find(slot);
      if (horn != rec->horns.end() && horn->second.sfx) {
        auto path = index->payload_prop(horn->second.sfx, "sourcefile");
        if (!path.empty()) {
          std::replace(path.begin(), path.end(), '\\', '/');
          host_sfx_play_file((std::filesystem::path(index->game_root()) / path).string(),
                              horn->second.pitch);
        }
      }
    }
    rec->horn_slot = slot;
    return true;
  }
  if (part && part->class_name == "java.io.Controller" &&
      (verb == "controllable" || verb == "leave")) {
    int32_t id = 0;
    if (!(input >> id)) return false;
    if (auto* target = gameref_find_live(id); target && target->kind == GameRefKind::Camera) {
      target->camera_free = verb == "controllable";
      target->cam_pose_init = false;
      target->follow_hold = false;
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
    } else if (controlled_vehicle == id) {
      if (auto *target = gameref_find_live(id))
        if (auto *rec = record(as_part(static_cast<Object *>(target->key)))) stop_nitro(*rec);
      controlled_vehicle = 0;
    }
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
      // A rebind is a fresh chase pose; native "control" keeps the angle.
      cam->follow_hold = false;
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
        state->dmax = 3000.f;
        gameref_camera_set_fog(*state, 0x0091badb, 1500.f, 3000.f);
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
    (verb == "abs" ? rec->abs_strength : verb == "asr" ? rec->asr_strength :
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
  if (verb == "reload" && rec->config->native_kind == "car") {
    // MWM 45A244; Track.lockCar calls this explicitly. forceUpdate, reset,
    // repair and installing a bottle must not silently refill the aggregate.
    rec->nitro_remaining = nitro_capacity(part);
    return true;
  }
  if (verb == "repair") {
    repair_part(part, *rec);
    return true;
  }
  if (verb == "setsteer" && rec->config->native_kind == "car") {
    float value = 0;
    if (!(input >> value) || !std::isfinite(value))
      return false;
    rec->steering_input = std::clamp(value, -1.f, 1.f);
    rec->steering_request = rec->steering_input;
    rec->steering_correction = 0.f;
    if (rec->dynamic_frames) {
      // A parked chassis already has suspension/hub frames. Rebuild them
      // with the new steering before placing parts, not at the next repair.
      pose_wheels(part, *rec);
      return true;
    }
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
    rec->steering_request = rec->steering_correction = 0.f;
    rec->dynamics.body.velocity = {};
    rec->dynamics.body.momentum = {};
    rec->dynamics.reset_drive();
    rec->engine_running = false;
    stop_nitro(*rec);
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
    stop_nitro(*rec);
    return true;
  }
  if (verb == "suspend" && rec->config->native_kind == "car") {
    rec->physics_suspended = true;
    rec->physics_released = false;
    stop_nitro(*rec);
    return true;
  }
  if (verb == "wakeup" && rec->config->native_kind == "car") {
    rec->physics_suspended = false;
    return true;
  }
  if (verb == "flap_toggle") {
    // Mechanic's tuning click. Same hinge as Part.flap(1) and a hit.
    flap_geometry_for(part, *rec);
    physics::flap_toggle(rec->flap);
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
  // A running batch may read the shells of installed parts.
  part_physics_join();
  part = canonical(part);
  auto *rec = record(part);
  if (!rec)
    return false;
  // A promoted map prop that is removed any other way (world change, lost
  // ground shell) reappears as scenery where it last rested.
  if (rec->config->native_kind == "native_dynamic") scene_instances_settle_dynamic(part, nullptr);
  if (auto* chassis = car(part); chassis && chassis != part)
    if (auto* vehicle = record(chassis)) {
      vehicle->physics_dirty = true;
      vehicle->body_support_dirty = true;
    }
  loose_panels.erase(part);
  repair_relink_pending.erase(part);
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
  for (const auto &needle : rec->needles)
    gameref_remove(needle.node);
  release_side(*rec);
  records.erase(part);
  gameref_remove(part);
  return true;
}
bool part_paint_textures(Object *part, uint32_t *current, uint32_t *source) {
  auto *rec = record(part);
  if (!rec)
    return false;
  if (current)
    *current = rec->state.texture;
  if (source)
    *source = rec->config->texture;
  return true;
}
bool part_set_paint_texture(Object *part, uint32_t texture) {
  auto *rec = record(part);
  if (!rec || !rec->config->texture)
    return false;
  assign_texture(canonical(part), *rec, texture);
  return true;
}
bool part_paint_texture_shared(uint32_t texture, Object *part) {
  return texture_in_use(texture, canonical(part));
}
Object *part_showing_texture(uint32_t texture) {
  for (const auto &[part, rec] : records)
    if (texture && rec.state.texture == texture)
      return part;
  return nullptr;
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
bool finite_frame(const PartMatrix& frame) {
  return std::all_of(frame.begin(),frame.end(),[](float value){return std::isfinite(value);});
}
// Validate script-width motion before publishing any node fields. A finite
// double solver value can still overflow the GameRef's float representation.
std::array<float, 6> script_motion(const physics::RigidBody& body, const PartMatrix& parent) {
  const std::array motion{body.velocity, body.omega()};
  std::array<float, 6> out{};
  for (size_t kind = 0; kind < motion.size(); ++kind)
    for (int axis = 0; axis < 3; ++axis) {
      const double value = parent[axis * 4] * motion[kind].x +
          parent[axis * 4 + 1] * motion[kind].y + parent[axis * 4 + 2] * motion[kind].z;
      if (!std::isfinite(value) || std::fabs(value) > std::numeric_limits<float>::max())
        throw std::runtime_error("unrepresentable script rigid motion");
      out[kind * 3 + axis] = static_cast<float>(value);
    }
  return out;
}
// Zero moments (no shaped body) keep the solid box of the bounds.
bool initialize_loose_body(physics::RigidBody& body,double mass,
                           physics::Vec3 lo,physics::Vec3 hi,physics::Vec3 center,
                           const PartMatrix* frame,uint32_t resource,
                           physics::Vec3 moments = {}) {
  try {
    auto candidate = body;
    candidate.configure(mass,lo,hi,center,moments);
    if (frame) candidate.set_frame(*frame);
    body = candidate;
    return true;
  } catch (const std::exception& error) {
    std::fprintf(stderr,"[phys] rejected loose body type=0x%x: %s\n",resource,error.what());
    return false;
  }
}

void invalidate_body_support(Object* part) {
  if (auto* chassis = car(part))
    if (auto* rec = record(chassis)) rec->body_support_dirty = true;
}

void rebuild_body_support(Object* chassis, Record& rec) {
  using physics::Vec3;
  auto& dynamics = rec.dynamics;
  dynamics.body_support_count = 0;
  std::array<Vec3, 8> lowest{};
  std::array<double, 8> height;
  height.fill(std::numeric_limits<double>::infinity());
  const auto to_chassis = part_matrix_inverse_rigid(world_matrix(chassis));
  const auto transform = [](const PartMatrix& m, Vec3 p) {
    return Vec3{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
  };
  for (const auto& item : part_installed_tree(chassis)) {
    auto* part = record(item.object);
    if (!part) continue;
    if (item.object != chassis) {
      auto* branch = part;
      int depth = 0;
      while (branch && branch->state.parent != chassis && ++depth < 64)
        branch = record(branch->state.parent);
      if (!branch || branch->state.parent != chassis) continue;
      const auto mount = std::find_if(rec.config->slots.begin(), rec.config->slots.end(),
          [&](const auto& slot) { return slot.id == branch->state.parent_slot; });
      // Running gear already has its wheel contact. Hinged panels keep their
      // existing no-floor-contact policy; do not freeze a swinging door here.
      if (mount != rec.config->slots.end() && (corner_gear(mount->type) || mount->has_flap))
        continue;
      // Engine hardware (exhaust pipes, tips and mufflers, sumps) hangs below
      // the floor and scrapes; it does not hold the car up. Counted as body
      // supports, a lowered car's exhaust carried up to 1.5 kN at rest and
      // dragged on the road at 0.4 friction: the rear lost its load and the
      // car went sideways under power (Baiern GT III).
      if (runtime && runtime->tree_instance_of(item.object, "java.game.parts.EnginePart"))
        continue;
    }
    const auto part_frame = world_matrix(item.object);
    const auto from_part = part_matrix_multiply(to_chassis, part_frame);
    for (auto* render : part->renders) {
      const auto* node = gameref_get(render);
      if (!node || node->light_flags) continue;
      const auto mesh = scene_instances_geometry(static_cast<uint32_t>(node->type_id));
      if (!mesh) continue;
      // RenderRefs are already local to their owning part. world_matrix
      // only follows native Part records, not a RenderRef's scene parent.
      const auto mesh_to_part = node_local(render);
      for (size_t v = 0; v + 2 < mesh->positions.size(); v += 3) {
        Vec3 p = transform(mesh_to_part, {mesh->positions[v] * mesh->position_scale,
            mesh->positions[v + 1] * mesh->position_scale,
            mesh->positions[v + 2] * mesh->position_scale});
        if (part->deformation) p += part->deformation->displacement(p);
        p = transform(from_part, p);
        if (!p.finite()) continue;
        const double z = (p.z - dynamics.body.minimum.z) /
            std::max(.01, dynamics.body.maximum.z - dynamics.body.minimum.z);
        const size_t sector = (p.x >= dynamics.body.center.x ? 4 : 0) +
            static_cast<size_t>(std::clamp(z * 4, 0.0, 3.0));
        if (p.y < height[sector]) {
          height[sector] = p.y;
          lowest[sector] = p;
        }
      }
    }
  }
  for (size_t i = 0; i < lowest.size(); ++i)
    if (std::isfinite(height[i])) dynamics.body_supports[dynamics.body_support_count++] = lowest[i];
  rec.body_support_dirty = false;
}

// The world's wind (part_set_wind) and the driving-world time that carries
// its gusts. The host advances it as it prepares each physics batch, which
// takes a copy of both.
physics::Wind world_wind;
double wind_clock = 0;

// The car body gets real-life drag and lift; installed parts' CFG `wing`
// lines authored as wings act as airfoils in their current mount pose, as the
// EXE's per-part wing nodes follow their parts (0x44B48C, 0x48E880). Body
// surface lines only describe the shape the body model already covers.
void rebuild_aerodynamics(Object* chassis, Record& rec) {
  auto& aero = rec.dynamics.aero;
  aero.wings.clear();
  aero.body = !rec.native_vehicle;
  if (rec.native_vehicle) return;
  const auto to_chassis = part_matrix_inverse_rigid(world_matrix(chassis));
  for (const auto& item : part_installed_tree(chassis)) {
    const auto* part = record(item.object);
    if (!part || !part->config || part->config->wings.empty()) continue;
    const auto m = part_matrix_multiply(to_chassis, world_matrix(item.object));
    for (const auto& line : part->config->wings) {
      if (!part_wing_is_airfoil(line)) continue;
      const auto& p = line.position;
      const auto& n = line.normal;
      physics::AeroWing wing;
      wing.position = {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
                       m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                       m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
      wing.normal = physics::Vec3{m[0] * n[0] + m[4] * n[1] + m[8] * n[2],
                                  m[1] * n[0] + m[5] * n[1] + m[9] * n[2],
                                  m[2] * n[0] + m[6] * n[1] + m[10] * n[2]}.normalized();
      wing.area = part_wing_area(line);
      if (wing.position.finite() && wing.normal.length() > .5 &&
          std::isfinite(wing.area) && wing.area > 0)
        aero.wings.push_back(wing);
    }
  }
}

bool dynamics_configure(Object *chassis, Record &rec, const PartMatrix &frame,
                        bool preserve_parked_wheels = false) {
  auto unavailable = [&](const char *reason) {
    if (!rec.physics_reported) {
      std::printf("[phys] unavailable type=0x%x: %s\n", rec.state.resource,
                  reason);
      std::fflush(stdout);
      rec.physics_reported = true;
    }
    return false;
  };
  float mass = 0, com[3]{}, inertia[3]{};
  assembly_mass_properties(chassis, mass, com, inertia);
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
  // A changed assembly (a part broke off) keeps the body's velocity and spin,
  // not its angular momentum, as SLRR_rev 77b0fb6 does after a detach
  // (vehicle-3d.ts advance: setMass, then setAngularVelocity(omega)).
  const auto velocity = rec.dynamics.body.velocity;
  const auto omega = rec.dynamics.body.omega();
  // The installed parts' own mass distribution. A few point masses (a car
  // whose parts lack body shapes) can leave an axis nearly free, so no axis
  // drops below 30 % of the solid box of the chassis render bounds, which also
  // remains the fallback for any axis the parts cannot give.
  const physics::Vec3 size = hi - lo;
  const double box[3] = {mass * (size.y * size.y + size.z * size.z) / 12,
                         mass * (size.x * size.x + size.z * size.z) / 12,
                         mass * (size.x * size.x + size.y * size.y) / 12};
  physics::Vec3 moments;
  double* axes[3] = {&moments.x, &moments.y, &moments.z};
  for (int axis = 0; axis < 3; ++axis)
    *axes[axis] = inertia[axis] > 0 ? std::max<double>(inertia[axis], box[axis] * .3) : 0;
  // Configure a candidate: a bad bound/frame cannot poison the existing body.
  auto candidate = rec.dynamics.body;
  try {
    candidate.configure(mass, lo, hi, {com[0], com[1], com[2]}, moments);
    candidate.set_frame(frame);
  } catch (const std::exception& error) {
    rec.physics_ready = false;
    rec.physics_suspended = true;
    rec.physics_pending_placement = false;
    if (!rec.physics_fault_reported) {
      std::fprintf(stderr,"[phys] suspended chassis type=0x%x: %s\n",rec.state.resource,error.what());
      rec.physics_fault_reported = true;
    }
    return false;
  }
  const bool held = rec.physics_ready;
  rec.dynamics.body = candidate;
  if (held) {
    rec.dynamics.body.velocity = velocity;
    rec.dynamics.body.set_omega(omega);
  } else {
    // A passive body's first configuration must keep park_chassis's loaded
    // suspension. It will not step to seed wheel lengths before publication.
    if (!preserve_parked_wheels) rec.dynamics.release_wheels();
    rec.dynamics.reset_clock();
  }
  rec.physics_ready = true;
  rec.physics_dirty = false;
  rec.body_support_dirty = true;
  // A successful explicit placement/reconfiguration retry restores contact
  // eligibility too; the diagnostic latch must not make a recovered car ghost.
  rec.physics_fault_reported = false;
  rec.physics_parent = gameref_get(chassis)->parent;
  if (!rec.physics_reported) {
    std::printf("[phys] dynamic type=0x%x mass=%.2f fixed=%uHz\n",
                rec.state.resource, mass, physics::VehicleDynamics::frequency);
    std::fflush(stdout);
    rec.physics_reported = true;
  }
  return true;
}
// How far the hardware left on a corner reaches below the wheel centre, in
// the CFG wheel frame: the parts on its wheel slots (type 101-104, such as an
// outer dually wheel) and on its hub (201-204: rotor, caliper and what they
// carry). These follow the hub, so the reach does not depend on the
// suspension travel. Zero when nothing is mounted there.
double corner_hardware_reach(Record &rec, size_t corner) {
  const auto to_wheel =
      part_matrix_inverse_rigid(part_pose_matrix(rec.config->wheels[corner]));
  double reach = 0;
  for (const auto &slot : rec.config->slots) {
    if (slot.type != 101 + static_cast<int>(corner) &&
        slot.type != 201 + static_cast<int>(corner))
      continue;
    const auto child = rec.children.find(slot.id);
    auto *hub = child == rec.children.end() ? nullptr : record(child->second);
    if (!hub)
      continue;
    const PartSlot *mate = nullptr;
    for (const auto &own : hub->config->slots)
      if (own.id == hub->state.own_slot)
        mate = &own;
    PartMatrix mount_frame, mate_frame;
    if (!mate || !part_slot_matrix(*rec.config, posed_slot(rec, slot), mount_frame) ||
        !part_slot_matrix(*hub->config, posed_slot(*hub, *mate), mate_frame))
      continue;
    // The same rest transform place_child applies before the hub motion.
    const auto hub_frame = part_matrix_multiply(
        to_wheel, part_matrix_multiply(mount_frame, part_matrix_inverse_rigid(mate_frame)));
    const auto from_hub = part_matrix_inverse_rigid(world_matrix(child->second));
    for (const auto &item : part_installed_tree(child->second)) {
      const auto *part = record(item.object);
      if (!part)
        continue;
      const auto frame = part_matrix_multiply(
          hub_frame, part_matrix_multiply(from_hub, world_matrix(item.object)));
      for (auto *render : part->renders) {
        const auto *node = gameref_get(render);
        if (!node || node->light_flags)
          continue;
        const auto mesh = scene_instances_geometry(static_cast<uint32_t>(node->type_id));
        if (!mesh)
          continue;
        const auto m = part_matrix_multiply(frame, node_local(render));
        const double scale = mesh->position_scale;
        for (size_t v = 0; v + 2 < mesh->positions.size(); v += 3) {
          const double y = (m[1] * mesh->positions[v] + m[5] * mesh->positions[v + 1] +
                            m[9] * mesh->positions[v + 2]) * scale + m[13];
          reach = std::max(reach, -y);
        }
      }
    }
  }
  return reach;
}
void dynamics_wheels(Record &rec) {
  for (size_t i = 0; i < 4; ++i) {
    auto &cfg = rec.dynamics.wheels[i];
    const auto &live = rec.wheels[i];
    cfg.enabled = false;
    cfg.rigid_contact = false;
    cfg.metal_contact = false;
    if (i >= rec.config->wheels.size())
      continue;
    // The scripts size the wheel of the stock rim slot (Chassis.updatevariables
    // uses partOnSlot(101 + i)). Other wheel-type slots, such as an outer
    // dually wheel, carry no radius of their own.
    const int rim_slot = 101 + static_cast<int>(i);
    bool has_rim_slot = false, has_any_rim = false;
    const PartSlot *mount = nullptr;
    for (const auto &slot : rec.config->slots)
      if (slot.type == rim_slot) {
        has_rim_slot = has_rim_slot || slot.id == rim_slot;
        has_any_rim = has_any_rim || rec.children.count(slot.id);
        if (!mount && rec.children.count(slot.id))
          mount = &slot;
      }
    if (has_rim_slot && !rec.children.count(rim_slot))
      mount = nullptr;
    // Without its rim the corner keeps a wheel the size of the hardware still
    // on it, so the car rests on the brake instead of passing through the
    // floor, or on an outer dually wheel.
    const bool inline_wheel = rec.native_vehicle ||
        std::any_of(rec.inline_wheels.begin(), rec.inline_wheels.end(),
                    [&](const auto &w) { return w.corner == i; });
    const double reach = mount ? 0 : corner_hardware_reach(rec, i);
    if (!mount && !inline_wheel && !(reach > 0))
      continue;
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
    cfg.radius = inline_wheel ? fallback.radius : !mount ? reach : live.radius > 0 ? live.radius : fallback.radius;
    cfg.metal_contact = !inline_wheel && !has_any_rim;
    // WheelRef.setWidth: the tyre width in metres (Tyre.java), stored halved.
    cfg.half_width = cfg.metal_contact ? 0. : std::clamp(double(live.width), 0., .25);
    cfg.rest = live.rest;
    cfg.anchor = fallback.base + fallback.travel;
    cfg.minimum = std::max(0.f, live.min_len);
    cfg.maximum = std::max(cfg.minimum, double(live.max_len > 0 ? live.max_len : live.rest));
    cfg.spring = live.force;
    cfg.bump_damping = live.damp_bound;
    cfg.rebound_damping = live.damp_rebound;
    cfg.unsprung = fallback.mass;
    if (inline_wheel) {
      // Match TrafficVehicle's .25m nominal spring, .1m static sag, .65
      // damping.
      cfg.rest = .25;
      cfg.anchor = .25;
      cfg.minimum = .05;
      cfg.maximum = .45;
      double mass = rec.config->mass > 0 ? rec.config->mass : 1400;
      cfg.spring = mass * 9.81 / 4 / .1;
      cfg.bump_damping = cfg.rebound_damping =
          2 * std::sqrt(cfg.spring * mass / 4) * .65;
      cfg.unsprung = 0;
    }
    cfg.arm_length = 0;
    cfg.has_hub = false;
    cfg.hub = {};
    if (live.has_arm && live.opp < 0 && live.arm[0] > 0) {
      cfg.arm_length = live.arm[0];
      cfg.arm_pivot =
          cfg.center + physics::Vec3{live.arm[1], live.arm[2], live.arm[3]};
      cfg.arm_axis = {live.arm[4], live.arm[5], live.arm[6]};
      if (live.has_hub) {
        cfg.has_hub = true;
        for (int k = 0; k < 10; ++k)
          cfg.hub[static_cast<size_t>(k)] = live.hub[k];
      }
    }
    // Suspension.setRollCenter writes the front-view centre into the first
    // three values (x, y) and the side-view centre into the last three (y, z).
    const auto finite_center = [](float a, float b) {
      return std::isfinite(a) && std::isfinite(b) && std::abs(a) < 100.f && std::abs(b) < 100.f;
    };
    cfg.front_instant_center = finite_center(live.ic_offset[0], live.ic_offset[1]);
    cfg.front_center_offset = {live.ic_offset[0], live.ic_offset[1], 0};
    cfg.side_instant_center = finite_center(live.ic_offset[4], live.ic_offset[5]);
    cfg.side_center_offset = {0, live.ic_offset[4], live.ic_offset[5]};
    cfg.enabled = cfg.radius > 0;
    cfg.rigid_contact =
        cfg.enabled &&
        !(cfg.rest > 0 && cfg.maximum > cfg.minimum && cfg.spring > 0);
    if (cfg.rigid_contact) {
      // A mounted wheel without a working spring rests at its bump stop.
      const double stop = live.min_len > 0 ? live.min_len :
          std::max(0.0, double(fallback.base - fallback.travel));
      cfg.minimum = cfg.maximum = cfg.rest = stop;
      cfg.spring = 0;
    }
  }
}
void pose_wheels(Object *chassis, Record &rec);
void finish_dynamics_sync(Object *chassis, Record &rec);
void dynamics_sync(Object *chassis, Record &rec,
                   const PartMatrix &parent_frame, bool visuals = true) {
  auto *node = gameref_get(chassis);
  const auto world = rec.dynamics.body.frame();
  const auto local =
      part_matrix_multiply(part_matrix_inverse_rigid(parent_frame), world);
  if (!finite_frame(local)) throw std::runtime_error("non-finite chassis publication frame");
  const auto motion = script_motion(rec.dynamics.body, parent_frame);
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
  std::copy(motion.begin(), motion.begin() + 3, node->vel);
  std::copy(motion.begin() + 3, motion.end(), node->angular_vel);
  rec.published_frame = local;
  if (visuals) {
    pose_wheels(chassis, rec);
    finish_dynamics_sync(chassis, rec);
  }
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
    // Forward is hub -Z, so positive road speed rolls about hub -X.
    // The left/right mesh orientation is applied after this shared hub motion.
    const double spin = rec.physics_released ? -state.rotation : 0;
    const physics::Quaternion rolling{std::sin(spin * .5), 0, 0, std::cos(spin * .5)};
    rec.rolling_frame[i] = part_matrix_multiply(
        physics::corner_matrix(frames.hub_origin, hub_steered * rolling), inverse);
  }
  for (auto &wheel : rec.inline_wheels) {
    auto local =
        part_matrix_multiply(rec.rolling_frame[wheel.corner],
                             part_matrix_multiply(wheel.base, wheel.mesh));
    if (auto *n = gameref_get(wheel.node)) {
      std::copy(local.begin(), local.end(), n->local_matrix);
      n->matrix_override = true;
    }
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
void part_apply_state(Object *object, const float* velocity, const float* angular) {
  Object* chassis = as_part(object);
  auto* rec = record(chassis);
  auto* node = gameref_get(chassis);
  if (!rec || !node || rec->config->native_kind != "car") return;
  // Preserve the setMatrix tyre-contact placement convention, then release
  // the actual body with the supplied world velocities (SLRR_rev setState).
  park_chassis(chassis, true);
  PartMatrix frame;
  if (!scene_instances_script_matrix(chassis, frame.data()) ||
      !dynamics_configure(chassis, *rec, frame)) return;
  rec->dynamics.body.set_frame(frame);
  rec->dynamics.body.velocity = {velocity[0], velocity[1], velocity[2]};
  rec->dynamics.body.set_omega({angular[0], angular[1], angular[2]});
  rec->dynamics.reset_clock();
  rec->physics_suspended = false;
  rec->physics_pending_placement = false;
  rec->physics_released = true;
  rec->published_frame = node_local(chassis);
  std::copy(velocity, velocity + 3, node->vel);
  std::copy(angular, angular + 3, node->angular_vel);
}
bool part_car_script_pos(void *key, float *x, float *y, float *z) {
  return script_pos(key, x, y, z);
}
namespace {
Object *part_field(Object *part, const char *name) {
  auto value = part->fields.find(name);
  return value != part->fields.end() && value->second.tag == ValueTag::Obj
             ? value->second.v.o : nullptr;
}

// The driver's steering inputs for one host tick.
struct SteeringInput {
  float steer = 0;
  bool analog = false, handbrake = false, controls = false;
};
SteeringInput steering_input(bool controls) {
  const auto &in = host_input();
  // A partly pulled lever counts: the slide correction would fight it.
  return {in.drive_steer, in.drive_steer_analog, in.drive_handbrake > .05f, controls};
}
// One host tick of the steering filter on explicit state (update_steering).
void steering_filter(const SteeringGeometry &geometry, const SteeringInput &in,
                     const physics::RigidBody &body, bool grounded, float &steering_input,
                     float &steering_request, float &steering_correction, double dt) {
  const bool controls = in.controls;
  const float steer = controls ? std::clamp(in.steer, -1.f, 1.f) : 0.f;
  steering_request = steer;
  const float max_steer = std::max(.01f, geometry.max_steer);
  const auto velocity = body.velocity;
  const double speed = std::hypot(velocity.x, velocity.z);
  const float help = in.analog ? 0.f : geometry.help;
  float target = steer;
  const float wheelbase = geometry.ackermann > 0 ? geometry.ackermann : geometry.ackermann_length;
  // An analog wheel or stick gets the same window, blended by the steering
  // speed sensitivity option (0: full lock at any speed).
  const float narrowing = in.analog ? geometry.speed_sensitivity : 1.f;
  if (narrowing > 0 && wheelbase > 0 && speed > 1) {
    // Keyboard steering, with or without the game's steering help (#383): a
    // key is all or nothing, so full lock at speed is a slide. Cap the wheel
    // angle at what a turn at 1.2 g needs plus 8 degrees of tyre slip, about
    // the tyre's peak: at 50 km/h 17 degrees, at 72 km/h 12.5, at 144 km/h
    // 9.1; full lock below about 27 km/h on a 2.65 m wheelbase.
    constexpr double grip = 1.2 * 9.81, slip_margin = .14;
    const double limit = std::atan(wheelbase * grip / (speed * speed)) + slip_margin;
    // The window follows the slide: countersteer may reach the direction the
    // front axle travels in, plus the same margin. Capped around the body
    // alone, it stopped near 9 degrees at 72 km/h, and a rear sliding at 20
    // degrees could not be caught.
    const auto local_velocity = body.orientation.inverse_rotate(velocity);
    const auto local_spin = body.orientation.inverse_rotate(body.omega());
    const physics::Vec3 front_axle{0, 0, -.5 * wheelbase};
    const auto front = local_velocity + local_spin.cross(front_axle);
    // Positive steer points towards local -X; forward is local -Z.
    const double travel =
        -front.z > 1 ? std::clamp(-std::atan2(front.x, -front.z), -double(max_steer),
                                  double(max_steer))
                     : 0.;
    const double high = std::min(double(max_steer), std::max(0., travel) + limit);
    const double low = std::min(double(max_steer), std::max(0., -travel) + limit);
    const double window = std::lerp(double(max_steer), target > 0 ? high : low, double(narrowing));
    target *= static_cast<float>(window / max_steer);
  }
  if (help > 0 && wheelbase > 0 && speed > 0) {
    // For keyboard steering, bound the requested curvature as speed rises.
    // A rate limit alone still asks for full lock after a short button hold.
    constexpr double lateral_acceleration = .85 * 9.81;
    const double limit = std::atan(wheelbase * lateral_acceleration / (speed * speed * help));
    target *= static_cast<float>(std::min(1., limit / max_steer));
  }
  const bool unwinding = target == 0.f || target * steering_input < 0 ||
                         std::abs(target) < std::abs(steering_input);
  const double normalized_speed = speed / 15;
  const float helped_rate = static_cast<float>(
      std::max(.2, 2. / (1 + normalized_speed * normalized_speed)));
  // Keyboard steering turns in more slowly as speed rises and always
  // returns at 2 rad/s; an analog wheel or stick follows the hand (6 rad/s).
  const float rate = in.analog ? 6.f : unwinding ? 2.f : helped_rate;
  const float max_delta = static_cast<float>(rate * dt / max_steer);
  steering_input += std::clamp(target - steering_input, -max_delta, max_delta);

  const auto local_velocity = body.orientation.inverse_rotate(velocity);
  const double forward_speed = -local_velocity.z;
  if (!controls || geometry.help <= 0.f || in.handbrake ||
      !grounded || forward_speed <= 0.) {
    steering_correction = 0.f;
    return;
  }
  // Positive steer points towards local -X. Align the tyres slightly with
  // the direction of travel during a slide, through the normal tyre forces.
  const double slip = -std::atan2(local_velocity.x, forward_speed);
  const double blend = std::clamp((forward_speed - 5.) / 10., 0., 1.);
  const double excess_slip = std::max(0., std::abs(slip) - .07);
  const double limit = std::min(.12, excess_slip * .55);
  const double requested = steering_input * max_steer;
  const double already_correcting = std::max(0., std::copysign(1., slip) * requested);
  const float correction = static_cast<float>(geometry.help * blend *
      std::copysign(std::max(0., limit - already_correcting), slip));
  constexpr float response = 12.f;
  const float alpha = 1.f - std::exp(-response * static_cast<float>(dt));
  steering_correction = std::lerp(steering_correction, correction, alpha);
}
bool steering_grounded(const physics::VehicleDynamics &dynamics) {
  return dynamics.state[0].contact || dynamics.state[1].contact;
}
void update_steering(Record &rec, bool controls) {
  steering_filter(steering_geometry(rec), steering_input(controls), rec.dynamics.body,
                  steering_grounded(rec.dynamics), rec.steering_input, rec.steering_request,
                  rec.steering_correction, kPhysicsTick);
}

// What configure_drive leaves for the later host ticks of a physics batch:
// everything else it sets holds for the whole frame (#203).
struct DriveRefresh {
  enum class Mode : uint8_t { Parked, Steering, Engine, EngineLost } mode = Mode::Parked;
  bool autonomous = false, controls = false;
  float ai_steer = 0;
};
DriveRefresh configure_drive(Object *chassis, Record &rec, bool controlled) {
  DriveRefresh refresh;
  // Chassis.updatevariables leaves C_drag (fully_stripped_drag less the
  // drag_reduction of each body part on its slots) and drag_center for the
  // EXE's quadratic drag (read at 0x448CD2, applied at 0x456A83). It acts
  // on every scripted car, driven or not.
  auto &aero = rec.dynamics.aero;
  aero.drag = 0;
  aero.drag_center = {};
  if (!rec.native_vehicle) {
    const float drag = comp(chassis, "C_drag");
    aero.drag = std::isfinite(drag) ? std::max(0.f, drag) : 0.f;
    if (auto *center = part_field(chassis, "drag_center")) {
      const physics::Vec3 point{comp(center, "x"), comp(center, "y"), comp(center, "z")};
      if (point.finite()) aero.drag_center = point;
    }
  }
  auto &drive = rec.dynamics.drive;
  drive.torque = {};
  drive.nitro_torque = {};
  drive.nitro = 0;
  rec.nitro_requested = false;
  clamp_nitro(chassis, rec);
  drive.native_traffic = false;
  RaceDriveInput ai;
  const auto forward = rec.dynamics.body.orientation.rotate({0,0,-1});
  const auto position = rec.dynamics.body.position;
  const bool autonomous = !controlled && race_drive_input(chassis,
      {float(position.x),float(position.y),float(position.z)},
      float(std::atan2(-forward.x,-forward.z)),
      float(rec.dynamics.body.velocity.dot(forward)),1.f/60.f,ai);
  if (!controlled && !autonomous) {
    if (rec.steering_correction != 0.f) {
      rec.steering_correction = 0.f;
      for (size_t i = 0; i < drive.wheels.size(); ++i)
        drive.wheels[i].steer = wheel_steering_angle(rec, i);
    }
    return refresh;
  }
  refresh.mode = DriveRefresh::Mode::Steering;
  refresh.autonomous = autonomous;
  refresh.ai_steer = ai.steer;
  if (rec.driving_options_revision != host_driving_options_revision())
    apply_driving_options(rec);
  const bool controls = autonomous || !osd_has_modal();
  refresh.controls = controls;
  if (autonomous) {
    rec.steering_input = rec.steering_request = ai.steer;
    rec.steering_correction = 0;
  } else update_steering(rec, controls);
  for (size_t i = 0; i < drive.wheels.size(); ++i)
    drive.wheels[i].steer = wheel_steering_angle(rec, i);
  // A native stock template may carry mounted CFG wheels rather than inline
  // wheel meshes. It has no Java ratios/engine and still uses TrafficVehicle.
  if (rec.native_vehicle) {
    drive.native_traffic = true;
    drive.throttle = autonomous ? (ai.reverse ? -ai.throttle : ai.throttle)
                     : controls ? host_input().drive_throttle
                                : 0;
    drive.brake = autonomous ? ai.brake
                  : controls ? host_input().drive_brake
                             : 1;
    return refresh;
  }
  auto *ratios = part_field(chassis, "ratio");
  Object *dyno = nullptr;
  for (const auto &[slot, child] : rec.children) {
    if ((dyno = part_field(child, "dynodata")))
      break;
  }
  const bool engine = ratios && ratios->slots.size() >= 8 &&
                      field_int(chassis, "gears") >= 1 && dyno && comp(chassis, "maxRPM") > 0;
  // A car whose engine was torn off still rolls on its tyres and brakes;
  // without a drive it would get the parked-tyre friction that stops it.
  if (!engine && !rec.engine_lost) return refresh;
  refresh.mode = engine ? DriveRefresh::Mode::Engine : DriveRefresh::Mode::EngineLost;
  const auto &in = host_input();
  drive.throttle = autonomous ? ai.throttle : controls ? in.drive_throttle : 0.f;
  drive.brake = autonomous ? ai.brake : controls ? in.drive_brake : 0.f;
  // The handbrake is analog: a lever or axis pulls in proportion, up to the
  // 0.3 a key always gave.
  const double handbrake_pull =
      std::isfinite(in.drive_handbrake) ? std::clamp(in.drive_handbrake, 0.f, 1.f) : 0.f;
  drive.handbrake = !autonomous && controls ? kHandbrakeFull * handbrake_pull : 0;
  drive.abs_strength = rec.abs_strength;
  drive.asr_strength = rec.asr_strength;
  drive.differential_lock = std::clamp(comp(chassis, "diff_lock"), 0.f, 1.f);
  drive.clutch = autonomous ? (ai.start ? 1.f : 0.f) : controls ? 1.f - std::clamp(in.drive_clutch, 0.f, 1.f) : 0.f;
  drive.idle = std::max(400.f, comp(chassis, "engine_rpm_idle"));
  drive.limit = std::max(drive.idle + 100.0, double(comp(chassis, "RPM_limit")));
  // Crank + flywheel + clutch, kg m^2. Chassis.engine_inertia sums the parts
  // (0.09 per kg, 5.6-7.6 on stock engines). 0.02 x that gives 0.11-0.15,
  // the range of a road-car engine. The old 0.28 floor made every engine
  // about 2-3x heavier than a real one, and hid flywheel changes. The 0.10
  // floor is the lowest inertia the stability fixtures cover.
  drive.inertia = std::max(.10, .02 * std::max(0.f, comp(chassis, "engine_inertia")));
  if (engine) {
    // Immutable curve copies: the physics thread samples them while scripts
    // may recalculate or release the dyno session.
    drive.torque = dyno_torque_function(dyno, 0);
    drive.nitro_torque = dyno_torque_function(dyno, 1);
    rec.nitro_input = std::isfinite(in.drive_nitro) ? std::clamp(in.drive_nitro, 0.f, 1.f) : 0.f;
    rec.nitro_requested = controlled && controls && rec.nitro_input > .1f && rec.physics_released;
  } else {
    drive.torque = [](double) { return 0.0; };
    rec.dynamics.engine_running = false;
    rec.dynamics.engine_omega = 0;
  }
  drive.engaged = rec.physics_released;
  drive.reverse = autonomous ? ai.reverse : controls && in.drive_reverse;
  drive.shift = autonomous || !controls ? 0 : in.drive_shift > .5f ? 1 : in.drive_shift < -.5f ? -1 : 0;
  // The benchmark drive shifts automatically whatever the player's saved
  // transmission, so every run covers the same street (#358).
  const bool bench = controls && in.bench_throttle >= 0.f;
  drive.automatic_shift = autonomous || bench || rec.transmission == 1;
  drive.automatic_first = autonomous || rec.transmission == 5;
  drive.auto_clutch = autonomous || bench || rec.transmission != 2;
  drive.starter = engine ? std::max(0.f, comp(chassis, "starter_torque")) : 0.f;
  const double brake_inertia = std::max(.01f, rec.config->brake_inertia);
  drive.bearing_scale = .1 / brake_inertia;
  // The patched DiscBrake (#446) sends each wheel its real torque at full
  // pedal and its hardware (setBrakeSpec): the pedal is then the line
  // pressure and no gain applies. The handbrake (the rear discs' torque)
  // keeps a gain of 2: it is meant to lock the rears at once.
  const bool physical_brakes = std::any_of(rec.wheels.begin(), rec.wheels.end(),
                                           [](const WheelPhys &w) { return w.brake_diameter > 0; });
  // The stock scripts supply torques about twice too high, divided by the
  // engine CFG's `brake_inertia`. kBrakeGain (#400) sets where the pedal
  // locks the wheels. With 50 the stock Baiern CoupeSport (brake_inertia 32)
  // locked its rears at 40 % pedal and all four at 50 %, so the upper half of
  // the travel did nothing. With 25 and the 70 % front split
  // (brake_balance.hpp) the deceleration follows the pedal and the wheels
  // lock near full pedal. The handbrake keeps 50.
  constexpr double kBrakeGain = 25, kHandbrakeGain = 50, kPhysicalHandbrakeGain = 2;
  drive.brake_scale = (physical_brakes ? 1 : kBrakeGain / brake_inertia) *
                      std::max(0.f, rec.config->brake_power) / 1000;
  drive.handbrake_scale = (physical_brakes ? kPhysicalHandbrakeGain : kHandbrakeGain / brake_inertia) *
                          std::max(0.f, rec.config->handbrake_power) / 2000;
  // Real rear hardware needs the rear proportioning valve every car without
  // electronic distribution has, or the rears lock first: full rear pressure
  // up to 30 bar (30 % pedal), then 0.3 of the rise.
  drive.rear_valve_knee = physical_brakes ? .3 : 0;
  drive.rear_valve_slope = physical_brakes ? .3 : 1;
  // Without an engine there is no gearbox either: neutral, wheels roll free.
  drive.gears = engine ? std::clamp(field_int(chassis, "gears"), 1, 6) : 0;
  const double final_drive = comp(chassis, "rearend_ratio");
  drive.ratios = {};
  for (int i = 1; engine && i < 8; ++i)
    drive.ratios[i] = scalar(ratios->slots, i, 0) * final_drive;
  const bool horn = !autonomous && controls && in.drive_horn;
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
    out.brake = std::max(0.f, wheel.brake);
    out.handbrake = std::max(0.f, wheel.hbrake);
    out.bearing = std::max(0.f, wheel.bearing);
    out.hardware = physics::brake_hardware(wheel.brake_pad, wheel.brake_rotor, wheel.brake_diameter);
    out.rolling_resistance = std::max(0.f, wheel.roll_res);
    out.tyre.friction = wheel.friction;
    out.tyre.stiffness = wheel.stiffness;
    out.tyre.sliction = wheel.sliction;
    out.tyre.maximum_load = wheel.max_load;
    out.tyre.load_smoothing = wheel.load_smooth;
    out.tyre.longitudinal_grip = wheel.frictn_x;
    out.tyre.relaxation_length = std::clamp(wheel.relaxation, .05f, 2.f);
    // setCPatch stores the sine of the scripted patch angle.
    out.tyre.patch_angle = std::asin(std::clamp(wheel.cpatch_ang, 0.f, 1.f));
    if (rec.dynamics.wheels[i].metal_contact) {
      // The missing rim's script no longer refreshes its tyre parameters.
      // Do not copy them into a contact on the remaining disc/caliper.
      out.tyre = {};
      out.rolling_resistance = .04;
    }
  }
  // The chassis brake balance as a front share (brake_balance.hpp). The
  // patched Brake applies the balance itself, on real front/rear hardware.
  if (physical_brakes) return refresh;
  std::array<double, 4> brakes{};
  for (size_t i = 0; i < brakes.size(); ++i) brakes[i] = drive.wheels[i].brake;
  const auto balance = chassis->fields.find("brake_balance");
  distribute_brakes(brakes, 2,
                    balance == chassis->fields.end() ? .5 : comp(chassis, "brake_balance"));
  for (size_t i = 0; i < brakes.size(); ++i) drive.wheels[i].brake = brakes[i];
  return refresh;
}

// A car's sounds in the world (#390): placed at its body with its velocity,
// for the listener pan, Doppler and environment reverb. `attenuate` false
// keeps a gain the caller already scales by distance.
HostSfxPlace car_place(const physics::Vec3 &position, const physics::Vec3 &velocity,
                       float reference, float maximum, bool attenuate = true) {
  HostSfxPlace place;
  place.position[0] = float(position.x); place.position[1] = float(position.y); place.position[2] = float(position.z);
  place.velocity[0] = float(velocity.x); place.velocity[1] = float(velocity.y); place.velocity[2] = float(velocity.z);
  place.reference = reference;
  place.maximum = maximum;
  // SLRR_rev vehicle-audio.ts panners: inverse model, rolloff .25.
  place.rolloff = .25f;
  place.attenuate = attenuate;
  return place;
}
// Engine, exhaust and nitro: the player's car is inside the reference
// distance in the cockpit and chase views, so it stays centred at full gain.
HostSfxPlace engine_place(const Record &rec) {
  return car_place(rec.dynamics.body.position, rec.dynamics.body.velocity, 6.f, 400.f);
}

void update_engine_audio(Record &rec, bool active) {
  // MWM 452103: one trigger per press, not an injection loop. Holding an
  // empty kit through a refill is not a new press.
  const bool nitro_down = active && rec.nitro_requested &&
      comp(rec.state.object, "consumption_nitro") > 0;
  if (nitro_down && !rec.nitro_sound_down && rec.nitro_remaining > 0 && index) {
    auto path = index->payload_prop(rec.nitro_sfx, "sourcefile");
    if (!path.empty()) {
      std::replace(path.begin(), path.end(), '\\', '/');
      host_sfx_stop(rec.nitro_voice);
      rec.nitro_voice = host_sfx_play_at(
          (std::filesystem::path(index->game_root()) / path).string(), engine_place(rec), rec.nitro_pitch);
    }
  }
  rec.nitro_sound_down = nitro_down;
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
          item.voice = host_sfx_play_at(item.path, engine_place(rec), pitch, gain, true);
        else {
          host_sfx_update_loop(item.voice, pitch, gain);
          host_sfx_set_place(item.voice, engine_place(rec));
        }
      } else if (item.voice) {
        host_sfx_stop(item.voice);
        item.voice = 0;
      }
    }
  }
}

// Tyre slide and collision sounds, after SLRR_rev 4ca11c2 vehicle-audio.ts.
// The stock clips are sound.rpk `surface/slide` (SLIP3.WAV), `surface`'s
// sharp-skid chirp skid_ok.wav and `collision` (Collision.wav), layered
// with two objects.rpk metal hits for harder impacts. The Web Audio panner's
// inverse distance model (ref, max, rolloff .25) is the host's placed-voice
// model (#390); distance_gain only gates work for far cars.
struct StockSound {
  std::string path;
  float volume = 1;
};
const StockSound &stock_sound(uint32_t local) {
  static std::unordered_map<uint32_t, StockSound> cache;
  auto [it, fresh] = cache.try_emplace(local);
  if (fresh && index) {
    const uint32_t id = RpkIndex::make_full_id(index->open_lib("sound.rpk"), local);
    auto path = index->payload_prop(id, "sourcefile");
    if (!path.empty()) {
      std::replace(path.begin(), path.end(), '\\', '/');
      it->second.path = (std::filesystem::path(index->game_root()) / path).string();
    }
    const auto volume = index->payload_prop(id, "volume");
    if (!volume.empty()) it->second.volume = std::clamp(std::strtof(volume.c_str(), nullptr), 0.f, 1.f);
  }
  return it->second;
}
std::string game_sound(const char *relative) {
  return index ? (std::filesystem::path(index->game_root()) / relative).string() : std::string{};
}
// `collision/chassis` (0x13) has an empty payload; its parent `collision`
// (0x0b) carries the Collision.wav entry.
constexpr uint32_t kSoundCollision = 0x0b, kSoundSurfaceSlide = 0x06;
// Gain of a source `distance` metres from the camera; 0 beyond `maximum`.
float distance_gain(const physics::Vec3 &position, float reference, float maximum) {
  float eye[3]{}, at[3]{}, up[3] = {0.f, 1.f, 0.f};
  if (!gameref_world_camera(eye, at, nullptr, nullptr, nullptr, up)) return 1.f;
  const float dx = float(position.x) - eye[0], dy = float(position.y) - eye[1],
              dz = float(position.z) - eye[2];
  const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!std::isfinite(distance)) return 0.f;
  if (distance > maximum) return 0.f;
  return reference / (reference + .25f * std::max(0.f, distance - reference));
}
// SLRR_rev audio-settings.ts skidLoudness: quieter squeal at speed.
float skid_loudness(double speed) {
  return 1.f - .55f * static_cast<float>(std::clamp((std::abs(speed) - 8) / 22, 0.0, 1.0));
}

void update_slide_audio(Record &rec, bool active) {
  const auto &body = rec.dynamics.body;
  const double speed = body.velocity.length();
  double slip = 0, sideways = 0;
  for (size_t i = 0; i < rec.dynamics.state.size(); ++i) {
    const auto &s = rec.dynamics.state[i];
    // A hub on the road scrapes metal; it does not squeal.
    if (!s.contact || s.force <= 0 || rec.dynamics.wheels[i].metal_contact) continue;
    slip = std::max(slip, std::hypot(s.slip_speed, s.lateral_speed));
    sideways = std::max(sideways, std::abs(s.lateral_speed));
  }
  const float place = active ? distance_gain(body.position, 2.f, 55.f) : 0.f;
  const float level = skid_loudness(speed);
  // One loop for the car, from its most slipping wheel: four wheel loops per
  // car would take the mixer's loop budget from the engines.
  const auto &slide = stock_sound(kSoundSurfaceSlide);
  const float gain = (place > 0 ? 1.f : 0.f) *
                     static_cast<float>(std::clamp((slip - 2) / 10, 0.0, 1.0)) * .3f * level * slide.volume;
  const HostSfxPlace where = car_place(body.position, body.velocity, 2.f, 55.f);
  // SLRR_rev keeps the clip's pitch; a host addition raises it slightly with
  // the slip so a long slide is not one flat tone.
  const float pitch = static_cast<float>(std::clamp(.92 + (slip - 2) / 60, .92, 1.15));
  if (gain > .001f && !slide.path.empty()) {
    if (!rec.slide_voice || !host_sfx_is_playing(rec.slide_voice))
      rec.slide_voice = host_sfx_play_at(slide.path, where, pitch, gain, true);
    else {
      host_sfx_update_loop(rec.slide_voice, pitch, gain);
      host_sfx_set_place(rec.slide_voice, where);
    }
  } else if (rec.slide_voice) {
    host_sfx_stop(rec.slide_voice);
    rec.slide_voice = 0;
  }
  // The sharp chirp of a car snapping sideways, once per slide.
  const bool sharp = place > 0 && speed > 14 && sideways > 3.4;
  if (sharp && !rec.sharp_skid)
    host_sfx_play_at(game_sound("sound/wav/surface/skid_ok.wav"), where, 1.f, .62f * level);
  rec.sharp_skid = sharp || (rec.sharp_skid && speed > 9 && sideways > 1.6);
}

// Skid marks from the wheel state this frame published (not per solver
// step, so the free-running 1000 Hz clock adds no work). Cars past 120 m of
// the camera skip it.
void update_skid_marks(Record &rec, double dt) {
  const auto &body = rec.dynamics.body;
  if (!(distance_gain(body.position, 1.f, 120.f) > 0)) return;
  const physics::Vec3 forward = body.orientation.rotate({0, 0, -1});
  const double speed = body.velocity.length();
  const auto key = reinterpret_cast<uintptr_t>(rec.state.object);
  for (size_t i = 0; i < rec.dynamics.state.size(); ++i) {
    const auto &s = rec.dynamics.state[i];
    const auto &wheel = rec.dynamics.wheels[i];
    SkidWheelSample sample;
    sample.contact = s.contact && s.force > 0 && wheel.radius > 0 && !wheel.metal_contact;
    sample.force = s.force;
    sample.slip_speed = s.slip_speed;
    const double road = s.spin * wheel.radius - s.slip_speed;
    sample.slip_ratio = s.slip_speed / std::max(.5, std::abs(road));
    sample.lateral_speed = s.lateral_speed;
    sample.point = {s.patch.x, s.patch.y, s.patch.z};
    sample.normal = {s.normal.x, s.normal.y, s.normal.z};
    sample.forward = {forward.x, forward.y, forward.z};
    skid_marks_update(key, i, sample, dt, speed);
  }
}

void play_impact_audio(Record &rec, const physics::VehicleDynamics::Impact &hit) {
  const auto now = std::chrono::steady_clock::now();
  if (now - rec.last_impact_sound < std::chrono::milliseconds(160)) return;
  const float place = distance_gain(hit.point, 3.f, 70.f);
  if (!(place > 0)) return;
  rec.last_impact_sound = now;
  const float speed = static_cast<float>(hit.speed);
  const float strength = std::clamp((speed - 2) / 18, 0.f, 1.f);
  const float pitch = std::clamp(.9f + speed / 90, .9f, 1.12f);
  const HostSfxPlace where = car_place(hit.point, rec.dynamics.body.velocity, 3.f, 70.f);
  const auto &chassis = stock_sound(kSoundCollision);
  if (!chassis.path.empty())
    host_sfx_play_at(chassis.path, where, pitch, (.32f + strength * .48f) * chassis.volume);
  if (speed > 7)
    host_sfx_play_at(game_sound("sound/wav/collision/MetalHit2.WAV"), where, pitch * .93f,
                     .15f + strength * .3f);
  if (speed > 14)
    host_sfx_play_at(game_sound("sound/wav/collision/Metal_plane_hit_04.WAV"), where, pitch * 1.05f,
                     strength * .25f);
}

// Native stock cars (traffic, fake racers) have no Java engine or SfxTables, so
// their loop is a host model: the three diesel samples shipped with the traffic
// pack, cross-faded by an RPM derived from speed through a stepped gearbox,
// attenuated by the distance to the world camera and panned by its side. Pitch
// follows the car's mass plus a fixed per-car offset, so a bus and a hatchback,
// or two identical cars, do not sound the same. A CFG `sfx_loop` is the car's
// radio. Only the nearest few cars hold voices because the host mixer has a
// small loop budget.
void update_traffic_engine_audio() {
  constexpr size_t kNearest = 4;
  constexpr float kRange = 80.f, kRefDistance = 5.f, kVolume = .5f, kRadioRange = 40.f;
  constexpr std::array<float, 3> kSampleRpm = {1000.f, 2200.f, 3300.f};
  constexpr std::array<const char *, 3> kSampleFile = {"1000rpm.wav", "2200rpm.wav", "3300rpm.wav"};
  constexpr std::array<float, 4> kGearTop = {8.f, 14.f, 22.f, 34.f}; // m/s
  static std::array<std::string, 3> paths;
  static bool resolved = false;
  static uint32_t epoch = 0;
  if (!resolved && index) {
    resolved = true;
    const auto dir = std::filesystem::path(index->game_root()) / "cars" / "traffic" / "sounds" /
                     "engines" / "diesel_5.0L";
    std::error_code ec;
    bool all = true;
    for (size_t i = 0; i < paths.size(); ++i) {
      const auto file = dir / kSampleFile[i];
      all = all && std::filesystem::is_regular_file(file, ec);
      paths[i] = file.string();
    }
    if (!all) paths = {};
  }
  float eye[3]{}, at[3]{}, up[3] = {0.f, 1.f, 0.f}, right[3]{};
  bool listener = !paths[0].empty() && gameref_world_camera(eye, at, nullptr, nullptr, nullptr, up);
  if (listener) {
    // The renderer flips Z and uses a left-handed look-at, so script-space
    // right is forward x up (ped_view's up x forward only uses |side|).
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    right[0] = f[1] * up[2] - f[2] * up[1];
    right[1] = f[2] * up[0] - f[0] * up[2];
    right[2] = f[0] * up[1] - f[1] * up[0];
    const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    listener = fl > 1e-4f && rl > 1e-4f;
    if (listener)
      for (float &c : right) c /= rl;
  }
  struct Candidate {
    float distance_squared;
    Record *rec;
  };
  std::array<Candidate, kNearest> chosen;
  size_t count = 0;
  if (listener)
    for (auto &[key, rec] : records) {
      if (!rec.native_vehicle || !rec.engine_running || rec.state.parent) continue;
      const auto &p = rec.dynamics.body.position;
      const float dx = float(p.x) - eye[0], dy = float(p.y) - eye[1], dz = float(p.z) - eye[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (!std::isfinite(d2) || d2 > kRange * kRange) continue;
      if (count < kNearest) {
        chosen[count++] = {d2, &rec};
      } else {
        auto farthest = std::max_element(chosen.begin(), chosen.end(), [](const auto &a, const auto &b) {
          return a.distance_squared < b.distance_squared;
        });
        if (d2 < farthest->distance_squared) *farthest = {d2, &rec};
      }
    }
  ++epoch;
  for (size_t n = 0; n < count; ++n) {
    Record &rec = *chosen[n].rec;
    rec.traffic_audio_epoch = epoch;
    const auto &body = rec.dynamics.body;
    const auto forward = body.orientation.rotate({0, 0, -1});
    const float speed = std::abs(static_cast<float>(body.velocity.dot(forward)));
    size_t gear = 0;
    while (gear + 1 < kGearTop.size() && speed >= kGearTop[gear]) ++gear;
    const float low = gear ? kGearTop[gear - 1] : 0.f;
    const float fraction = std::clamp((speed - low) / (kGearTop[gear] - low), 0.f, 1.f);
    const float throttle = std::clamp(std::abs(static_cast<float>(rec.dynamics.drive.throttle)), 0.f, 1.f);
    const float target = std::clamp(
        std::lerp(gear ? 1900.f : 1000.f, 3100.f, fraction) + throttle * 200.f, 1000.f, 3300.f);
    rec.traffic_rpm = rec.traffic_rpm > 0 ? std::lerp(rec.traffic_rpm, target, .2f) : target;
    const float rpm = rec.traffic_rpm;
    const float distance = std::sqrt(chosen[n].distance_squared);
    const float level = scene_ambience_gain(kVolume * (.7f + .3f * throttle), kRefDistance, kRange,
                                            distance);
    // The traffic gain law stays; the host pans the car and adds Doppler.
    const HostSfxPlace where = car_place(body.position, body.velocity, kRefDistance, kRange, false);
    // Heavier cars run lower; the offset is stable per car, within about +-6 %.
    const float mass = rec.config->mass > 100.f ? rec.config->mass : 1300.f;
    const uint32_t hash = static_cast<uint32_t>(
        (reinterpret_cast<uintptr_t>(&rec) >> 4) * 2654435761u + rec.state.resource * 40503u);
    const float character = std::clamp(std::pow(1300.f / mass, .2f), .75f, 1.25f) *
                            (.94f + .12f * float((hash >> 8) & 255u) / 255.f);
    for (size_t i = 0; i < paths.size(); ++i) {
      float weight = 0;
      if (i == 0 && rpm <= kSampleRpm[0]) weight = 1;
      else if (i + 1 == paths.size() && rpm >= kSampleRpm[i]) weight = 1;
      else if (rpm >= kSampleRpm[i] && i + 1 < paths.size() && rpm < kSampleRpm[i + 1])
        weight = (kSampleRpm[i + 1] - rpm) / (kSampleRpm[i + 1] - kSampleRpm[i]);
      else if (i > 0 && rpm >= kSampleRpm[i - 1] && rpm < kSampleRpm[i])
        weight = (rpm - kSampleRpm[i - 1]) / (kSampleRpm[i] - kSampleRpm[i - 1]);
      const float gain = level * weight;
      auto &voice = rec.traffic_voice[i];
      if (gain > .002f) {
        const float pitch = rpm / kSampleRpm[i] * character;
        if (voice && host_sfx_is_playing(voice)) {
          host_sfx_update_loop(voice, pitch, gain);
          host_sfx_set_place(voice, where);
        } else {
          voice = host_sfx_play_at(paths[i], where, pitch, gain, true);
        }
      } else if (voice) {
        host_sfx_stop(voice);
        voice = 0;
      }
    }
    // The car's own radio, only close by.
    const uint32_t radio = rec.config->sfx_loop;
    const float radio_gain = radio && index
        ? scene_ambience_gain(.35f, 3.f, kRadioRange, distance) : 0.f;
    if (radio_gain > .005f && distance < kRadioRange) {
      if (!rec.radio_voice || !host_sfx_is_playing(rec.radio_voice)) {
        auto file = index->payload_prop(radio, "sourcefile");
        std::replace(file.begin(), file.end(), '\\', '/');
        rec.radio_voice = file.empty() ? 0 : host_sfx_play_at(
            (std::filesystem::path(index->game_root()) / file).string(), where, 1.f, radio_gain, true);
      } else {
        host_sfx_update_loop(rec.radio_voice, 1.f, radio_gain);
        host_sfx_set_place(rec.radio_voice, where);
      }
    } else if (rec.radio_voice) {
      host_sfx_stop(rec.radio_voice);
      rec.radio_voice = 0;
    }
  }
  // Cars that left the nearest set, went out of range or stopped.
  for (auto &[key, rec] : records) {
    if (rec.traffic_audio_epoch == epoch) continue;
    for (auto &voice : rec.traffic_voice)
      if (voice) {
        host_sfx_stop(voice);
        voice = 0;
      }
    if (rec.radio_voice) {
      host_sfx_stop(rec.radio_voice);
      rec.radio_voice = 0;
    }
    rec.traffic_rpm = 0;
  }
}

// The corner probe points of collide_chassis in the chassis frame, before the
// crush cage moves them.
std::array<physics::Vec3, 4> chassis_probe_points(const physics::RigidBody& body, double radius) {
  std::array<physics::Vec3, 4> points;
  size_t i = 0;
  for (double x : {body.minimum.x + radius, body.maximum.x - radius})
    for (double z : {body.minimum.z + radius, body.maximum.z - radius})
      points[i++] = {x, std::lerp(body.minimum.y, body.maximum.y, .35), z};
  return points;
}
using WakeBox = std::array<float, 6>;  // lower and upper world corners
// `probe_shift` is the crush cage's displacement of each probe point, taken
// when the batch was prepared. Props to wake are queued for the host, with a
// reach that covers `ticks` host ticks of travel before it can promote them.
physics::VehicleDynamics::Impact collide_chassis(physics::RigidBody& body,
                                                 const std::array<physics::Vec3, 4>& probe_shift,
                                                 physics::Vec3 before, unsigned ticks,
                                                 std::vector<WakeBox>& wake) {
  using physics::Vec3;
  const Vec3 delta = body.position - before;
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
  const auto probes = chassis_probe_points(body, radius);
  for (size_t probe = 0; probe < probes.size(); ++probe) {
    const Vec3 point = probes[probe] + probe_shift[probe];
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
  // Evaluate primitive contacts independently: a triangle hit elsewhere must
  // not hide an earlier native box/sphere impact or a thin trunk between probes.
  PartMatrix frame = body.frame();
  const Vec3 bounds_center = (body.minimum + body.maximum) * .5;
  const Vec3 world_center = body.position +
                            body.orientation.rotate(bounds_center - body.center);
  frame[12] = static_cast<float>(world_center.x);
  frame[13] = static_cast<float>(world_center.y);
  frame[14] = static_cast<float>(world_center.z);
  const float half[3] = {float((body.maximum.x - body.minimum.x) * .5),
                         float((body.maximum.y - body.minimum.y) * .5),
                         float((body.maximum.z - body.minimum.z) * .5)};
  {
    // Props that are still plain scenery become loose bodies before the car
    // reaches them; the margin covers the next tick of travel.
    const float reach = std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]) +
                        3.f + static_cast<float>(delta.length()) * 2.f * std::max(1u, ticks);
    wake.push_back({float(world_center.x) - reach, float(world_center.y) - reach,
                    float(world_center.z) - reach, float(world_center.x) + reach,
                    float(world_center.y) + reach, float(world_center.z) + reach});
  }
  const int samples = static_cast<int>(
      std::clamp(std::ceil(delta.length() / .2), 1.0, 8.0));
  // The box above the lowest 30 % of the chassis, the band the corner probes
  // also start at, so kerbs, steps and road seams do not stop the car.
  const float wall_half[3] = {half[0], half[1] * .7f, half[2]};
  const float wall_lift = half[1] * .3f;
  const auto probe_at = [&](double fraction, SceneBodyContact& out) {
    PartMatrix probe = frame;
    for (int axis = 0; axis < 3; ++axis)
      probe[12 + axis] -= static_cast<float>(movement[axis] * (1.0 - fraction));
    // Shapes and walls both use the box above the kerb band; a lower obstacle
    // is support for the wheels, not a wall.
    PartMatrix raised = probe;
    for (int axis = 0; axis < 3; ++axis) raised[12 + axis] += probe[4 + axis] * wall_lift;
    SceneBodyContact shape, wall;
    const bool shape_hit = scene_instances_contact_box(raised.data(), wall_half, shape);
    const bool wall_hit = scene_instances_contact_walls(raised.data(), wall_half, wall);
    if (!shape_hit && !wall_hit) return false;
    out = shape_hit && (!wall_hit || shape.penetration >= wall.penetration) ? shape : wall;
    return true;
  };
  SceneBodyContact primitive;
  double primitive_fraction = 1.0;
  bool has_primitive = false;
  double clear_fraction = 0.0;
  for (int sample = 1; sample <= samples; ++sample) {
    const double fraction = static_cast<double>(sample) / samples;
    if (!probe_at(fraction, primitive)) {
      clear_fraction = fraction;
      continue;
    }
    primitive_fraction = fraction;
    has_primitive = true;
    break;
  }
  // Narrow the first overlap down between the last clear sample and the hit,
  // so the car is put back close to where it touched instead of up to one
  // sample spacing deep. A car that starts the tick already overlapping (pushed
  // out of one wall into another) keeps its first-sample response, so it is
  // not rewound to a standstill every tick.
  SceneBodyContact start;
  if (has_primitive && (clear_fraction > 0.0 || !probe_at(0.0, start))) {
    for (int step = 0; step < 5 && primitive_fraction - clear_fraction > 1e-3; ++step) {
      const double middle = (clear_fraction + primitive_fraction) * .5;
      SceneBodyContact earlier;
      if (probe_at(middle, earlier)) {
        primitive_fraction = middle;
        primitive = earlier;
      } else {
        clear_fraction = middle;
      }
    }
  }
  // An impact rewinds the body along this tick's movement to where it first
  // touched, so a fast car cannot pass through. A slow approach (a car held
  // against a wall by its tyres, or resting on it) is pushed straight out of
  // the overlap instead, keeping its movement along the surface, and only a
  // quarter-millimetre off it. Rewinding every tick stalled a car sliding
  // along a wall, and the old 2 mm gap took two ticks to drive back across,
  // so a car pushing against a wall shook at half the tick rate.
  constexpr double resting_speed = 1.0;  // m/s towards the surface
  const auto standoff = [](double speed) { return speed < resting_speed ? .00025 : .002; };
  if (has_primitive &&
      (closest.fraction >= 1.f || primitive_fraction < closest.fraction)) {
    const Vec3 normal{primitive.normal[0], primitive.normal[1], primitive.normal[2]};
    const Vec3 point{primitive.point[0], primitive.point[1], primitive.point[2]};
    const double speed = -body.point_velocity(point).dot(normal);
    SceneBodyContact end;
    if (speed < resting_speed && probe_at(1.0, end)) {
      const Vec3 out{end.normal[0], end.normal[1], end.normal[2]};
      body.position += out * (end.penetration + standoff(speed));
    } else {
      body.position = before + delta * primitive_fraction +
                      normal * (primitive.penetration + standoff(speed));
    }
    body.contact(point, normal, .12, .35);
    return {point, normal, speed};
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
  const Vec3 touch = before + delta * closest.fraction;
  const double speed = -body.point_velocity(touch + arm - normal * radius).dot(normal);
  if (speed < resting_speed) {
    // The part of this tick's movement into the surface after the touch.
    const double depth = (1.0 - closest.fraction) * std::max(0.0, -delta.dot(normal));
    body.position = before + delta + normal * (depth + closest.penetration + standoff(speed));
  } else {
    body.position = touch + normal * standoff(speed);
  }
  const Vec3 point = body.position + arm - normal * radius;
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
      if (!body.collision) continue;
      const auto frame = part_pose_matrix(body.pose);
      ScxGeometry source;
      PartMatrix primitive_frame{};
      primitive_frame[0] = primitive_frame[5] = primitive_frame[10] = primitive_frame[15] = 1.f;
      if (body.shape == PartBodyShape::Box)
        source = collision::make_box_mesh(
            primitive_frame, {body.box_half[0], body.box_half[1], body.box_half[2]});
      else if (body.shape == PartBodyShape::Sphere)
        source = collision::make_sphere_mesh(primitive_frame, body.sphere_radius, 12, 6);
      else if (body.shape == PartBodyShape::Mesh && !body.collision_mesh.empty())
        source = load_scx_geometry(
            (std::filesystem::path(index->game_root()) / body.collision_mesh).string());
      else
        continue;
      if (!source.info.ok || source.indices.empty()) continue;
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
  invalidate_body_support(part);
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
  invalidate_body_support(part);
  // Repair must also clear stale render/collision data if the cage is already gone.
  rec.deformation.reset();
  rec.deformed_collision.reset();
  rec.deformation_upload_ticks = 0;
  // Lamp glass keeps its lens snap (#284): deformation never moves it.
  for (auto* render : rec.renders)
    if (auto* node = gameref_get(render); node && !node->light_flags) node->deformed_geometry.reset();
  for (const auto& [slot, child] : rec.children)
    if (const auto* installed = record(child); installed && installed->config->category == 2)
      place_child(part, slot, false);
}

bool scripted_parts_contain(Object* owner, Object* child) {
  if (!owner) return false;
  const auto attached = owner->fields.find("attachedParts");
  if (attached == owner->fields.end() || attached->second.tag != ValueTag::Obj ||
      !attached->second.v.o)
    return false;
  Object* vector = attached->second.v.o;
  const auto data = vector->fields.find("elementData");
  if (data == vector->fields.end() || data->second.tag != ValueTag::Obj ||
      !data->second.v.o || !data->second.v.o->is_array)
    return false;
  const auto& slots = data->second.v.o->slots;
  const int count = std::clamp(field_int(vector, "elementCount",
                                         static_cast<int>(slots.size())),
                               0, static_cast<int>(slots.size()));
  for (int i = 0; i < count; ++i)
    if (slots[static_cast<size_t>(i)].tag == ValueTag::Obj &&
        slots[static_cast<size_t>(i)].v.o &&
        canonical(slots[static_cast<size_t>(i)].v.o) == canonical(child))
      return true;
  return false;
}

void repair_part(Object* part, Record& rec) {
  // SLRR_rev 77b0fb6 repairDamage: clear deformation and slot/wheel damage.
  // Stock Part.repair already visits the installed tree, so keep this reset local.
  restore_deformation(part, rec);
  for (const auto& entry : rec.slot_damage) {
    const int slot_index = entry.first;
    if (slot_index < 0 || static_cast<size_t>(slot_index) >= rec.config->slots.size())
      continue;
    const int slot = rec.config->slots[static_cast<size_t>(slot_index)].id;
    if (const auto scripted = rec.slot_script_pose.find(slot);
        scripted != rec.slot_script_pose.end())
      rec.slot_pose[slot] = scripted->second;
    else
      rec.slot_pose.erase(slot);
    if (rec.state.parent && slot == rec.state.own_slot)
      place_child(rec.state.parent, rec.state.parent_slot, false);
    else
      place_child(part, slot, false);
  }
  rec.slot_damage.clear();
  for (auto& damage : rec.wheel_damage) damage.clear();
  rec.wheel_mount.fill(1.f);
  rec.wheel_strut.fill(1.f);
  rec.wheel_shift_base.fill(0.0);
  rec.engine_mount = 1.f;
  // Avoid relink_assembly here: stock repair can visit descendants before their
  // parents. Queue each affected part once and flush after the scripted walk.
  if (!rec.links.empty()) repair_relink_pending.insert(part);

  // The chassis's attachedParts is flat and covers normal whole-car repair.
  // A detached non-chassis assembly has no such script list in this host, so
  // fill only those uncovered children instead of traversing them twice.
  Object* chassis = car(part);
  for (const auto& [slot, child] : rec.children) {
    const bool scripted = scripted_parts_contain(part, child) ||
        (chassis && chassis != part && scripted_parts_contain(chassis, child));
    if (!scripted && runtime && record(child))
      runtime->invoke_by_name(child->class_name, "repair",
                              {Value::make_obj(child)}, false);
  }
}

struct PanelContact {
  physics::Vec3 minimum, maximum, point;
  double distance = 1e9;
};

// The crush zone of a part is its visible shell: the cage bounds and the
// impact distance come from the render vertices, as SLRR_rev 77b0fb6 builds
// its cages over render samples (vehicle-3d.ts register, viewer.ts
// localPositions). The phys SCX bodies carry the mass and the collision, not
// the crush zone; only a part with no render geometry falls back to them.
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
  bool shell = false;
  for (auto* render : rec.renders) {
    const auto* node = gameref_get(render);
    if (!node || node->light_flags || node->empty) continue;
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
      shell = true;
      const Vec3 difference = p - impact;
      const double distance = difference.dot(difference);
      if (distance < result.distance) {
        result.distance = distance;
        result.point = p;
      }
    }
  }
  if (shell) {
    result.distance = std::sqrt(result.distance);
    return result;
  }
  const auto& physical = part_collision_mesh(rec);
  if (!physical.indices.empty()) {
    result.distance = scene_mesh_nearest(physical, impact, result.point);
    const auto& box = physical.info;
    grow({box.bbox_min[0], box.bbox_min[1], box.bbox_min[2]});
    grow({box.bbox_max[0], box.bbox_max[1], box.bbox_max[2]});
  }
  return result;
}

// SLRR_rev 77b0fb6 wheel release (physics::wheel_mount_condition), impact
// half: the impact energy reaching a corner's hardware adds up against 55 kJ.
// The impulse is the one that stops the closing speed through the effective
// mass at the contact (restitution 0.12, as collide_chassis). A floor contact
// below 8 m/s is routine chassis support (kerbs, bottoming out) and, as in
// SLRR_rev, does not load the mounts. The hardpoint-travel half waits for the
// chassis cage to settle (settle_wheel_mounts): right after impact() the cage
// shows the transient crush peak, including the elastic part.
void damage_wheel_mounts(Record& rec, const physics::RigidBody& body,
                         const physics::VehicleDynamics::Impact& hit,
                         physics::Vec3 local_hit, double internal,
                         std::vector<Object*>& released) {
  const double energy = internal > 0 && (hit.normal.y <= .65 || hit.speed >= 8)
      ? (hit.normal_impulse >= 0
             ? hit.normal_impulse * hit.normal_impulse / (2 * body.mass)
             : physics::contact_impulse_energy(body, hit.point, hit.normal, hit.speed)) * internal
      : 0.0;
  if (!(energy > 0)) return;
  for (size_t corner = 0; corner < 4 && corner < rec.config->wheels.size(); ++corner) {
    auto& condition = rec.wheel_strut[corner];
    const auto& center = rec.config->wheels[corner].position;
    condition = physics::wheel_mount_condition(
        condition, 0, energy, std::hypot(local_hit.x - center[0], local_hit.z - center[2]));
    if (condition <= 0) corner_wheel_parts(rec, corner, released);
  }
}

// Hinge of a part mounted on a `flap` slot. Binds the state to the current
// mount (a part moved or taken off starts closed and latched). The plate is
// sized once per mount, in the authored slot frame, from the panel's
// closed-pose bounds; every use then moves it to the slot's current pose and
// takes the mass of the installed subtree, so the simulated hinge stays the
// drawn one (place_child) after a setSlotPos, a damage pose or an install.
// False when the mount has no usable hinge; `out` may be null to bind only.
bool flap_geometry_for(Object *part, Record &rec, physics::FlapGeometry *out) {
  part = canonical(part);
  Object *parent = rec.state.parent ? canonical(rec.state.parent) : nullptr;
  if (parent != rec.flap_parent || rec.state.parent_slot != rec.flap_slot) {
    rec.flap = {};
    rec.flap_parent = parent;
    rec.flap_slot = rec.state.parent_slot;
    rec.flap_sized = rec.flap_struck = rec.flap_carrier_valid = false;
    rec.flap_box = rec.flap_world_valid = false;
  }
  auto *owner = record(parent);
  if (!owner) return false;
  const PartSlot *mount = nullptr, *mate = nullptr;
  for (const auto &slot : owner->config->slots)
    if (slot.id == rec.state.parent_slot) mount = &slot;
  for (const auto &slot : rec.config->slots)
    if (slot.id == rec.state.own_slot) mate = &slot;
  if (!mount) return false;
  if (!rec.flap_sized) {
    physics::FlapGeometry geometry;
    if (!flap_hinge_authored(*mount, geometry)) return false;
    PartMatrix parent_frame, child_frame;
    const auto bounds = panel_contact(rec, {});
    if (mate && bounds.distance < 1e4 &&
        part_slot_matrix(*owner->config, *mount, parent_frame) &&
        part_slot_matrix(*rec.config, posed_slot(rec, *mate), child_frame)) {
      const auto closed = part_matrix_multiply(parent_frame,
                                               part_matrix_inverse_rigid(child_frame));
      double corners[8][3];
      for (int i = 0; i < 8; ++i) {
        const double p[3] = {i & 1 ? bounds.maximum.x : bounds.minimum.x,
                             i & 2 ? bounds.maximum.y : bounds.minimum.y,
                             i & 4 ? bounds.maximum.z : bounds.minimum.z};
        for (int r = 0; r < 3; ++r)
          corners[i][r] = closed[r] * p[0] + closed[4 + r] * p[1] +
                          closed[8 + r] * p[2] + closed[12 + r];
      }
      physics::flap_size_from_points(geometry, corners, 8, 0);
    }
    if (bounds.distance < 1e4) {
      rec.flap_box = true;
      rec.flap_box_min = bounds.minimum;
      rec.flap_box_max = bounds.maximum;
    }
    rec.flap_geometry = geometry;
    rec.flap_sized = true;
  }
  if (!out) return true;
  *out = rec.flap_geometry;
  flap_follow_slot(*owner, *mount, *out);
  double mass = 0;
  for (const auto &item : part_installed_tree(part))
    if (const auto *installed = record(item.object)) mass += installed->config->mass;
  out->mass = std::max(2., mass);
  return true;
}

// A chassis impact reaching a hinged panel: a strong enough hit releases its
// latch and swings it (flap_hit). Called from the impact path with the same
// world-frame impact damage_chassis receives; it is independent of the
// damage options, which scale deformation, not latches.
void flap_impact(Object *chassis, Record &rec, const physics::RigidBody &body,
                 const physics::VehicleDynamics::Impact &hit) {
  using physics::Vec3;
  if (!(hit.speed >= 2)) return;
  const Vec3 local_hit = body.center + body.orientation.inverse_rotate(hit.point - body.position);
  const Vec3 normal = body.orientation.inverse_rotate(hit.normal);
  const auto inverse = part_matrix_inverse_rigid(world_matrix(chassis));
  for (const auto &installed : part_installed_tree(chassis)) {
    auto *flap = record(installed.object);
    if (!flap || flap == &rec || !flap->state.parent) continue;
    physics::FlapGeometry geometry;
    if (!flap_geometry_for(installed.object, *flap, &geometry)) continue;
    // Nearest point of the panel as it hangs now, as damage_chassis measures it.
    const auto m = part_matrix_multiply(inverse, world_matrix(installed.object));
    const Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    const Vec3 offset = local_hit - Vec3{m[12], m[13], m[14]};
    const auto contact = panel_contact(*flap, {offset.dot(x), offset.dot(y), offset.dot(z)});
    const double speed = hit.speed * std::clamp(1 - contact.distance / .65, 0., 1.);
    if (speed < 1) continue;
    // Into the parent frame: the part's own local matrix carries the point,
    // the parent's chassis-relative rotation the normal.
    const auto local = node_local(installed.object);
    const auto carrier = part_matrix_multiply(inverse, world_matrix(flap->state.parent));
    const Vec3 q = contact.point;
    const double point[3] = {
        local[0] * q.x + local[4] * q.y + local[8] * q.z + local[12],
        local[1] * q.x + local[5] * q.y + local[9] * q.z + local[13],
        local[2] * q.x + local[6] * q.y + local[10] * q.z + local[14]};
    const double push[3] = {
        carrier[0] * normal.x + carrier[1] * normal.y + carrier[2] * normal.z,
        carrier[4] * normal.x + carrier[5] * normal.y + carrier[6] * normal.z,
        carrier[8] * normal.x + carrier[9] * normal.y + carrier[10] * normal.z};
    if (physics::flap_hit(flap->flap, geometry, point, push, speed))
      flap->flap_struck = true;
  }
}

// The engine flies out on a big impact near it, as SLRR_rev 77b0fb6 frees the
// block at zero condition (vehicle-3d.ts damageImpact, the rigid-part loop):
// the impact energy weighted by 0.5 exp(-(d / 1.1 m)^2) of the horizontal
// distance to the engine adds up against 120 kJ (physics::kEngineMountTuning).
// SLRR_rev tuning, not stock behaviour. As there, the engine leaves with the
// velocity it had before the contact, so it carries on out of the car. Floor
// contacts below 8 m/s do not count.
void damage_engine_mount(Object* chassis, Record& rec, const physics::RigidBody& body,
                         const physics::VehicleDynamics::Impact& hit,
                         physics::Vec3 local_hit, double internal,
                         std::vector<Object*>& released) {
  Object* engine = chassis_engine(rec);
  if (!engine || rec.engine_release == engine || !(internal > 0) ||
      (hit.normal.y > .65 && hit.speed < 8)) return;
  const double energy = (hit.normal_impulse >= 0
      ? hit.normal_impulse * hit.normal_impulse / (2 * body.mass)
      : physics::contact_impulse_energy(body, hit.point, hit.normal, hit.speed)) * internal;
  const auto frame = part_matrix_multiply(part_matrix_inverse_rigid(world_matrix(chassis)),
                                          world_matrix(engine));
  rec.engine_mount = physics::wheel_mount_condition(
      rec.engine_mount, 0, energy, std::hypot(local_hit.x - frame[12], local_hit.z - frame[14]),
      physics::kEngineMountTuning);
  if (rec.engine_mount > 0) return;
  const physics::Vec3 at = body.position + body.orientation.rotate(
      physics::Vec3{frame[12], frame[13], frame[14]} - body.center);
  rec.engine_release = engine;
  rec.engine_release_velocity = hit.has_pre_motion
      ? hit.pre_velocity + hit.pre_omega.cross(at - body.position)
      : physics::pre_contact_velocity(body, hit.point, hit.normal, hit.speed, at);
  released.push_back(engine);
}

// `body` is the chassis when it was hit; the batch may have moved it since.
void damage_chassis(Object* chassis, Record& rec, const physics::RigidBody& body,
                    const physics::VehicleDynamics::Impact& hit,
                    bool player, std::vector<Object*>& released) {
  using physics::Vec3;
  flap_impact(chassis, rec, body, hit);
  if (hit.speed < 2) return;
  const auto& options = host_game_options();
  const double multiplier = rec.damage_multiplier >= 0 ? rec.damage_multiplier
                            : player ? options.player_damage : 1.f;
  if (multiplier <= 0) return;
  const double deformation = multiplier * options.deformation / 1.6;
  const double internal = multiplier * options.internal_damage;
  const Vec3 local_hit = body.center + body.orientation.inverse_rotate(hit.point - body.position);
  const Vec3 normal = body.orientation.inverse_rotate(hit.normal);
  if (deformation > 0) {
    // The chassis shell's own CFG mass, not the assembly's (SLRR_rev
    // vehicle-3d.ts register: mass(root)).
    if (!rec.deformation)
      rec.deformation = std::make_unique<physics::DeformationCage>(
          body.minimum, body.maximum, rec.config->mass);
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
  damage_wheel_mounts(rec, body, hit, local_hit, internal, released);
  damage_engine_mount(chassis, rec, body, hit, local_hit, internal, released);
}

// Bounds of the visible shells of `root`'s installed subtree, in root's frame.
bool subtree_shell_bounds(Object* root, physics::Vec3& lo, physics::Vec3& hi) {
  using physics::Vec3;
  lo = {1e30, 1e30, 1e30};
  hi = -lo;
  bool any = false;
  const auto inverse = part_matrix_inverse_rigid(world_matrix(root));
  for (const auto& item : part_installed_tree(root)) {
    const auto* part = record(item.object);
    if (!part) continue;
    const auto relative = part_matrix_multiply(inverse, world_matrix(item.object));
    for (auto* render : part->renders) {
      const auto* node = gameref_get(render);
      if (!node || node->light_flags || node->empty) continue;
      const auto mesh = node->deformed_geometry && node->deformed_type == node->type_id
                            ? node->deformed_geometry
                            : scene_instances_geometry(static_cast<uint32_t>(node->type_id));
      if (!mesh) continue;
      const auto m = part_matrix_multiply(relative, node_local(render));
      const double scale = mesh->position_scale;
      for (size_t i = 0; i + 2 < mesh->positions.size(); i += 3) {
        const double px = mesh->positions[i] * scale, py = mesh->positions[i + 1] * scale,
                     pz = mesh->positions[i + 2] * scale;
        const Vec3 p{m[0] * px + m[4] * py + m[8] * pz + m[12],
                     m[1] * px + m[5] * py + m[9] * pz + m[13],
                     m[2] * px + m[6] * py + m[10] * pz + m[14]};
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        any = true;
      }
    }
  }
  return any && lo.finite() && hi.finite();
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
  // The loose body spans the whole subtree that breaks off (a rim carries its
  // tyre) and keeps that subtree's own mass distribution.
  Vec3 lo, hi;
  if (!subtree_shell_bounds(part, lo, hi)) {
    const auto bounds = panel_contact(rec, {});
    if (bounds.distance > 1e4) return;
    lo = bounds.minimum;
    hi = bounds.maximum;
  }
  bool wheel = false;
  if (const auto* parent = record(rec.state.parent))
    for (const auto& slot : parent->config->slots)
      if (slot.id == rec.state.parent_slot)
        wheel = slot.type >= 101 && slot.type <= 104;
  const auto distribution = subtree_mass_properties(part);
  // SLRR_rev 77b0fb6 vehiclePartMass: a part on a wheel slot weighs at least
  // 14 kg (stock rims and tyres carry no body). Other parts keep the 2 kg floor.
  const double mass = std::max(wheel ? 14. : 2., distribution.mass);
  const Vec3 padding{.005, .005, .005};
  Vec3 center = (lo + hi) * .5, moments;
  if (!distribution.empty()) {
    const Vec3 c = distribution.center;
    center = {std::clamp(c.x, lo.x, hi.x), std::clamp(c.y, lo.y, hi.y),
              std::clamp(c.z, lo.z, hi.z)};
    moments = distribution.moments() * (mass / distribution.mass);
    // Point-like bodies (a 0.1 m sphere on a large part) would leave the loose
    // body spinning on every contact; keep the box of the visible bounds then.
    const Vec3 s = hi - lo;
    if (moments.x < .05 * mass * (s.y * s.y + s.z * s.z) / 12 ||
        moments.y < .05 * mass * (s.x * s.x + s.z * s.z) / 12 ||
        moments.z < .05 * mass * (s.x * s.x + s.y * s.y) / 12)
      moments = {};
  }
  void* const world_parent = chassis_node->parent;
  auto& loose = loose_panels[part];
  loose.vehicle = gameref_make_live(chassis);
  auto& body = loose.body;
  if (!initialize_loose_body(body, mass, lo - padding, hi + padding, center, &frame,
                             rec.state.resource, moments)) {
    loose_panels.erase(part);
    return;
  }
  const auto& carrier = vehicle.dynamics.body;
  loose.carrier_frame = carrier.frame();
  body.velocity = carrier.point_velocity(body.position);
  body.set_omega(carrier.omega());
  if (part == vehicle.engine_release) {
    // A failed engine mount: the engine keeps its pre-impact velocity.
    if (vehicle.engine_release_velocity.finite())
      body.velocity = vehicle.engine_release_velocity;
    vehicle.engine_release = nullptr;
    vehicle.engine_lost = true;
    loose.exit_ticks = physics::VehicleDynamics::frequency;
  }
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

// Static scene response of a loose body after integration: one sphere sweep of
// its travel against walls, then floor support under its eight corners.
struct SceneSupport {
  bool penetrating = false;  // a corner was below the floor and was pushed out
  bool touching = false;     // a corner is within 1 cm of the floor afterwards
};
SceneSupport collide_loose_with_scene(physics::RigidBody& body, physics::Vec3 before,
                                      void* self) {
  using physics::Vec3;
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
  double clearance = std::numeric_limits<double>::infinity();
  Vec3 correction;
  for (const auto& point : body.corners()) {
    SceneFloorHit floor;
    if (!scene_instances_sample_floor(float(point.x), float(point.z),
        float(std::max(before.y, body.position.y) + .25), self, floor)) continue;
    const double overlap = floor.height - point.y;
    clearance = std::min(clearance, -overlap);
    if (overlap <= 0) continue;
    const Vec3 normal{floor.normal[0], floor.normal[1], floor.normal[2]};
    body.contact(point, normal, .12, .75);
    if (overlap > depth) {
      depth = overlap;
      correction = normal;
    }
  }
  if (depth > 0) body.position += correction * (depth + .001);
  // The push-out leaves the body 1 mm clear, so it alternates between ticks
  // in and out of contact while it rests.
  return {depth > 0, depth > 0 || clearance < .01};
}

// At the 60 Hz loose-body step gravity alone adds 0.16 m/s per tick, and the
// single-pass corner contacts leave a resting body jittering, rocking and
// creeping at 0.1-0.3 m/s. A car panel tolerates it; a map object or a script
// body would stay awake, and so live, forever. Instead of its instantaneous
// speed, count the ticks it stays on the ground within 5 cm of where it began
// to slow down. Returns the updated quiet tick count.
unsigned map_object_quiet_ticks(const physics::RigidBody& body, bool free, unsigned quiet,
                                physics::Vec3& anchor) {
  if (!free || body.velocity.dot(body.velocity) > .25) return 0;
  if (!quiet) anchor = body.position;
  const auto drift = body.position - anchor;
  return drift.dot(drift) < .0025 ? quiet + 1 : 0;
}

}  // namespace

Object* part_add_dynamic_native(const PartConfig& config,
                                const std::vector<PartRender>& renders,
                                const PartMatrix& world, void* parent, int32_t seed) {
  if (!runtime || !parent || !finite_frame(world) ||
      invalid_dynamic_types.contains(config.resource)) return nullptr;
  // Mass and centre from the collision bodies; nocollision bodies do not move
  // the object.
  double mass = 0.0;
  physics::Vec3 center;
  for (const auto& body : config.bodies) {
    if (!body.collision || !(body.mass > 0.f)) continue;
    mass += body.mass;
    center = center + physics::Vec3{body.pose.position[0], body.pose.position[1],
                                    body.pose.position[2]} * body.mass;
  }
  if (!(mass > 0.0)) return nullptr;
  auto* node = runtime->new_object("java.util.resource.RenderRef");
  if (!node) return nullptr;
  auto& rec = records[node];
  auto dynamic_config = std::make_shared<PartConfig>(config);
  dynamic_config->native_kind = "native_dynamic";
  rec.config = std::move(dynamic_config);
  rec.state.object = node;
  rec.state.resource = config.resource;
  rec.published_frame = world;
  const auto& physical = part_collision_mesh(rec);
  if (physical.indices.empty()) {
    records.erase(node);
    std::fprintf(stderr, "[part] dynamic native has no collision geometry: %s\n",
                 config.name.c_str());
    return nullptr;
  }

  std::vector<PartMatrix> offsets;
  std::vector<PartRender> slots = renders;
  if (slots.empty()) {
    PartRender render;
    render.resource = config.render;
    render.pose = config.render_pose;
    slots.push_back(render);
  }
  for (const auto& render : slots) {
    auto* render_node = &render == &slots.front()
                            ? node
                            : runtime->new_object("java.util.resource.RenderRef");
    if (!render_node) continue;
    auto* state = gameref_upsert(render_node, GameRefKind::RenderRef);
    if (!state) continue;
    rec.renders.push_back(render_node);
    offsets.push_back(part_pose_matrix(render.pose));
    state->dynamic_native = true;
    state->script = render_node;
    state->class_name = render_node->class_name;
    state->alias = config.name.empty() ? "dynamic_native" : config.name;
    const uint32_t resource = render.variants.empty()
                                  ? render.resource
                                  : render.variants[static_cast<size_t>(seed) % render.variants.size()];
    gameref_set_type(*state, static_cast<int32_t>(resource));
    if (!render.colors.empty())
      state->color = render.colors[static_cast<size_t>(seed) % render.colors.size()];
    const auto local = part_matrix_multiply(world, part_pose_matrix(render.pose));
    std::copy(local.begin(), local.end(), state->local_matrix);
    state->matrix_override = true;
    state->empty = false;
    render_node->fields["id"] = Value::make_int(static_cast<int32_t>(resource));
    gameref_set_parent(render_node, parent);
  }

  // A flat collision body (a sign board) has no extent on one axis; the
  // rigid body needs some, as detach_panel also pads.
  const physics::Vec3 padding{.005, .005, .005};
  const physics::Vec3 lo = physics::Vec3{physical.info.bbox_min[0], physical.info.bbox_min[1],
                                         physical.info.bbox_min[2]} - padding;
  const physics::Vec3 hi = physics::Vec3{physical.info.bbox_max[0], physical.info.bbox_max[1],
                                         physical.info.bbox_max[2]} + padding;
  if (!lo.finite() || !hi.finite()) {
    for (auto* render : rec.renders)
      if (render != node) gameref_remove(render);
    records.erase(node);
    gameref_remove(node);
    return nullptr;
  }
  auto& loose = loose_panels[node];
  if (!initialize_loose_body(loose.body,mass,lo,hi,center/mass,nullptr,config.resource)) {
    invalid_dynamic_types.insert(config.resource);
    loose_panels.erase(node);
    part_destroy(node);
    return nullptr;
  }
  loose.render_offsets = std::move(offsets);
  loose.vehicle = controlled_vehicle;
  loose.quiet_ticks = physics::VehicleDynamics::frequency / 2;
  if (auto* chassis = part_controlled_chassis()) {
    if (auto* vehicle = record(chassis))
      loose.carrier_frame = vehicle->dynamics.body.frame();
    else
      loose.carrier_frame = world;
  } else {
    loose.carrier_frame = world;
  }
  return node;
}

namespace {
// A promoted map prop at rest this far beyond car reach becomes scenery again.
constexpr double kSettleDistance = 40.0;
// One that is still moving out there after this long (fell off the map, keeps
// rolling) goes back to where the map placed it, and so does one below this.
unsigned lost_ticks_limit() { return physics::VehicleDynamics::frequency * 10; }
constexpr double kLostHeight = -500.0;

// A body released from deep inside the car starts overlapping its collision
// shell, where the corner sweeps cannot separate it cleanly: the engine would
// shove the car with its whole momentum. It skips the car contacts while any
// of its corners is still inside the car's bounds, for at most one second.
bool leaving_carrier(const physics::RigidBody& body, const physics::RigidBody& car) {
  for (const auto& corner : body.corners()) {
    const auto p = car.center + car.orientation.inverse_rotate(corner - car.position);
    if (p.x > car.minimum.x && p.x < car.maximum.x && p.y > car.minimum.y &&
        p.y < car.maximum.y && p.z > car.minimum.z && p.z < car.maximum.z)
      return true;
  }
  return false;
}

// ---- Physics thread (#203) ------------------------------------------------
// part_physics_begin prepares one batch with the host ticks owed for the
// frame and hands it to the physics thread, which integrates it while the
// host renders; part_physics_end joins it and commits the results. A batch
// owns a copy of every body it integrates and of the inputs they need, taken
// while the host owns everything. On the physics thread it reads only those
// copies and the scene collision prepared for it: never the VM, GameRef,
// records, audio or bgfx. GameRef publication, damage, scene changes and
// loose-body lifetimes wait for the commit, which applies them in solver order.

bool contact_visible(Object* chassis) {
  unsigned depth = 0;
  for (void* key = chassis; key && ++depth <= 128;) {
    const auto* node = gameref_get(key);
    if (!node || node->empty || node->streamed_hidden) return false;
    if (key == scene_instances_world()) return true;
    if (node->flags & (kWorldTreeRoot | kWorldTreeLeaf)) return false;
    key = node->parent;
  }
  return false;
}

// A car shell for loose and free body contacts (collide_loose_with_car).
struct SimShell {
  const ScxGeometry* mesh = nullptr;
  PartMatrix relative{};  // the part in its chassis's script frame
};

struct SimCar {
  Object* chassis = nullptr;
  Record* rec = nullptr;  // for the host only, before and after the batch
  uint64_t incarnation = 0;
  int32_t live_id = 0;
  uint32_t resource = 0;
  bool controlled = false;
  bool configured = false;  // configure_drive ran for the first tick
  PartMatrix parent{}, world{}, local{};  // `local`/`world`: last publication
  physics::VehicleDynamics dynamics;
  // The record fields the solver changes; the commit copies them back.
  bool ready = false, released = false, suspended = false, pending = false;
  bool fault_reported = false, engine_running = false, render_blend = false;
  unsigned quiet_ticks = 0;
  physics::Vec3 render_from_position;
  physics::Quaternion render_from_orientation;
  double nitro_remaining = 0, nitro_capacity = 0, nitro_rate = 0;
  float nitro_input = 0;
  bool nitro_requested = false, nitro_active = false, nitro_flame = false;
  uint32_t nitro_random = 1;
  float steering_input = 0, steering_request = 0, steering_correction = 0;
  physics::Vec3 cockpit_offset;
  float head_steer = 0, head_acceleration = 0, head_velocity = 0;
  DriveRefresh drive;
  SteeringGeometry steering;
  SteeringInput steer;
  std::array<physics::Vec3, 4> probe_shift{};
  std::vector<SimShell> shells;
  std::shared_ptr<SceneFloorPatch> floor_patch;  // the record's
  // Distant traffic (#380): steps once every `stride` solver steps, as one
  // merged step; set by prepare_batch, back to 1 after a car contact.
  unsigned stride = 1;
  // This substep's contact list entry.
  bool listed = false;
  physics::RigidBody previous;
  float mileage = 0;
  // Results.
  unsigned steps = 0;  // solver steps, for the crush cage
  int publish = 0;     // 1: chassis frame, 2: with the running gear (dynamics_sync)
  bool fault = false, fault_was_ready = false;  // stepping failed
  PartMatrix fault_local{};
  bool contact_fault = false;
  std::string message;
};

struct SimImpact {
  size_t car = 0;
  physics::VehicleDynamics::Impact hit;
  physics::RigidBody body;  // the chassis when it was hit
};

struct SimLoose {
  Object* part = nullptr;
  uint32_t resource = 0;
  bool native_dynamic = false;
  PartMatrix parent_frame{};
  physics::RigidBody body;
  int32_t vehicle = 0;
  unsigned quiet_ticks = 0, far_awake_ticks = 0, exit_ticks = 0;
  physics::Vec3 rest_anchor;
  // The car a released engine is leaving: a batch car, or its prepared body.
  int carrier = -1;
  bool carrier_fixed = false;
  physics::RigidBody carrier_body;
  enum class End : uint8_t { None, Settle, Lost, Fault } end = End::None;
  bool published = false;
  PartMatrix local{}, end_frame{};
  std::string message;
};

struct SimFree {
  void* key = nullptr;
  int32_t live_id = 0;
  PhysicsShape shape = PhysicsShape::None;
  PartMatrix parent_frame{};
  physics::RigidBody body;
  unsigned quiet_ticks = 0;
  physics::Vec3 rest_anchor;
  std::array<float, 3> pos{}, ypr{};  // last publication
  std::array<float, 6> motion{};
  bool published = false, disabled = false;
  std::string message;
};

struct ContactSphere {
  physics::Vec3 center;
  double radius = 0;
};

struct SimBatch {
  unsigned ticks = 0;
  std::vector<unsigned> step_count, step_base;  // solver steps per host tick (#380)
  unsigned solver_steps = 0;
  // Cursor of simulate_step. An open batch (free-running thread, #380) has no
  // planned length: each host tick is one solver step, appended as it runs.
  bool open = false;
  unsigned tick = 0, substep = 0, steps_done = 0;
  unsigned horizon_steps = 0;  // open batch: steps expected before the next sync
  std::vector<size_t> vehicles;  // scratch of simulate_step
  std::vector<ContactSphere> spheres;
  std::vector<SimCar> cars;
  std::vector<SimLoose> loose;
  std::vector<SimFree> free;
  std::vector<SimImpact> impacts;
  std::vector<WakeBox> wake;
  std::vector<size_t> loose_ended;  // loose bodies that left the batch, in order
  int64_t thread_ticks = 0;         // physics-thread time for the frame profiler
  physics::Wind wind;               // world_wind, and wind_clock at the first tick
  double wind_time = 0;
};

bool sim_live(const SimCar& car) { return car.ready && !car.fault_reported; }

// Multi-rate traffic (#380). In a free-running batch, native traffic farther
// than kCoarseDistance from the controlled car steps at about 125 Hz, and
// beyond kFarDistance at about 60 Hz: one merged VehicleDynamics step every
// `stride` solver steps, its phase spread over the cars. A car contact puts
// both cars back on every step. The Physics Lab gives the same handling at
// 125 Hz and stable, less accurate driving at 60 Hz.
constexpr double kCoarseDistance = 60, kFarDistance = 150;
unsigned traffic_stride(double distance) {
  const unsigned hz = distance > kFarDistance ? 60 : distance > kCoarseDistance ? 125 : 0;
  return hz ? std::max(1u, physics::VehicleDynamics::frequency / hz) : 1;
}
uint64_t solver_step_clock = 0;  // solver steps run, for the coarse phase

// Bounding sphere of a contact car over this step's sweep, for the pair
// broadphase: the box centre, and the box half-diagonal plus the step's travel.
ContactSphere contact_sphere(const SimCar& car) {
  const auto& body = car.dynamics.body;
  const physics::Vec3 local_center = body.minimum * .5 + body.maximum * .5;
  return {body.position + body.orientation.rotate(local_center - body.center),
          (body.maximum * .5 - body.minimum * .5).length() +
              (body.position - car.previous.position).length()};
}

std::string phys_message(const char* what, uint32_t resource, const char* reason) {
  char line[512];
  std::snprintf(line, sizeof(line), "[phys] %s type=0x%x: %s\n", what, resource, reason);
  return line;
}

void sim_suspend_contact(SimCar& car, const char* reason) {
  if (!car.fault_reported)
    car.message += phys_message("suspended contact chassis", car.resource, reason);
  car.fault_reported = true;
  car.suspended = true;
  car.pending = car.ready = false;
  car.engine_running = car.render_blend = false;
  car.dynamics.body.velocity = car.dynamics.body.momentum = {};
  car.dynamics.reset_clock();
  car.dynamics.reset_drive();
  car.contact_fault = true;
}

bool sim_near(const physics::RigidBody& body, const SimCar& vehicle, double padding = 0) {
  const auto& car = vehicle.dynamics.body;
  const auto gap = body.position - car.position;
  const double reach = ((car.maximum - car.minimum).length() +
                         (body.maximum - body.minimum).length()) * .5 + 1 + padding +
      (car.velocity.length() + body.velocity.length()) * physics::VehicleDynamics::timestep * 2;
  return gap.dot(gap) < reach * reach;
}

// What dynamics_sync publishes, checked as it checks it.
void sim_publish(SimCar& car, bool running_gear) {
  const auto world = car.dynamics.body.frame();
  const auto local = part_matrix_multiply(part_matrix_inverse_rigid(car.parent), world);
  if (!finite_frame(local)) throw std::runtime_error("non-finite chassis publication frame");
  script_motion(car.dynamics.body, car.parent);
  car.local = local;
  car.world = world;
  car.publish = std::max(car.publish, running_gear ? 2 : 1);
}

physics::VehicleDynamics::Impact body_pair_impact(
    const physics::RigidBody& before, const physics::RigidBody& after,
    physics::Vec3 point, physics::Vec3 normal, double impulse, double closing) {
  // Translate a finite-mass impulse into this body's equivalent wall severity
  // for legacy deformation/wear, while mounts consume the actual impulse.
  const auto arm = point - after.position;
  const double inverse_mass = 1 / before.mass +
      normal.dot(before.inverse_world_inertia(arm.cross(normal)).cross(arm));
  physics::VehicleDynamics::Impact hit{point, normal,
      impulse * inverse_mass / (closing > .8 ? 1.08 : 1)};
  hit.normal_impulse = impulse;
  hit.pre_velocity = before.velocity;
  hit.pre_omega = before.omega();
  hit.has_pre_motion = true;
  return hit;
}

// Damage for the host; impacts below 2 m/s change nothing there.
void sim_impact(SimBatch& batch, size_t car, const physics::VehicleDynamics::Impact& hit) {
  if (!(hit.speed < 2)) batch.impacts.push_back({car, hit, batch.cars[car].dynamics.body});
}

// SLRR_rev triangle-surface.ts. Sweep in the carrier's moving frame so a
// stationary fragment can also be hit by the car.
bool collide_loose_with_car(physics::RigidBody& body,
                            const std::array<physics::Vec3, 8>& previous,
                            physics::Vec3 before, SimCar& vehicle,
                            const PartMatrix& previous_carrier,
                            physics::VehicleDynamics::Impact& car_hit) {
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
  const auto corners = body.corners();
  SceneSphereHit hit;
  Vec3 point, normal;
  for (const auto& shell : vehicle.shells) {
    const auto& mesh = *shell.mesh;
    const auto& relative = shell.relative;
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
  const auto carrier_before = carrier;
  const double closing = std::max(0.0, -(body.point_velocity(point) -
                                        carrier.point_velocity(point)).dot(normal));
  const double impulse = body.contact(point, normal, .08, .55, &carrier);
  car_hit = body_pair_impact(carrier_before, carrier, point, -normal, impulse, closing);
  return true;
}

using SimCarriers = std::vector<std::pair<SimCar*, physics::RigidBody>>;
using SimContacts = std::vector<std::pair<size_t, physics::VehicleDynamics::Impact>>;

void sim_validate_exchange(SimBatch& batch, const physics::RigidBody& body,
                           const SimCarriers& carriers, const SimContacts& impacts) {
  physics::validate_vehicle_contact_body(body);
  for (const auto& carrier : carriers)
    physics::validate_vehicle_contact_body(carrier.first->dynamics.body);
  for (const auto& [index, hit] : impacts) {
    const auto& car = batch.cars[index];
    script_motion(car.dynamics.body, car.parent);
    if (!hit.point.finite() || !hit.normal.finite() || !std::isfinite(hit.speed) ||
        hit.speed < 0 || !std::isfinite(hit.normal_impulse) || hit.normal_impulse < 0 ||
        !hit.pre_velocity.finite() || !hit.pre_omega.finite())
      throw std::runtime_error("non-finite dynamic contact impact");
  }
}

void sim_commit_impacts(SimBatch& batch, const SimContacts& impacts) {
  for (const auto& [index, hit] : impacts) {
    auto& car = batch.cars[index];
    if (!sim_live(car)) continue;
    if (hit.normal_impulse > 1) {
      car.released = true;
      car.suspended = false;
      car.pending = false;
    }
    sim_impact(batch, index, hit);
  }
}

void sim_loose_bodies(SimBatch& batch, const std::vector<size_t>& vehicles) {
  using physics::Vec3;
  std::vector<std::pair<size_t, bool>> settled;  // keep the current pose?
  for (size_t index = 0; index < batch.loose.size(); ++index) {
    auto& loose = batch.loose[index];
    if (loose.end != SimLoose::End::None) continue;
    SimCarriers carrier_backups;
    SimContacts impacts;
    bool completed = false;
    try {
      const unsigned rest_ticks = physics::VehicleDynamics::frequency / 2;
      const bool sleeping = loose.quiet_ticks >= rest_ticks;
      auto& body = loose.body;
      bool near = false, far = true;
      for (size_t vehicle : vehicles) {
        const auto& car = batch.cars[vehicle];
        if (!sim_live(car)) continue;
        near |= sim_near(body, car);
        far &= !sim_near(body, car, kSettleDistance);
      }
      if (loose.native_dynamic) {
        loose.far_awake_ticks = far && !sleeping ? loose.far_awake_ticks + 1 : 0;
        if (far && sleeping) {
          settled.push_back({index, true});
          continue;
        }
        if (loose.far_awake_ticks > lost_ticks_limit() || body.position.y < kLostHeight) {
          settled.push_back({index, false});
          continue;
        }
      }
      if (sleeping && !near) continue;
      const Vec3 before = body.position;
      const auto previous = body.corners();
      if (!sleeping) body.integrate(physics::VehicleDynamics::timestep);
      if (loose.exit_ticks) {
        const physics::RigidBody* carrier =
            loose.carrier >= 0 ? &batch.cars[static_cast<size_t>(loose.carrier)].dynamics.body
            : loose.carrier_fixed ? &loose.carrier_body
                                  : nullptr;
        if (!carrier || !leaving_carrier(body, *carrier)) loose.exit_ticks = 0;
        else --loose.exit_ticks;
      }
      bool carrier_contact = false;
      for (size_t vehicle : vehicles) {
        auto& car = batch.cars[vehicle];
        if (!sim_live(car) || !sim_near(body, car)) continue;
        if (loose.exit_ticks && car.live_id == loose.vehicle) continue;
        carrier_backups.emplace_back(&car, car.dynamics.body);
        physics::VehicleDynamics::Impact hit;
        if (collide_loose_with_car(body, previous, before, car, car.previous.frame(), hit)) {
          carrier_contact = true;
          impacts.emplace_back(vehicle, hit);
        }
      }
      if (sleeping && !carrier_contact) continue;
      const auto support = collide_loose_with_scene(body, before, loose.part);
      if (loose.native_dynamic) {
        loose.quiet_ticks = map_object_quiet_ticks(body, !carrier_contact && support.touching,
                                                   loose.quiet_ticks, loose.rest_anchor);
      } else {
        const bool quiet = !carrier_contact && support.penetrating &&
            body.velocity.dot(body.velocity) < .0025 && body.omega().dot(body.omega()) < .01;
        loose.quiet_ticks = quiet ? loose.quiet_ticks + 1 : 0;
      }
      if (loose.quiet_ticks >= rest_ticks) body.velocity = body.momentum = {};
      sim_validate_exchange(batch, body, carrier_backups, impacts);
      const auto local =
          part_matrix_multiply(part_matrix_inverse_rigid(loose.parent_frame), body.frame());
      if (!finite_frame(local)) throw std::runtime_error("non-finite loose-body frame");
      loose.local = local;
      loose.published = true;
      completed = true;
    } catch (const std::exception& error) {
      for (const auto& [car, before] : carrier_backups) car->dynamics.body = before;
      loose.message = phys_message("removed loose body", loose.resource, error.what());
      loose.end = SimLoose::End::Fault;
      batch.loose_ended.push_back(index);
    }
    if (completed) sim_commit_impacts(batch, impacts);
  }
  for (const auto& [index, keep_pose] : settled) {
    auto& loose = batch.loose[index];
    loose.end = keep_pose ? SimLoose::End::Settle : SimLoose::End::Lost;
    loose.end_frame = loose.body.frame();
    batch.loose_ended.push_back(index);
  }
}

bool advance_deformation(Object* part, Record& rec, unsigned steps) {
  using physics::Vec3;
  PartMatrix frame;
  if (!scene_instances_script_matrix(part, frame.data())) return false;
  const Vec3 x{frame[0], frame[1], frame[2]}, y{frame[4], frame[5], frame[6]},
             z{frame[8], frame[9], frame[10]}, origin{frame[12], frame[13], frame[14]};
  const bool advanced = rec.deformation->advance(steps, [&](Vec3 position, Vec3& point, Vec3& normal) {
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
  // A chassis cage that has just settled holds its plastic shape: check the
  // wheel mounts against it. Detaching waits for the end of the records loop.
  if (advanced && !rec.deformation->active() && rec.config->native_kind == "car")
    settle_wheel_mounts(rec, settled_wheel_releases);
  return advanced;
}

namespace {
// A PhysicsRef primitive has no authored mass; it is given the density of
// wood. CFG resource bodies use their own mass when they have one.
constexpr double kFreeBodyDensity = 500.0;

bool configure_free_body(const GameRefState& state, physics::RigidBody& body) {
  using physics::Vec3;
  Vec3 lo, hi, center;
  double mass = 0;
  const float* size = state.physics_size;
  if (state.physics_shape == PhysicsShape::Box) {
    hi = {size[0], size[1], size[2]};
    lo = hi * -1.0;
    mass = kFreeBodyDensity * 8.0 * size[0] * size[1] * size[2];
  } else if (state.physics_shape == PhysicsShape::Sphere) {
    hi = {size[0], size[0], size[0]};
    lo = hi * -1.0;
    mass = kFreeBodyDensity * 4.0 / 3.0 * 3.14159265358979 * size[0] * size[0] * size[0];
  } else if (state.physics_shape == PhysicsShape::Resource) {
    std::shared_ptr<const ScxGeometry> mesh;
    float authored = 0.f, c[3] = {};
    if (!scene_instances_native_body(static_cast<uint32_t>(state.type_id), mesh, authored, c))
      return false;
    const Vec3 padding{.005, .005, .005};
    lo = Vec3{mesh->info.bbox_min[0], mesh->info.bbox_min[1], mesh->info.bbox_min[2]} - padding;
    hi = Vec3{mesh->info.bbox_max[0], mesh->info.bbox_max[1], mesh->info.bbox_max[2]} + padding;
    const Vec3 extent = hi - lo;
    mass = authored > 0.f ? authored : kFreeBodyDensity * extent.x * extent.y * extent.z;
    center = authored > 0.f ? Vec3{c[0], c[1], c[2]} : (lo + hi) * .5;
  } else {
    return false;
  }
  if (!std::isfinite(mass) || !lo.finite() || !hi.finite()) return false;
  body.configure(std::clamp(mass, 1.0, 50000.0), lo, hi, center);
  return true;
}

// The GameRef pose and script velocities of a free body; throws for a pose a
// GameRef cannot hold.
void free_body_pose(const physics::RigidBody& body, const PartMatrix& parent_frame,
                    std::array<float, 3>& pos, std::array<float, 3>& ypr,
                    std::array<float, 6>& motion) {
  const auto local = part_matrix_multiply(part_matrix_inverse_rigid(parent_frame), body.frame());
  if (!finite_frame(local)) throw std::runtime_error("non-finite PhysicsRef publication frame");
  motion = script_motion(body, parent_frame);
  std::copy(local.begin() + 12, local.begin() + 15, pos.begin());
  // The inverse of local_matrix(): yaw about Y, then pitch, then roll.
  const double pitch = std::asin(std::clamp(-double(local[9]), -1.0, 1.0));
  ypr[1] = static_cast<float>(pitch);
  if (std::fabs(std::cos(pitch)) > 1e-6) {
    ypr[0] = std::atan2(local[8], local[10]);
    ypr[2] = std::atan2(local[1], local[5]);
  } else {
    ypr[0] = std::atan2(-local[2], local[0]);
    ypr[2] = 0;
  }
}

void disable_free_body(GameRefState& state, void* key, const std::array<float, 3>& pos,
                       const std::array<float, 3>& ypr, const char* reason) {
  std::fprintf(stderr, "[phys] disabled PhysicsRef id=%d: %s\n", state.live_id, reason);
  state.matrix_override = false;
  const auto finite = [](float v) { return std::isfinite(v); };
  if (!std::all_of(pos.begin(), pos.end(), finite) || !std::all_of(ypr.begin(), ypr.end(), finite))
    state.empty = true;  // No finite pose exists to render; never publish NaNs.
  std::copy(pos.begin(), pos.end(), state.pos);
  std::copy(ypr.begin(), ypr.end(), state.ypr);
  std::fill(std::begin(state.vel), std::end(state.vel), 0.f);
  std::fill(std::begin(state.angular_vel), std::end(state.angular_vel), 0.f);
  gameref_set_physics_simulated(key, false);
  free_bodies.erase(key);
}
}  // namespace

// Free PhysicsRef bodies share contacts with every active physical car.
// Bodies outside the active world wait; control handoff changes no ownership.
void sim_free_bodies(SimBatch& batch, const std::vector<size_t>& vehicles) {
  using physics::Vec3;
  for (auto& free : batch.free) {
    if (free.disabled) continue;
    SimCarriers carrier_backups;
    SimContacts impacts;
    bool completed = false;
    try {
      const unsigned rest_ticks = physics::VehicleDynamics::frequency / 2;
      const bool sleeping = free.quiet_ticks >= rest_ticks;
      auto& body = free.body;
      bool near = false;
      for (size_t vehicle : vehicles)
        if (sim_live(batch.cars[vehicle]) && sim_near(body, batch.cars[vehicle])) near = true;
      if (sleeping && !near) continue;
      const Vec3 before = body.position;
      const auto previous_body = body;
      const auto previous = body.corners();
      if (!sleeping) body.integrate(physics::VehicleDynamics::timestep);
      bool carrier_contact = false;
      for (size_t vehicle : vehicles) {
        auto& car = batch.cars[vehicle];
        if (!sim_live(car) || !sim_near(body, car)) continue;
        auto& car_body = car.dynamics.body;
        const auto car_before = car_body;
        carrier_backups.emplace_back(&car, car_before);
        physics::VehicleDynamics::Impact hit;
        bool contact = false;
        if (free.shape == PhysicsShape::Box || free.shape == PhysicsShape::Sphere) {
          // SLRR_rev v221 vehicle-3d.ts freeContactsCore: unshaped bodies use
          // the chassis OBB. Corner-only mesh probes miss a box whose upper and
          // lower corners straddle the whole car shell.
          if (const auto pair = physics::resolve_swept_vehicle_contact(body, car_body, previous_body,
                                                                       car.previous)) {
            hit = body_pair_impact(car_before, car_body, pair->point, -pair->normal,
                                   pair->normal_impulse, pair->speed);
            contact = true;
          }
        } else {
          contact = collide_loose_with_car(body, previous, before, car, car.previous.frame(), hit);
        }
        if (contact) {
          carrier_contact = true;
          impacts.emplace_back(vehicle, hit);
        }
      }
      if (sleeping && !carrier_contact) continue;
      const auto support = collide_loose_with_scene(body, before, free.key);
      free.quiet_ticks = map_object_quiet_ticks(body, !carrier_contact && support.touching,
                                                free.quiet_ticks, free.rest_anchor);
      if (free.quiet_ticks >= rest_ticks) body.velocity = body.momentum = {};
      const auto frame = body.frame();
      if (!std::all_of(frame.begin(),frame.end(),[](float v){return std::isfinite(v);}))
        throw std::runtime_error("non-finite free-body frame");
      sim_validate_exchange(batch, body, carrier_backups, impacts);
      auto pos = free.pos, ypr = free.ypr;
      auto motion = free.motion;
      free_body_pose(body, free.parent_frame, pos, ypr, motion);
      free.pos = pos;
      free.ypr = ypr;
      free.motion = motion;
      free.published = true;
      completed = true;
    } catch (const std::exception& error) {
      for (const auto& [car, before] : carrier_backups) car->dynamics.body = before;
      free.message = error.what();
      free.disabled = true;
    }
    if (completed) sim_commit_impacts(batch, impacts);
  }
}

void sim_clamp_nitro(SimCar& car) {
  car.nitro_remaining = std::clamp(car.nitro_remaining, 0., car.nitro_capacity);
  if (!(car.nitro_remaining > 0 && std::isfinite(car.nitro_rate) && car.nitro_rate > 0))
    car.nitro_active = car.nitro_flame = false;
}

// The part of configure_drive that changes between the host ticks of one
// batch; it ran in full on the host for the first tick. A parked car woken
// during a batch keeps its drive until the next frame configures it.
void sim_refresh_drive(SimCar& car, double dt) {
  using Mode = DriveRefresh::Mode;
  auto& drive = car.dynamics.drive;
  drive.nitro = 0;
  car.nitro_requested = false;
  sim_clamp_nitro(car);
  const auto steer_wheels = [&] {
    for (size_t i = 0; i < drive.wheels.size(); ++i)
      drive.wheels[i].steer = steered_wheel_angle(car.steering, car.steering_input,
                                                  car.steering_correction, i);
  };
  if (car.drive.mode == Mode::Parked) {
    if (car.steering_correction != 0.f) {
      car.steering_correction = 0.f;
      steer_wheels();
    }
    return;
  }
  if (car.drive.autonomous) {
    car.steering_input = car.steering_request = car.drive.ai_steer;
    car.steering_correction = 0;
  } else {
    steering_filter(car.steering, car.steer, car.dynamics.body, steering_grounded(car.dynamics),
                    car.steering_input, car.steering_request, car.steering_correction, dt);
  }
  steer_wheels();
  if (car.drive.mode == Mode::Steering) return;
  if (car.drive.mode == Mode::EngineLost) {
    car.dynamics.engine_running = false;
    car.dynamics.engine_omega = 0;
  } else {
    car.nitro_requested =
        car.controlled && car.drive.controls && car.nitro_input > .1f && car.released;
  }
  drive.engaged = car.released;
}

// --physics-trace (#380): the controlled car's last 0.6 s of solver steps,
// written out with the next 0.3 s whenever its body takes a vertical
// acceleration over kTraceTrigger (a kerb, a landing). The physics thread
// fills it during a batch; the host writes it at the commit.
constexpr double kTraceTrigger = 1.5 * 9.81;
struct TraceRow {
  int event = 0;
  double time = 0;
  float y = 0, vy = 0, ay = 0, ax = 0, az = 0, speed = 0, wall = 0, ground = 0;
  float length[4]{}, travel[4]{}, force[4]{}, normal_y[4]{}, normal_z[4]{};
  int contact = 0;
  float hit[3]{}, hit_ny = 0, pitch = 0, roll = 0;  // ground impact point (chassis), body spin
};
struct PhysicsTrace {
  std::string path;
  std::array<TraceRow, 600> ring{};
  size_t head = 0, count = 0;
  int after = 0, events = 0;
  double time = 0, quiet_until = 0;
  std::vector<TraceRow> out;
} physics_trace;

void trace_step(const SimCar& car, physics::Vec3 acceleration, double wall, double ground,
                unsigned steps) {
  auto& trace = physics_trace;
  const auto& body = car.dynamics.body;
  trace.time += steps * physics::VehicleDynamics::timestep;
  TraceRow row;
  row.time = trace.time;
  row.y = float(body.position.y);
  row.vy = float(body.velocity.y);
  const auto local = body.orientation.inverse_rotate(acceleration);
  row.ay = float(acceleration.y);
  row.ax = float(local.x);
  row.az = float(local.z);
  row.speed = float(body.velocity.length() * 3.6);
  row.wall = float(wall);
  row.ground = float(ground);
  if (ground > 0) {
    const auto& impact = car.dynamics.ground_impact;
    const auto at = body.orientation.inverse_rotate(impact.point - body.position) + body.center;
    row.hit[0] = float(at.x);
    row.hit[1] = float(at.y);
    row.hit[2] = float(at.z);
    row.hit_ny = float(impact.normal.normalized().y);
  }
  const auto spin = body.orientation.inverse_rotate(body.omega());
  row.pitch = float(spin.x);
  row.roll = float(spin.z);
  for (size_t i = 0; i < 4; ++i) {
    const auto& w = car.dynamics.state[i];
    const auto n = body.orientation.inverse_rotate(w.normal);
    row.length[i] = float(w.length);
    row.travel[i] = float(w.speed);
    row.force[i] = float(w.force);
    row.normal_y[i] = float(n.y);
    row.normal_z[i] = float(n.z);
    row.contact |= w.contact ? 1 << i : 0;
  }
  if (trace.after > 0) {
    row.event = trace.events;
    trace.out.push_back(row);
    --trace.after;
  } else if (std::abs(acceleration.y) > kTraceTrigger && trace.time >= trace.quiet_until) {
    ++trace.events;
    for (size_t k = 0; k < trace.count; ++k) {
      auto old = trace.ring[(trace.head + trace.ring.size() - trace.count + k) % trace.ring.size()];
      old.event = trace.events;
      trace.out.push_back(old);
    }
    row.event = trace.events;
    trace.out.push_back(row);
    trace.after = 300;
    trace.quiet_until = trace.time + 1;
    trace.count = 0;
  }
  trace.ring[trace.head] = row;
  trace.head = (trace.head + 1) % trace.ring.size();
  trace.count = std::min(trace.count + 1, trace.ring.size());
}

void trace_flush() {
  auto& trace = physics_trace;
  if (trace.out.empty() || trace.path.empty()) return;
  const bool header = !std::filesystem::exists(trace.path);
  if (FILE* f = std::fopen(trace.path.c_str(), "a")) {
    if (header) {
      std::fprintf(f, "event,t,y,vy,ay_g,ax_g,az_g,kph,wall,ground,contact,hit_x,hit_y,hit_z,hit_ny,pitch,roll");
      for (int i = 0; i < 4; ++i)
        std::fprintf(f, ",len%d,travel%d,force%d,ny%d,nz%d", i, i, i, i, i);
      std::fputc(10, f);
    }
    for (const auto& r : trace.out) {
      std::fprintf(f, "%d,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",
                   r.event, r.time, r.y, r.vy, r.ay / 9.81, r.ax / 9.81, r.az / 9.81, r.speed,
                   r.wall, r.ground, r.contact, r.hit[0], r.hit[1], r.hit[2], r.hit_ny, r.pitch,
                   r.roll);
      for (int i = 0; i < 4; ++i)
        std::fprintf(f, ",%.4f,%.3f,%.0f,%.3f,%.3f", r.length[i], r.travel[i], r.force[i],
                     r.normal_y[i], r.normal_z[i]);
      std::fputc(10, f);
    }
    std::fclose(f);
  }
  trace.out.clear();
}

// One shared substep of a stepping car (the records loop of the former
// single-threaded tick). A car that fails restores its pose before the
// substep and is suspended, as before.
void sim_step_car(SimBatch& batch, size_t index, unsigned tick, unsigned substep,
                  std::vector<size_t>& vehicles) {
  auto& car = batch.cars[index];
  const auto saved_body = car.dynamics.body;
  const auto saved_local = car.local;
  const bool was_ready = car.ready;
  try {
    // An unreleased chassis (catalog, before start) may drop onto a floor.
    // With no floor the step only applies gravity, and the car leaves the shot.
    if (!car.released && car.pending) {
      SceneFloorHit probe;
      if (!scene_instances_sample_floor(car.world[12], car.world[14], car.world[13] + 4.f,
                                        car.chassis, probe))
        return;
    }
    // The floor under the car for this step's ~40 floor queries (#380): the
    // body's XZ box, its travel over the step and a margin for the wheels,
    // in a patch rebuilt once the car leaves it (3 m of slack).
    auto& patch = *car.floor_patch;
    {
      const auto& body = car.dynamics.body;
      double x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30;
      for (const auto& p : body.corners()) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        z0 = std::min(z0, p.z);
        z1 = std::max(z1, p.z);
      }
      const double margin = 1 + body.velocity.length() * physics::VehicleDynamics::timestep *
                                    std::max(1u, car.stride);
      x0 -= margin;
      z0 -= margin;
      x1 += margin;
      z1 += margin;
      if (!scene_instances_floor_patch_covers(patch, float(x0), float(z0), float(x1),
                                              float(z1), car.chassis))
        scene_instances_floor_patch(float(x0 - 3), float(z0 - 3), float(x1 + 3),
                                    float(z1 + 3), car.chassis, patch);
    }
    const auto terrain = [&](double x, double z, double ceiling, physics::GroundHit &ground) {
      SceneFloorHit hit;
      if (!scene_instances_sample_floor(patch, static_cast<float>(x), static_cast<float>(z),
                                        static_cast<float>(ceiling), hit))
        return false;
      ground.height = hit.height;
      ground.normal = {hit.normal[0], hit.normal[1], hit.normal[2]};
      return true;
    };
    // Wind blows on cars out in the world, not in the garage or a preview.
    const double wind_time =
        batch.wind_time + (batch.step_base[tick] + substep) * physics::VehicleDynamics::timestep;
    car.dynamics.aero.wind =
        car.released ? physics::wind_velocity(batch.wind, car.dynamics.body.position, wind_time)
                     : physics::Vec3{};
    // One shared solver substep; controls remain on their 60 Hz host tick.
    const auto before_body = car.dynamics.body;
    const auto before = before_body.position;
    const auto orientation_before = car.dynamics.body.orientation;
    const auto velocity_before = car.dynamics.body.velocity;
    sim_clamp_nitro(car);
    const double nitro_rpm = car.dynamics.engine_omega * 9.54929658551372;
    const double nitro_limiter = car.dynamics.engine_limit_fraction();
    car.nitro_active = car.nitro_requested && car.dynamics.engine_running &&
        car.nitro_remaining > 0 && std::isfinite(car.nitro_rate) && car.nitro_rate > 0;
    car.dynamics.drive.nitro = car.nitro_active ? car.nitro_input : 0.;
    const auto steps =
        car.dynamics.advance(physics::VehicleDynamics::timestep, terrain, car.stride > 1);
    // MWM 454A42: RPM * .0002 * input * limiter * dt * script rate.
    // Consume once per solver step, never from sampling a curve or collecting
    // a camera/reflection. Empty tanks return to the dry bank next step.
    if (car.nitro_active && steps)
      car.nitro_remaining = std::max(0., car.nitro_remaining -
          nitro_rpm * .0002 * car.nitro_input * nitro_limiter * car.nitro_rate *
          steps * physics::VehicleDynamics::timestep);
    if (substep == 0) {
      // Original flag-256 children flicker with a 30% MSVC-rand gate per
      // render update. Sample once per 60-Hz simulation tick instead: all
      // nozzles/cameras agree, and pause freezes both fuel and presentation.
      car.nitro_random = car.nitro_random * 214013u + 2531011u;
      car.nitro_flame = car.nitro_active && nitro_rpm > 1000 &&
          car.dynamics.drive.throttle > .01 &&
          ((car.nitro_random >> 16) & 32767u) < 9830u;
    }
    if (!car.nitro_active || car.nitro_remaining <= 0)
      car.nitro_flame = false;
    // Collision belongs to the released body, not its engine/controller. A
    // coasting car still hits the world after its driver leaves or is removed.
    if (car.released) {
      const auto wall = collide_chassis(car.dynamics.body, car.probe_shift, before,
                                        batch.open ? batch.horizon_steps : batch.ticks - tick,
                                        batch.wake);
      const auto& ground = car.dynamics.ground_impact;
      sim_impact(batch, index, wall.speed >= ground.speed ? wall : ground);
      if (car.controlled && steps) {
        const auto& body = car.dynamics.body;
        const auto local_acceleration = body.orientation.inverse_rotate(
            (body.velocity - velocity_before) / (steps * physics::VehicleDynamics::timestep));
        const physics::Vec3 lean{
            std::clamp(car.steering_input * .045 * car.head_steer -
                           local_acceleration.x * .003 * car.head_acceleration, -.15, .15),
            0,
            std::clamp(body.velocity.length() * .0015 * car.head_velocity -
                           local_acceleration.z * .004 * car.head_acceleration, -.15, .15)};
        car.cockpit_offset += (lean - car.cockpit_offset) *
            -std::expm1(-physics::VehicleDynamics::timestep / .14);
        if (!physics_trace.path.empty())
          trace_step(car, (body.velocity - velocity_before) /
                              (steps * physics::VehicleDynamics::timestep),
                     wall.speed, ground.speed, steps);
      }
      const auto delta = car.dynamics.body.position - before;
      car.mileage += static_cast<float>(std::hypot(delta.x, delta.z) * .1);
    }
    car.engine_running = car.dynamics.drive.native_traffic
                             ? car.released
                             : car.dynamics.drive.torque && car.dynamics.engine_running;
    if (!finite_frame(car.dynamics.body.frame()))
      throw std::runtime_error("non-finite chassis output");
    if (steps) {
      sim_publish(car, false);
      if (substep == 0 || !car.render_blend) {
        // A passive car can first wake in the second substep. Its first drawn
        // interval must start here, never from an uninitialized world origin.
        car.render_from_position = before;
        car.render_from_orientation = orientation_before;
      }
      car.render_blend = true;
    }
    car.steps += steps;
    if (steps && car.pending) {
      if (!car.suspended)
        car.pending = false;
      else {
        bool supported = true;
        double force = 0;
        for (size_t i = 0; i < 4; ++i) {
          if (car.dynamics.wheels[i].enabled)
            supported = supported && car.dynamics.state[i].contact;
          force += car.dynamics.state[i].force;
        }
        const bool quiet = supported &&
                           car.dynamics.body.velocity.length() < .02 &&
                           car.dynamics.body.omega().length() < .02 &&
                           std::fabs(force - car.dynamics.body.mass * 9.81) <
                               car.dynamics.body.mass * 9.81 * .02;
        car.quiet_ticks = quiet ? car.quiet_ticks + steps : 0;
        // Keep the quarter-second settling window across solver frequencies.
        if (car.quiet_ticks >= physics::VehicleDynamics::frequency / 4) {
          car.pending = false;
          car.dynamics.body.velocity = {};
          car.dynamics.body.momentum = {};
        }
      }
    }
    car.previous = before_body;
    car.listed = true;
    vehicles.push_back(index);
  } catch (const std::exception& error) {
    if (!car.fault_reported) {
      car.message += phys_message("suspended chassis", car.resource, error.what());
      car.fault_reported = true;
    }
    car.dynamics.body = saved_body;
    car.dynamics.body.velocity = car.dynamics.body.momentum = {};
    car.dynamics.reset_clock();
    car.dynamics.reset_drive();
    car.dynamics.release_wheels();
    car.ready = false;
    car.suspended = true;
    car.pending = false;
    car.engine_running = car.render_blend = false;
    car.nitro_requested = car.nitro_active = car.nitro_flame = false;
    car.nitro_input = 0;
    car.dynamics.drive.nitro = 0;
    car.fault = true;
    car.fault_local = saved_local;
    car.fault_was_ready = was_ready;
  }
}

// One solver step of the batch at its cursor (#380): a host tick starts
// when the cursor is at its first substep. The physics thread calls it in a
// loop, for a whole planned batch or, free-running, one real-time step at a time.
void simulate_step(SimBatch& batch) {
  if (batch.open && batch.substep == 0) {
    batch.step_base.push_back(batch.steps_done);
    batch.step_count.push_back(1);
  }
  const unsigned tick = batch.tick, substep = batch.substep;
  const unsigned substeps = batch.step_count[tick];
  auto& vehicles = batch.vehicles;
  {
    // SLRR_rev v221 7441a697 vehicle-physics-world.ts: all bodies share
    // substeps and each unordered pair is resolved once.
    vehicles.clear();
    for (size_t i = 0; i < batch.cars.size(); ++i) {
      auto& car = batch.cars[i];
      car.listed = false;
      if (substep == 0) {
        car.engine_running = false;
        car.nitro_active = car.nitro_flame = false;
        car.dynamics.drive.nitro = 0;
        car.render_blend = false;
      }
      if (car.suspended && !car.pending) continue;
      if (substep == 0 && tick > 0 && car.configured)
        sim_refresh_drive(car, substeps * physics::VehicleDynamics::timestep);
      if (car.stride > 1 && (solver_step_clock + i) % car.stride != 0) {
        // Not this car's phase: it stays put and is listed as a passive body.
        car.dynamics.defer(physics::VehicleDynamics::timestep);
        continue;
      }
      sim_step_car(batch, i, tick, substep, vehicles);
    }
    // Settled parked cars are passive participants. Two parked showroom cars
    // do not collide, but a released car can wake one without inventing a driver.
    for (size_t i = 0; i < batch.cars.size(); ++i) {
      auto& car = batch.cars[i];
      if (car.listed || car.fault_reported || (!car.ready && car.pending)) continue;
      car.previous = car.dynamics.body;
      try { physics::validate_vehicle_contact_body(car.dynamics.body); }
      catch (const std::exception& error) { sim_suspend_contact(car, error.what()); continue; }
      car.listed = true;
      vehicles.push_back(i);
    }
    std::sort(vehicles.begin(), vehicles.end(), [&](size_t a, size_t b) {
      return batch.cars[a].live_id < batch.cars[b].live_id;
    });
    // Broadphase: a pair whose swept bounding spheres are apart cannot touch
    // (resolve_swept_vehicle_contact rejects it too, after copying and
    // validating both bodies). A contact moves its cars: refresh their spheres.
    auto& spheres = batch.spheres;
    spheres.resize(vehicles.size());
    for (size_t i = 0; i < vehicles.size(); ++i) spheres[i] = contact_sphere(batch.cars[vehicles[i]]);
    for (size_t i = 0; i < vehicles.size(); ++i) {
      auto& a = batch.cars[vehicles[i]];
      if (!sim_live(a)) continue;
      for (size_t j = i + 1; j < vehicles.size(); ++j) {
        if (!sim_live(a)) break;
        auto& b = batch.cars[vehicles[j]];
        if (!sim_live(b) || (!a.released && !b.released)) continue;
        const double reach = spheres[i].radius + spheres[j].radius;
        if ((spheres[i].center - spheres[j].center).length() > reach * 1.001 + 1e-6) continue;
        auto& body_a = a.dynamics.body;
        auto& body_b = b.dynamics.body;
        const auto before_a = body_a, before_b = body_b;
        std::optional<physics::VehicleContact> hit;
        try {
          hit = physics::resolve_swept_vehicle_contact(body_a, body_b, a.previous, b.previous);
          if (hit) {
            script_motion(body_a, a.parent);
            script_motion(body_b, b.parent);
            if (!finite_frame(body_a.frame()) || !finite_frame(body_b.frame()))
              throw std::runtime_error("unrepresentable vehicle contact frame");
          }
        } catch (const std::exception& error) {
          body_a = before_a;
          body_b = before_b;
          bool bad_a = false, bad_b = false;
          try { physics::validate_vehicle_contact_body(body_a); } catch (...) { bad_a = true; }
          try { physics::validate_vehicle_contact_body(body_b); } catch (...) { bad_b = true; }
          // The kernel is transactional. Isolate a malformed passive car, not
          // its healthy peer. Finite-body arithmetic failure belongs to this
          // pair only; unrelated cars and the host/native pump continue.
          if (bad_a || !bad_b) sim_suspend_contact(a, error.what());
          if (bad_b || !bad_a) sim_suspend_contact(b, error.what());
          continue;
        }
        if (!hit) continue;
        spheres[i] = contact_sphere(a);
        spheres[j] = contact_sphere(b);
        if (hit->normal_impulse > 1) {
          a.stride = b.stride = 1;  // an impact: both on every step
          for (auto* car : {&a, &b}) {
            car->released = true;
            car->suspended = false;
            car->pending = false;
          }
          sim_publish(a, false);
          sim_publish(b, false);
          sim_impact(batch, vehicles[i],
                     body_pair_impact(before_a, body_a, hit->point, hit->normal,
                                      hit->normal_impulse, hit->speed));
          sim_impact(batch, vehicles[j],
                     body_pair_impact(before_b, body_b, hit->point, -hit->normal,
                                      hit->normal_impulse, hit->speed));
        }
      }
    }
    sim_loose_bodies(batch, vehicles);
    sim_free_bodies(batch, vehicles);
    for (size_t index : vehicles) {
      auto& car = batch.cars[index];
      if (!sim_live(car)) continue;
      try {
        physics::validate_vehicle_contact_body(car.dynamics.body);
        car.dynamics.sync_wheels();
        sim_publish(car, substep + 1 == substeps);
      } catch (const std::exception& error) {
        sim_suspend_contact(car, error.what());
      }
    }
  }
  ++solver_step_clock;
  ++batch.steps_done;
  if (++batch.substep == substeps) {
    batch.substep = 0;
    ++batch.tick;
    if (batch.open) {
      batch.ticks = batch.tick;
      batch.solver_steps = batch.steps_done;
    }
  }
}

// The physics thread's part of a frame: every host tick of a planned batch.
void simulate_batch(SimBatch& batch) {
  const auto started = perf::now_ticks();
  batch.vehicles.reserve(batch.cars.size());
  while (batch.tick < batch.ticks) simulate_step(batch);
  batch.thread_ticks = perf::now_ticks() - started;
}

// The pose a failed chassis keeps: the one published before the failure, or
// none when it never had a finite one.
void restore_chassis_pose(GameRefState& node, const Record& rec, const PartMatrix& saved_local,
                          bool was_ready) {
  const auto& safe = finite_frame(saved_local) ? saved_local : rec.published_frame;
  if (finite_frame(saved_local) || (was_ready && finite_frame(safe))) {
    std::copy(safe.begin(),safe.end(),node.local_matrix);
    std::copy(safe.begin()+12,safe.begin()+15,node.pos);
    const double pitch = std::asin(std::clamp(-double(safe[9]),-1.0,1.0));
    node.ypr[1] = static_cast<float>(pitch);
    if (std::fabs(std::cos(pitch)) > 1e-6) {
      node.ypr[0] = std::atan2(safe[8],safe[10]);
      node.ypr[2] = std::atan2(safe[1],safe[5]);
    } else {
      node.ypr[0] = std::atan2(-safe[2],safe[0]);
      node.ypr[2] = 0;
    }
    node.matrix_override = true;
  } else {
    node.empty = true; // No valid pose has ever been published for this body.
  }
  node.render_pose = false;
  std::fill(std::begin(node.vel),std::end(node.vel),0.f);
  std::fill(std::begin(node.angular_vel),std::end(node.angular_vel),0.f);
}

// A chassis that cannot be prepared (non-finite frame or drive) is suspended
// on the host, as a failing solver step is.
void suspend_chassis(Object* chassis, Record& rec, GameRefState& node,
                     const physics::RigidBody& saved_body, const PartMatrix& saved_local,
                     bool was_ready, const char* reason) {
  if (!rec.physics_fault_reported) {
    std::fprintf(stderr,"[phys] suspended chassis type=0x%x: %s\n",rec.state.resource,reason);
    rec.physics_fault_reported = true;
  }
  rec.dynamics.body = saved_body;
  rec.dynamics.body.velocity = rec.dynamics.body.momentum = {};
  rec.dynamics.reset_clock();
  rec.dynamics.reset_drive();
  rec.dynamics.release_wheels();
  rec.physics_ready = false;
  rec.physics_suspended = true;
  rec.physics_pending_placement = false;
  rec.engine_running = rec.render_blend = false;
  stop_nitro(rec);
  (void)chassis;
  restore_chassis_pose(node, rec, saved_local, was_ready);
}

// The host's part before the physics thread runs a batch: everything that
// reads GameRef, scripts, input or options, with the configuration the
// former tick did at its first substep.
SimBatch prepare_batch(unsigned ticks, bool open = false, unsigned horizon_steps = 0) {
  SimBatch batch;
  batch.ticks = open ? 0 : ticks;
  batch.open = open;
  batch.horizon_steps = std::max(1u, horizon_steps);
  if (open) ticks = 0;
  // Solver steps per host tick from one running count, so a rate that 60
  // does not divide (1000 Hz: 16 or 17) keeps exact time (#380).
  batch.step_count.resize(ticks);
  batch.step_base.resize(ticks);
  unsigned base = 0;
  for (unsigned t = 0; t < ticks; ++t) {
    batch.step_base[t] = base;
    batch.step_count[t] = physics::VehicleDynamics::steps_in_host_tick(host_tick_clock + t);
    base += batch.step_count[t];
  }
  batch.solver_steps = base;
  host_tick_clock += ticks;
  batch.wind = world_wind;
  batch.wind_time = wind_clock;  // commit_batch advances wind_clock by the steps run
  const auto* target = gameref_find_live(controlled_vehicle);
  Object* controlled = target ? as_part(static_cast<Object*>(target->key)) : nullptr;
  const auto& options = host_game_options();
  std::vector<size_t> stepping, parked;
  for (auto& [chassis, rec] : records) {
    const auto ineligible = [&rec = rec] {
      // The per-tick reset every record not in the batch gets.
      rec.engine_running = false;
      rec.nitro_active = rec.nitro_flame = false;
      rec.dynamics.drive.nitro = 0;
      rec.render_blend = false;
    };
    if (rec.external_motion || rec.config->native_kind != "car" || rec.state.parent) {
      ineligible();
      continue;
    }
    auto* node = gameref_get(chassis);
    if (!node || !contact_visible(chassis)) {
      ineligible();
      continue;
    }
    SimCar car;
    const bool active = !(rec.physics_suspended && !rec.physics_pending_placement);
    PartMatrix frame, parent = eye_matrix();
    if (active) {
      const auto saved_body = rec.dynamics.body;
      const auto saved_local = node_local(chassis);
      const bool was_ready = rec.physics_ready;
      try {
        if (!scene_instances_script_matrix(chassis, frame.data()) ||
            (node->parent && !scene_instances_script_matrix(node->parent, parent.data()))) {
          ineligible();
          continue;
        }
        const auto local = node_local(chassis);
        if (!finite_frame(frame) || !finite_frame(parent) || !finite_frame(local))
          throw std::runtime_error("non-finite chassis frame");
        bool external = node->parent != rec.physics_parent;
        for (size_t i = 0; i < 16; ++i)
          external = external || std::fabs(local[i] - rec.published_frame[i]) > 1e-5f;
        if (external) rec.physics_ready = false;
        if ((!rec.physics_ready || rec.physics_dirty) && !dynamics_configure(chassis, rec, frame)) {
          ineligible();
          continue;
        }
        dynamics_wheels(rec);
        configure_anti_roll(rec);
        car.drive = configure_drive(chassis, rec, chassis == controlled);
        car.configured = true;
        if (rec.body_support_dirty) {
          rebuild_aerodynamics(chassis, rec);
          rebuild_body_support(chassis, rec);
        }
        car.local = local;
      } catch (const std::exception& error) {
        suspend_chassis(chassis, rec, *node, saved_body, saved_local, was_ready, error.what());
        ineligible();
        continue;
      }
    } else {
      if (rec.physics_fault_reported || (!rec.physics_ready && rec.physics_pending_placement)) {
        ineligible();
        continue;
      }
      if ((node->parent && !scene_instances_script_matrix(node->parent, parent.data())) ||
          !scene_instances_script_matrix(chassis, frame.data())) {
        ineligible();
        continue;
      }
      const auto local = node_local(chassis);
      const bool external = node->parent != rec.physics_parent || local != rec.published_frame;
      // setMatrix/park has authority over an asleep car too. Never publish a
      // stale solver body back over a new scripted placement.
      if (external || !rec.physics_ready || rec.physics_dirty) {
        if (!dynamics_configure(chassis, rec, frame, true)) {
          ineligible();
          continue;
        }
        rec.dynamics.body.velocity = rec.dynamics.body.momentum = {};
        rec.published_frame = local;
      }
      car.local = local;
    }
    car.chassis = chassis;
    car.rec = &rec;
    car.incarnation = node->incarnation;
    car.resource = rec.state.resource;
    car.controlled = chassis == controlled;
    car.parent = parent;
    car.world = frame;
    car.dynamics = rec.dynamics;
    car.floor_patch = rec.floor_patch;
    car.ready = rec.physics_ready;
    car.released = rec.physics_released;
    car.suspended = rec.physics_suspended;
    car.pending = rec.physics_pending_placement;
    car.fault_reported = rec.physics_fault_reported;
    car.quiet_ticks = rec.physics_quiet_ticks;
    car.engine_running = rec.engine_running;
    car.render_blend = rec.render_blend;
    car.render_from_position = rec.render_from_position;
    car.render_from_orientation = rec.render_from_orientation;
    car.nitro_remaining = rec.nitro_remaining;
    car.nitro_capacity = nitro_capacity(chassis);
    car.nitro_rate = comp(chassis, "consumption_nitro");
    car.nitro_input = rec.nitro_input;
    car.nitro_requested = rec.nitro_requested;
    car.nitro_active = rec.nitro_active;
    car.nitro_flame = rec.nitro_flame;
    car.nitro_random = rec.nitro_random;
    car.steering_input = rec.steering_input;
    car.steering_request = rec.steering_request;
    car.steering_correction = rec.steering_correction;
    car.cockpit_offset = rec.cockpit_offset;
    car.mileage = rec.mileage;
    car.head_steer = options.head_steer;
    car.head_acceleration = options.head_acceleration;
    car.head_velocity = options.head_velocity;
    car.steering = steering_geometry(rec);
    car.steer = steering_input(car.drive.controls);
    if (rec.deformation) {
      const auto probes = chassis_probe_points(rec.dynamics.body, .18);
      for (size_t i = 0; i < probes.size(); ++i)
        car.probe_shift[i] = rec.deformation->displacement(probes[i]);
    }
    (active ? stepping : parked).push_back(batch.cars.size());
    batch.cars.push_back(std::move(car));
  }
  if (open) {
    const SimCar* player = nullptr;
    for (const auto& car : batch.cars)
      if (car.controlled && car.released) player = &car;
    for (auto& car : batch.cars)
      if (player && !car.controlled && car.released && car.dynamics.drive.native_traffic)
        car.stride = traffic_stride(
            (car.dynamics.body.position - player->dynamics.body.position).length());
  }
  // Live ids order the pair contacts; give them in the former order: stepping
  // cars first, then parked ones.
  for (auto* order : {&stepping, &parked})
    for (size_t index : *order) {
      auto& car = batch.cars[index];
      gameref_make_live(car.chassis);
      car.live_id = gameref_get(car.chassis)->live_id;
    }

  for (auto it = loose_panels.begin(); it != loose_panels.end();) {
    auto current = it++;
    auto* part = current->first;
    auto& loose = current->second;
    auto* node = gameref_get(part);
    auto* rec = record(part);
    const bool native_dynamic = rec && rec->config->native_kind == "native_dynamic";
    PartMatrix parent_frame;
    if (!node || !node->parent) {
      loose_panels.erase(current);
      part_destroy(part);
      continue;
    }
    if (!scene_instances_script_matrix(node->parent, parent_frame.data()) ||
        (native_dynamic && !scene_instances_ground_frame(node->parent, parent_frame.data()))) {
      // Its world is no longer the active one (the garage, or a new visit to
      // the city with a new GroundRef). A map prop becomes scenery again where
      // it last was, so the instances it replaced are not left hidden.
      if (native_dynamic && rec->physics_ready) {
        const auto frame = loose.body.frame();
        scene_instances_settle_dynamic(part, frame.data());
      }
      loose_panels.erase(current);
      part_destroy(part);
      continue;
    }
    if (native_dynamic && !rec->physics_ready) {
      // Placement ownership is the map, not whichever car was controlled when
      // the prop woke. The first tick resolves its ground-shell transform.
      loose.body.set_frame(part_matrix_multiply(parent_frame, rec->published_frame));
      rec->physics_ready = true;
    }
    SimLoose sim;
    sim.part = part;
    sim.resource = rec ? rec->state.resource : 0;
    sim.native_dynamic = native_dynamic;
    sim.parent_frame = parent_frame;
    sim.body = loose.body;
    sim.vehicle = loose.vehicle;
    sim.quiet_ticks = loose.quiet_ticks;
    sim.far_awake_ticks = loose.far_awake_ticks;
    sim.exit_ticks = loose.exit_ticks;
    sim.rest_anchor = loose.rest_anchor;
    if (loose.exit_ticks)
      if (const auto* owner = gameref_find_live(loose.vehicle))
        if (const auto* carrier = record(static_cast<Object*>(owner->key))) {
          for (size_t i = 0; i < batch.cars.size() && sim.carrier < 0; ++i)
            if (batch.cars[i].rec == carrier) sim.carrier = static_cast<int>(i);
          if (sim.carrier < 0) {
            sim.carrier_fixed = true;
            sim.carrier_body = carrier->dynamics.body;
          }
        }
    batch.loose.push_back(std::move(sim));
  }

  std::unordered_set<void*> live;
  for (auto* state : gameref_simulated_bodies()) {
    void* key = state->key;
    PartMatrix parent_frame;
    if (!state->parent || !scene_instances_script_matrix(state->parent, parent_frame.data()))
      continue;
    live.insert(key);
    auto& free = free_bodies[key];
    // Keep the published pose until the complete step has succeeded.
    const std::array<float, 3> saved_pos = free.configured
        ? std::array{free.pos[0],free.pos[1],free.pos[2]}
        : std::array{state->pos[0],state->pos[1],state->pos[2]};
    const std::array<float, 3> saved_ypr = free.configured
        ? std::array{free.ypr[0],free.ypr[1],free.ypr[2]}
        : std::array{state->ypr[0],state->ypr[1],state->ypr[2]};
    try {
      const bool moved = !std::equal(free.pos, free.pos + 3, state->pos) ||
                         !std::equal(free.ypr, free.ypr + 3, state->ypr);
      if (!free.configured || free.live_id != state->live_id || moved) {
        // New body, reused key or setMatrix: start at rest at the script pose.
        if (!free.configured || free.live_id != state->live_id) {
          free = FreeBody{};
          free.live_id = state->live_id;
          if (!configure_free_body(*state, free.body)) {
            gameref_set_physics_simulated(key, false);
            continue;
          }
          free.configured = true;
        }
        float local[16];
        PartMatrix script_local;
        scene_instances_local_matrix(*state, local);
        std::copy(local, local + 16, script_local.begin());
        free.body.set_frame(part_matrix_multiply(parent_frame, script_local));
        free.body.velocity = free.body.momentum = {};
        free.quiet_ticks = 0;
        std::copy(state->pos, state->pos + 3, free.pos);
        std::copy(state->ypr, state->ypr + 3, free.ypr);
      }
    } catch (const std::exception& error) {
      disable_free_body(*state, key, saved_pos, saved_ypr, error.what());
      live.erase(key);
      continue;
    }
    SimFree sim;
    sim.key = key;
    sim.live_id = state->live_id;
    sim.shape = state->physics_shape;
    sim.parent_frame = parent_frame;
    sim.body = free.body;
    sim.quiet_ticks = free.quiet_ticks;
    sim.rest_anchor = free.rest_anchor;
    std::copy(free.pos, free.pos + 3, sim.pos.begin());
    std::copy(free.ypr, free.ypr + 3, sim.ypr.begin());
    batch.free.push_back(std::move(sim));
  }
  std::erase_if(free_bodies, [&](const auto& entry) { return !live.contains(entry.first); });

  // Car shells for loose and free bodies; parts keep their place on the
  // chassis during a batch. Only a car that can reach such a body within the
  // batch needs them: the contact test (sim_near) with this batch's travel and
  // a margin for acceleration.
  const double batch_seconds =
      open ? horizon_steps * physics::VehicleDynamics::timestep
           : ticks / double(physics::VehicleDynamics::update_frequency);
  const auto may_meet = [&](const physics::RigidBody& car, const physics::RigidBody& body) {
    const auto gap = body.position - car.position;
    const double reach = ((car.maximum - car.minimum).length() +
                          (body.maximum - body.minimum).length()) * .5 + 3 +
        (car.velocity.length() + body.velocity.length()) * (batch_seconds + .1) * 2;
    return gap.dot(gap) < reach * reach;
  };
  if (!batch.loose.empty() || !batch.free.empty())
    for (auto& car : batch.cars) {
      const auto& body = car.dynamics.body;
      if (std::none_of(batch.loose.begin(), batch.loose.end(),
                       [&](const SimLoose& loose) { return may_meet(body, loose.body); }) &&
          std::none_of(batch.free.begin(), batch.free.end(),
                       [&](const SimFree& free) { return may_meet(body, free.body); }))
        continue;
      const auto inverse_script = part_matrix_inverse_rigid(world_matrix(car.chassis));
      for (const auto& installed : part_installed_tree(car.chassis)) {
        auto* part = record(installed.object);
        if (part != car.rec && part->config->category != 2) continue;
        const auto& mesh = part_collision_mesh(*part);
        if (mesh.indices.empty()) continue;
        car.shells.push_back(
            {&mesh, part_matrix_multiply(inverse_script, world_matrix(installed.object))});
      }
    }
  return batch;
}

// The host's part after a batch: write the bodies back, then publish them and
// apply damage, crush cages, loose-body lifetimes and scene changes in solver
// order.
// Force feedback: the hardest hit on the controlled car since the last
// update (-1..1, positive turns the wheel left), from the committed impacts.
double feedback_jolt = 0;
void commit_batch(SimBatch& batch) {
  trace_flush();
  wind_clock += batch.solver_steps * physics::VehicleDynamics::timestep;
  std::vector<Object*> released;
  const auto current = [](const SimCar& car) -> GameRefState* {
    if (record(car.chassis) != car.rec) return nullptr;
    auto* node = gameref_get(car.chassis);
    return node && node->incarnation == car.incarnation ? node : nullptr;
  };
  for (auto& car : batch.cars) {
    auto* node = current(car);
    if (!node) continue;
    auto& rec = *car.rec;
    rec.dynamics = car.dynamics;
    rec.physics_ready = car.ready;
    rec.physics_released = car.released;
    rec.physics_suspended = car.suspended;
    rec.physics_pending_placement = car.pending;
    rec.physics_fault_reported = car.fault_reported;
    rec.physics_quiet_ticks = car.quiet_ticks;
    rec.engine_running = car.engine_running;
    rec.render_blend = car.render_blend;
    rec.render_from_position = car.render_from_position;
    rec.render_from_orientation = car.render_from_orientation;
    if (car.render_blend) rec.render_parent = car.parent;
    rec.nitro_remaining = car.nitro_remaining;
    rec.nitro_input = car.nitro_input;
    rec.nitro_requested = car.nitro_requested;
    rec.nitro_active = car.nitro_active;
    rec.nitro_flame = car.nitro_flame;
    rec.nitro_random = car.nitro_random;
    rec.steering_input = car.steering_input;
    rec.steering_request = car.steering_request;
    rec.steering_correction = car.steering_correction;
    rec.cockpit_offset = car.cockpit_offset;
    rec.mileage = car.mileage;
    if (!car.message.empty()) std::fputs(car.message.c_str(), stderr);
    if (car.fault) {
      if (car.publish) rec.published_frame = car.local;
      stop_nitro(rec);
      restore_chassis_pose(*node, rec, car.fault_local, car.fault_was_ready);
    } else if (car.contact_fault) {
      if (car.publish) {
        // The pose it published before the failure, without its motion.
        const auto& local = car.local;
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
        rec.published_frame = local;
      }
      node->render_pose = false;
      std::fill(std::begin(node->vel), std::end(node->vel), 0.f);
      std::fill(std::begin(node->angular_vel), std::end(node->angular_vel), 0.f);
    } else if (car.publish) {
      dynamics_sync(car.chassis, rec, car.parent, car.publish > 1);
    }
  }
  // One sound per car per batch, from its hardest hit: at 1000 Hz a crash
  // reports many ticks' contacts in the same frame.
  std::unordered_map<size_t, const physics::VehicleDynamics::Impact*> loudest;
  for (const auto& impact : batch.impacts) {
    const auto& car = batch.cars[impact.car];
    if (!current(car)) continue;
    damage_chassis(car.chassis, *car.rec, impact.body, impact.hit, car.controlled, released);
    if (car.controlled && impact.hit.speed > 1) {
      // A hit on the left of the car drags that side back: the wheel turns
      // left. 10 m/s and harder is a full jolt.
      const auto& body = impact.body;
      const auto local = body.orientation.inverse_rotate(impact.hit.point - body.position);
      const double jolt = std::copysign(std::min(1.0, impact.hit.speed / 10), -local.x);
      if (std::abs(jolt) > std::abs(feedback_jolt)) feedback_jolt = jolt;
    }
    auto& hit = loudest[impact.car];
    if (!hit || impact.hit.speed > hit->speed) hit = &impact.hit;
  }
  for (const auto& [car_index, hit] : loudest)
    if (current(batch.cars[car_index])) play_impact_audio(*batch.cars[car_index].rec, *hit);
  // Crush cages advance by the substeps the batch ran: other parts' cages as
  // the records loop advanced them, each car's by its own solver steps.
  const unsigned substeps = batch.solver_steps;
  for (auto& [part, rec] : records) {
    if (rec.config->native_kind == "car" || !rec.deformation || !rec.deformation->active() ||
        !advance_deformation(part, rec, substeps))
      continue;
    rec.deformation_upload_ticks += substeps;
    if (rec.deformation_upload_ticks >= 3 || !rec.deformation->active()) {
      publish_deformation(part, rec);
      rec.deformation_upload_ticks = 0;
    }
    rec.state.tear = std::min(rec.state.tear, rec.deformation->condition());
    part->fields["tear"] = Value::make_float(rec.state.tear);
    if (rec.state.parent && rec.deformation->released()) released.push_back(part);
  }
  for (auto& car : batch.cars) {
    if (!car.steps || !current(car)) continue;
    auto& rec = *car.rec;
    if (rec.deformation && rec.deformation->active() &&
        advance_deformation(car.chassis, rec, car.steps)) {
      rec.deformation_upload_ticks += car.steps;
      if (rec.deformation_upload_ticks >= physics::VehicleDynamics::frequency / 20 ||
          !rec.deformation->active()) {
        publish_deformation(car.chassis, rec);
        car.chassis->fields["tear"] = Value::make_float(rec.state.tear);
        rec.deformation_upload_ticks = 0;
      }
    }
  }
  for (const auto& loose : batch.loose) {
    const auto found = loose_panels.find(loose.part);
    if (found == loose_panels.end()) continue;
    auto& panel = found->second;
    panel.body = loose.body;
    panel.quiet_ticks = loose.quiet_ticks;
    panel.far_awake_ticks = loose.far_awake_ticks;
    panel.exit_ticks = loose.exit_ticks;
    panel.rest_anchor = loose.rest_anchor;
    if (!loose.published || loose.end == SimLoose::End::Fault) continue;
    auto* rec = record(loose.part);
    if (loose.native_dynamic && rec) {
      for (size_t i = 0; i < rec->renders.size() && i < panel.render_offsets.size(); ++i)
        if (auto* state = gameref_get(rec->renders[i])) {
          const auto slot = part_matrix_multiply(loose.local, panel.render_offsets[i]);
          std::copy(slot.begin(), slot.end(), state->local_matrix);
        }
    } else if (auto* node = gameref_get(loose.part)) {
      std::copy(loose.local.begin(), loose.local.end(), node->local_matrix);
    }
  }
  for (size_t index : batch.loose_ended) {
    const auto& loose = batch.loose[index];
    if (!loose_panels.contains(loose.part)) continue;
    if (loose.end == SimLoose::End::Fault) {
      std::fputs(loose.message.c_str(), stderr);
      if (loose.native_dynamic) scene_instances_settle_dynamic(loose.part, nullptr);
    } else {
      scene_instances_settle_dynamic(
          loose.part, loose.end == SimLoose::End::Settle ? loose.end_frame.data() : nullptr);
    }
    loose_panels.erase(loose.part);
    part_destroy(loose.part);
  }
  for (const auto& free : batch.free) {
    auto* state = gameref_get(free.key);
    const auto found = free_bodies.find(free.key);
    if (!state || state->live_id != free.live_id || found == free_bodies.end()) continue;
    if (free.disabled) {
      disable_free_body(*state, free.key, free.pos, free.ypr, free.message.c_str());
      continue;
    }
    auto& body = found->second;
    body.body = free.body;
    body.quiet_ticks = free.quiet_ticks;
    body.rest_anchor = free.rest_anchor;
    if (!free.published) continue;
    state->matrix_override = false;
    std::copy(free.pos.begin(), free.pos.end(), state->pos);
    std::copy(free.ypr.begin(), free.ypr.end(), state->ypr);
    std::copy(free.motion.begin(), free.motion.begin() + 3, state->vel);
    std::copy(free.motion.begin() + 3, free.motion.end(), state->angular_vel);
    std::copy(free.pos.begin(), free.pos.end(), body.pos);
    std::copy(free.ypr.begin(), free.ypr.end(), body.ypr);
  }
  for (const auto& box : batch.wake) scene_instances_wake_dynamics(box.data(), box.data() + 3);
  // After all contact iteration: creating a loose body inserts into `records`.
  scene_instances_promote_dynamics();
  // Wheels whose mounts failed when a chassis cage settled (advance_deformation).
  released.insert(released.end(), settled_wheel_releases.begin(), settled_wheel_releases.end());
  settled_wheel_releases.clear();
  for (auto* part : released)
    if (auto* panel = record(part))
      if (auto* chassis = car(part))
        if (auto* vehicle = record(chassis))
          detach_panel(part, *panel, chassis, *vehicle);
}

// Host ticks run at a fixed 60 Hz, but frames come at the display rate.
// Drawing the last tick made turns and motion step whenever a frame ran zero
// or two ticks, so each car is drawn between its last two ticks, by the
// fraction of a tick that is still unsimulated. (The Classic framerate mode
// that drew the last tick is gone.) Scripts and physics keep the ticked pose.
void publish_render_poses(double alpha) {
  perf::Scope timing(perf::Zone::Interpolation);
  alpha = std::clamp(alpha, 0., 1.);
  for (auto& [chassis, rec] : records) {
    if (rec.config->native_kind != "car") continue;
    auto* node = gameref_get(chassis);
    if (!node) continue;
    node->render_pose = false;
    if (!rec.render_blend || !node->matrix_override) continue;
    const auto& body = rec.dynamics.body;
    auto from = rec.render_from_orientation;
    const auto& to = body.orientation;
    // Blend along the shorter arc.
    const double sign =
        from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w < 0 ? -1 : 1;
    from = {from.x * sign, from.y * sign, from.z * sign, from.w * sign};
    physics::RigidBody drawn = body;
    drawn.position = rec.render_from_position + (body.position - rec.render_from_position) * alpha;
    drawn.orientation = physics::Quaternion{from.x + (to.x - from.x) * alpha,
                                            from.y + (to.y - from.y) * alpha,
                                            from.z + (to.z - from.z) * alpha,
                                            from.w + (to.w - from.w) * alpha}.normalized();
    const auto local = part_matrix_multiply(part_matrix_inverse_rigid(rec.render_parent),
                                            drawn.frame());
    std::copy(local.begin(), local.end(), node->render_matrix);
    std::copy(node->local_matrix, node->local_matrix + 16, node->render_basis);
    node->render_pose = true;
  }
}

void transform_point(const PartMatrix& m, float x, float y, float z, float out[3]) {
  out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
  out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
  out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

float ped_unit() {
  ped_rng = ped_rng * 1664525u + 1013904223u;
  return (ped_rng >> 8) * (1.f / 16777216.f);
}

void pose_steering_wheels() {
  for (auto& entry : records) {
    auto& rec = entry.second;
    if (!rec.config || rec.config->native_kind != "car" || !rec.config->has_steering)
      continue;
    const PartSlot* mount = nullptr;
    for (const auto& slot : rec.config->slots)
      if (slot.type == 11) mount = &slot;
    if (!mount) continue;
    const auto child = rec.children.find(mount->id);
    if (child == rec.children.end()) continue;
    auto* node = gameref_get(child->second);
    auto* wheel = record(child->second);
    if (!node || !wheel) continue;
    PartMatrix mount_frame;
    if (!part_slot_matrix(*rec.config, *mount, mount_frame)) continue;
    PartMatrix own = eye_matrix();
    for (const auto& slot : wheel->config->slots)
      if (slot.id == wheel->state.own_slot) {
        part_slot_matrix(*wheel->config, slot, own);
        break;
      }
    PartMatrix spin = eye_matrix();
    // A road car's steering ratio (#383): 14 degrees of steering wheel per
    // degree at the road wheels, about 1.4 turns to lock. The column used to
    // turn like the road wheels, a few degrees at speed.
    constexpr double kSteeringRatio = 14;
    const float angle = static_cast<float>(chassis_steering_angle(rec) * kSteeringRatio);
    const float c = std::cos(angle), s = std::sin(angle);
    spin[0] = c;
    spin[1] = s;
    spin[4] = -s;
    spin[5] = c;
    const PartMatrix posed = part_matrix_multiply(
        mount_frame, part_matrix_multiply(spin, part_matrix_inverse_rigid(own)));
    std::copy(posed.begin(), posed.end(), node->local_matrix);
    node->matrix_override = true;
  }
}

// Instrument values, shared by the overlay HUD and the dashboard pins.
// Without an engine the tachometer rests at zero.
double gauge_rpm(const Record& rec) {
  return rec.dynamics.drive.torque ? rec.dynamics.engine_omega * 9.54929658551372 : 0;
}
double gauge_speed(const Record& rec) {
  return rec.dynamics.body.velocity.length() * 3.6;
}
// Dashboard pins follow their chassis every frame (#301), like the column.
void pose_cockpit_needles() {
  for (auto& entry : records) {
    auto& rec = entry.second;
    for (const auto& needle : rec.needles) {
      auto* node = gameref_get(needle.node);
      if (!node) continue;
      const auto& cfg = rec.config->cockpit[static_cast<size_t>(needle.gauge)];
      const double value = needle.gauge == CockpitGauge::Rpm ? gauge_rpm(rec) : gauge_speed(rec);
      const auto local = cockpit_needle_matrix(cfg, value);
      std::copy(local.begin(), local.end(), node->local_matrix);
      node->matrix_override = true;
    }
  }
}

float wheel_rim_radius(Object* wheel) {
  const auto* rec = record(wheel);
  if (!rec) return 0.15f;
  float radius = 0.f;
  for (auto* render : rec->renders) {
    const auto* node = gameref_get(render);
    if (!node) continue;
    const auto mesh = scene_instances_geometry(static_cast<uint32_t>(node->type_id));
    if (!mesh) continue;
    const float scale = mesh->position_scale > 0.f ? mesh->position_scale : 0.01f;
    for (size_t i = 0; i + 1 < mesh->positions.size(); i += 3) {
      const float x = mesh->positions[i] * scale;
      const float y = mesh->positions[i + 1] * scale;
      radius = std::max(radius, std::sqrt(x * x + y * y));
    }
  }
  return radius >= 0.05f && radius <= 0.5f ? radius : 0.15f;
}

void* chassis_root(Object* chassis);

void release_driver(DriverSeat& seat) {
  gameref_release_render_mask(seat.duplicate_mask);
  if (seat.owned && seat.render && gameref_get(seat.render))
    gameref_remove(seat.render);
  seat.render = nullptr;
  seat.owned = false;
  seat.gear = -1;
  seat.shifting = false;
}

Object* player_render() {
  if (!runtime) return nullptr;
  const Value player = runtime->get_static("java.game.GameLogic", "player");
  if (player.tag != ValueTag::Obj || !player.v.o) return nullptr;
  Object* render = part_field(player.v.o, "render");
  const auto* node = gameref_get(render);
  if (!node || node->kind != GameRefKind::RenderRef || !node->type_id) return nullptr;
  return render;
}

void hide_duplicate_drivers(Object* chassis, Object* keep, int32_t type, DriverSeat& seat) {
  gameref_mask_duplicate_renders(seat.duplicate_mask, chassis_root(chassis), keep, type);
}

void pose_driver() {
  Object* chassis = part_controlled_chassis();
  for (auto it = drivers.begin(); it != drivers.end();) {
    if (it->first != chassis) {
      release_driver(it->second);
      it = drivers.erase(it);
    } else {
      ++it;
    }
  }
  if (!chassis || !runtime || !index) return;
  auto* rec = record(chassis);
  if (!rec || !rec->config || !rec->config->has_seat) return;
  DriverSeat& seat = drivers[chassis];
  Object* scripted = player_render();
  if (scripted) {
    if (seat.render != scripted) {
      if (seat.owned && seat.render && gameref_get(seat.render))
        gameref_remove(seat.render);
      seat.render = scripted;
      seat.owned = false;
      std::fprintf(stderr, "[driver] vin seated\n");
    }
  } else if (!seat.render) {
    const RpkPack* pack = index->pack_by_name("humans.rpk");
    if (!pack || !index->model_for_resource(pack->pack_id, 0x000C).ok) return;
    Object* render = runtime->new_object("java.util.resource.RenderRef");
    auto* node = gameref_upsert(render, GameRefKind::RenderRef);
    gameref_set_type(*node, static_cast<int32_t>(RpkIndex::make_full_id(pack->pack_id, 0x000C)));
    node->script = render;
    node->alias = "driver";
    node->empty = false;
    gameref_set_parent(render, chassis);
    node->matrix_override = true;
    seat.render = render;
    seat.owned = true;
    std::fprintf(stderr, "[driver] vin seated\n");
  }
  auto* node = gameref_get(seat.render);
  if (!node) {
    seat.render = nullptr;
    seat.owned = false;
    return;
  }
  if (node->parent != chassis) gameref_set_parent(seat.render, chassis);
  node->empty = false;
  const PartMatrix seat_m = part_pose_matrix(rec->config->seat);
  std::copy(seat_m.begin(), seat_m.end(), node->local_matrix);
  node->matrix_override = true;
  hide_duplicate_drivers(chassis, seat.render, node->type_id, seat);
  // Solve from the bind pose every frame. Carrying last frame's joints makes
  // the measured limb length shrink until the body collapses onto the wheel.
  character_reset_pose(seat.render);
  const PartMatrix inv_seat = part_matrix_inverse_rigid(seat_m);
  const auto to_cm = [&](float x, float y, float z, float out[3]) {
    float metres[3];
    transform_point(inv_seat, x, y, z, metres);
    for (int i = 0; i < 3; ++i) out[i] = metres[i] * 100.f;
  };
  const int32_t type = node->type_id;
  const auto bone = [&](const char* name) {
    return character_find_bone(seat.render, type, *index, name);
  };
  // The first cockpit camera sits where the head is, and it leans back
  // with acceleration: from inside the car, the head would fill the view.
  // Shrink it to its joint while this car's camera 0 draws the world.
  if (const auto cockpit = chassis->fields.find("_hostCockpitCamera");
      cockpit != chassis->fields.end() && cockpit->second.v.o) {
    const GameRefState* camera = gameref_get(cockpit->second.v.o);
    if (camera && camera->cockpit_index == 0 && camera == fullscreen_script_camera()) {
      const int head = bone("Bip01 Head");
      if (head >= 0) character_scale_bone(seat.render, type, *index, head, 1e-3f);
    }
  }
  if (!rec->config->cameras.empty()) {
    const auto& camera = rec->config->cameras[0].position;
    float target[3];
    to_cm(camera[0], camera[1], camera[2], target);
    // The head joint sits ~7 cm above the neck. The neck is what the chain reaches.
    target[1] -= 7.2f;
    const int spine = bone("Bip01 Spine");
    const int spine1 = bone("Bip01 Spine1");
    const int neck = bone("Bip01 Neck");
    if (spine >= 0 && spine1 >= 0 && neck >= 0) {
      // A straight spine cannot shorten by aiming along itself. Bend the
      // middle of the back toward the seat (+Z), not the chest up into the chin.
      const float pole[3] = {0.f, 0.f, 1.f};
      character_solve_limb(seat.render, type, *index, spine, spine1, neck, target, pole);
    }
  }
  const PartSlot* mount = nullptr;
  for (const auto& slot : rec->config->slots)
    if (slot.type == 11) mount = &slot;
  Object* wheel = nullptr;
  if (mount) {
    const auto child = rec->children.find(mount->id);
    if (child != rec->children.end()) wheel = child->second;
  }
  if (wheel) {
    const auto* wheel_node = gameref_get(wheel);
    if (wheel_node) {
      PartMatrix wheel_m;
      std::copy(wheel_node->local_matrix, wheel_node->local_matrix + 16, wheel_m.begin());
      const float grip_r = std::max(0.04f, wheel_rim_radius(wheel) - 0.015f);
      const char* sides[2] = {"L", "R"};
      // Gear-shift gesture (build 606, 0x0044D0C0 and 0x0044D260): on each
      // gear change the hand on the console side (the right one when the
      // seat is left of centre) goes to the lever knob, `shifter` plus an H
      // offset for the new gear, and returns to the wheel 0.7 s later. The
      // exe re-targets the hand at once; here it eases over 0.15 s each way.
      // Physics ticks, so a pause holds the gesture.
      static constexpr float kShiftGrid[8][3] = {
          {0.f, 0.f, 0.f},     {-.05f, 0.f, -.05f}, {-.05f, 0.f, .05f}, {0.f, 0.f, -.05f},
          {0.f, 0.f, .05f},    {.05f, 0.f, -.05f},  {.05f, 0.f, .05f},  {-.09f, 0.f, -.05f}};
      const int gear = std::clamp(rec->dynamics.gear, 0, 7);
      if (seat.gear >= 0 && gear != seat.gear) {
        seat.shift_tick = rec->dynamics.ticks;
        seat.shifting = true;
      }
      seat.gear = gear;
      float lever_weight = 0.f;
      if (seat.shifting) {
        const double t = double(rec->dynamics.ticks - seat.shift_tick) *
                         physics::VehicleDynamics::timestep;
        if (t >= .7) seat.shifting = false;
        else lever_weight = float(std::clamp(std::min(t, .7 - t) / .15, 0., 1.));
      }
      const int shift_side = rec->config->seat.position[0] < 0.f ? 1 : 0;
      float lever[3], lever_x[3], lever_down[3];
      {
        const float* knob = rec->config->shifter;
        const float* cell = kShiftGrid[gear];
        const float p[3] = {knob[0] + cell[0], knob[1] + cell[1], knob[2] + cell[2]};
        to_cm(p[0], p[1], p[2], lever);
        to_cm(p[0] + .01f, p[1], p[2], lever_x);
        to_cm(p[0], p[1] - .01f, p[2], lever_down);
        for (int i = 0; i < 3; ++i) {
          lever_x[i] -= lever[i];
          lever_down[i] -= lever[i];
        }
      }
      for (int side = 0; side < 2; ++side) {
        const float sign = side == 0 ? -1.f : 1.f;
        const float grip_angle = sign;
        float chassis_point[3];
        transform_point(wheel_m, std::sin(grip_angle) * grip_r, std::cos(grip_angle) * grip_r,
                        0.f, chassis_point);
        // `palm` is the rim's centreline at the grip. Start with the wrist
        // 7.8 cm behind it along the wheel's axis toward the driver (the
        // seat's +Z is not that axis on a raked column); refined below.
        float palm[3];
        to_cm(chassis_point[0], chassis_point[1], chassis_point[2], palm);
        physics::Vec3 axis{wheel_m[8], wheel_m[9], wheel_m[10]};
        const physics::Vec3 to_seat{seat_m[12] - wheel_m[12], seat_m[13] - wheel_m[13],
                                    seat_m[14] - wheel_m[14]};
        if (axis.x * to_seat.x + axis.y * to_seat.y + axis.z * to_seat.z < 0) axis = axis * -1.;
        float target[3];
        to_cm(chassis_point[0] + float(axis.x) * 0.078f, chassis_point[1] + float(axis.y) * 0.078f,
              chassis_point[2] + float(axis.z) * 0.078f, target);
        const std::string prefix = std::string("Bip01 ") + sides[side] + " ";
        const int upper = bone((prefix + "UpperArm").c_str());
        const int lower = bone((prefix + "Forearm").c_str());
        const int hand = bone((prefix + "Hand").c_str());
        const int clavicle = bone((prefix + "Clavicle").c_str());
        if (upper < 0 || lower < 0 || hand < 0) continue;
        if (clavicle >= 0)
          character_aim_bone(seat.render, type, *index, clavicle, upper, target, 0.8f);
        const float pole[3] = {sign * 0.5f, -1.f, 0.f};
        character_solve_limb(seat.render, type, *index, upper, lower, hand, target, pole);
        // Hold the rim: the stock fist (one hand bone, no fingers) has its grip
        // hole at about (9.5, 0, -1.2) cm in hand space, open along the hand's
        // Y axis, palm toward -Z. Put that hole on the rim's centreline, its
        // axis along the rim, the palm toward the wheel's centre and the hand
        // as straight with the forearm as that allows; re-solve the arm to the
        // resulting wrist (two passes converge to under a millimetre).
        float rim_ahead[3], centre[3];
        {
          float p[3];
          transform_point(wheel_m, std::sin(grip_angle) * grip_r + std::cos(grip_angle) * 0.01f,
                          std::cos(grip_angle) * grip_r - std::sin(grip_angle) * 0.01f, 0.f, p);
          to_cm(p[0], p[1], p[2], rim_ahead);
          to_cm(wheel_m[12], wheel_m[13], wheel_m[14], centre);
        }
        const auto unit = [](float* v) {
          const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
          if (n > 1e-6f) for (int i = 0; i < 3; ++i) v[i] /= n;
          return n > 1e-6f;
        };
        float tangent[3] = {rim_ahead[0] - palm[0], rim_ahead[1] - palm[1], rim_ahead[2] - palm[2]};
        float inward[3] = {centre[0] - palm[0], centre[1] - palm[1], centre[2] - palm[2]};
        if (side == shift_side && lever_weight > 0.f) {
          // On the knob the fist closes across the car (hole along X), palm
          // down. Blend the hole, its axis and the palm direction.
          const float w = lever_weight;
          float across[3], down[3];
          std::copy(lever_x, lever_x + 3, across);
          std::copy(lever_down, lever_down + 3, down);
          unit(tangent);
          unit(inward);
          unit(across);
          unit(down);
          for (int i = 0; i < 3; ++i) {
            palm[i] += (lever[i] - palm[i]) * w;
            tangent[i] += (across[i] - tangent[i]) * w;
            inward[i] += (down[i] - inward[i]) * w;
          }
        }
        float hand_x[3] = {}, hand_y[3] = {};
        bool oriented = false;
        const bool frame = unit(tangent) && unit(inward);
        for (int pass = 0; frame && pass < 2; ++pass) {
          float elbow[16];
          if (!character_bone_metres(seat.render, lower, elbow)) break;
          float f[3] = {palm[0] - elbow[12] * 100.f, palm[1] - elbow[13] * 100.f,
                        palm[2] - elbow[14] * 100.f};
          const float ft = f[0] * tangent[0] + f[1] * tangent[1] + f[2] * tangent[2];
          for (int i = 0; i < 3; ++i) hand_x[i] = f[i] - ft * tangent[i];
          if (!unit(hand_x)) break;
          std::copy(tangent, tangent + 3, hand_y);
          float hand_z[3] = {hand_x[1] * hand_y[2] - hand_x[2] * hand_y[1],
                             hand_x[2] * hand_y[0] - hand_x[0] * hand_y[2],
                             hand_x[0] * hand_y[1] - hand_x[1] * hand_y[0]};
          // The palm (-Z) faces the wheel's centre.
          if (hand_z[0] * inward[0] + hand_z[1] * inward[1] + hand_z[2] * inward[2] > 0)
            for (int i = 0; i < 3; ++i) { hand_y[i] = -hand_y[i]; hand_z[i] = -hand_z[i]; }
          for (int i = 0; i < 3; ++i)
            target[i] = palm[i] - hand_x[i] * 9.5f + hand_z[i] * 1.2f;
          character_solve_limb(seat.render, type, *index, upper, lower, hand, target, pole);
          oriented = true;
        }
        if (oriented) {
          // X first, then Y: Y is already perpendicular to X, so the second
          // turn is a twist about X and keeps it.
          character_align_axis(seat.render, type, *index, hand, 0, hand_x);
          character_align_axis(seat.render, type, *index, hand, 1, hand_y);
        }
      }
    }
  }
  if (rec->config->has_pedals) {
    const char* sides[2] = {"L", "R"};
    for (int side = 0; side < 2; ++side) {
      const float sign = side == 0 ? -1.f : 1.f;
      float target[3];
      to_cm(rec->config->pedals[0] + sign * 0.1f, rec->config->pedals[1],
            rec->config->pedals[2], target);
      const std::string prefix = std::string("Bip01 ") + sides[side] + " ";
      const int thigh = bone((prefix + "Thigh").c_str());
      const int calf = bone((prefix + "Calf").c_str());
      const int foot = bone((prefix + "Foot").c_str());
      if (thigh < 0 || calf < 0 || foot < 0) continue;
      const float pole[3] = {0.f, 0.5f, -1.f};
      character_solve_limb(seat.render, type, *index, thigh, calf, foot, target, pole);
      // The foot bone's X is the toe. Left on the shin it points along the
      // leg, out through the pedal. Hang it down in the seat frame.
      const float down[3] = {0.f, -1.f, 0.f};
      character_align_axis(seat.render, type, *index, foot, 0, down);
    }
  }
}

std::vector<std::string> cfg_words(std::string line) {
  const auto cut = line.find_first_of(";#");
  if (cut != std::string::npos) line.resize(cut);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::istringstream input(line);
  std::vector<std::string> words;
  for (std::string word; input >> word;) words.push_back(std::move(word));
  return words;
}

bool parse_ped_kind(uint32_t type_id, PedKind* out) {
  if (!index || !out) return false;
  auto bytes = index->read_payload(type_id);
  std::string text(bytes.begin(), bytes.end());
  if (text.find("eof") == std::string::npos) {
    std::string relative = index->payload_prop(type_id, "native");
    if (relative.rfind("ped ", 0) == 0 || relative.rfind("ped\t", 0) == 0)
      relative.erase(0, 4);
    else
      relative = index->payload_prop(type_id, "sourcefile");
    while (!relative.empty() && (relative.front() == ' ' || relative.front() == '\t'))
      relative.erase(relative.begin());
    if (relative.empty()) return false;
    std::ifstream file(std::filesystem::path(index->game_root()) / relative, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    text = buffer.str();
  }
  uint32_t render = 0, anim = 0;
  PartPose pose{};
  bool have_pose = false;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    const auto words = cfg_words(line);
    if (words.empty()) continue;
    if (words[0] == "render" && words.size() >= 2 && render == 0) {
      render = static_cast<uint32_t>(std::strtoul(words[1].c_str(), nullptr, 0));
      if (words.size() >= 8) {
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) {
          char* end = nullptr;
          pose.position[i] = std::strtof(words[2 + i].c_str(), &end);
          ok = end && *end == '\0' && std::isfinite(pose.position[i]);
          pose.rotation[i] = std::strtof(words[5 + i].c_str(), &end);
          ok = ok && end && *end == '\0' && std::isfinite(pose.rotation[i]);
        }
        have_pose = ok;
      }
    } else if (words[0] == "anim" && words.size() >= 2 && anim == 0) {
      anim = static_cast<uint32_t>(std::strtoul(words[1].c_str(), nullptr, 0));
    }
  }
  if (!render) return false;
  const int pack = RpkIndex::pack_from_full(type_id);
  auto full = [&](uint32_t local) {
    const uint32_t resolved = index->resolve_embedded_id(pack, local);
    return resolved ? resolved : RpkIndex::make_full_id(pack, local);
  };
  out->render = full(render);
  out->anim = anim ? full(anim) : 0;
  if (!have_pose) pose = {};
  out->pose = pose;
  if (const auto mesh = scene_instances_geometry(out->render)) {
    const float scale = mesh->position_scale > 0.f ? mesh->position_scale : 0.01f;
    out->sole = mesh->info.bbox_min[1] * scale;
  }
  out->ready = index->model_for_resource(RpkIndex::pack_from_full(out->render),
                                         out->render & 0xffffu)
                   .ok;
  return out->ready;
}

void clear_ped_agents(PedCrowd& crowd) {
  for (auto& agent : crowd.agents)
    if (agent.render) gameref_remove(agent.render);
  crowd.agents.clear();
  crowd.announced = false;
}

void* chassis_root(Object* chassis) {
  void* key = chassis;
  for (int guard = 0; guard < 64; ++guard) {
    const auto* node = gameref_get(key);
    if (!node || !node->parent) return key;
    key = node->parent;
  }
  return key;
}

PedestrianLines& city_walk() {
  static PedestrianLines lines;
  static bool loaded = false;
  if (!loaded && index) {
    loaded = true;
    const std::filesystem::path path =
        std::filesystem::path(index->game_root()) / "maps" / "city" / "ai" / "city.walk";
    std::ifstream in(path, std::ios::binary);
    if (!in) return lines;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (lines.load(text))
      std::fprintf(stderr, "[ped] city.walk rows=%d\n", lines.rows());
  }
  return lines;
}

struct PedSlot {
  void* crowd = nullptr;
  size_t index = 0;
  uint32_t gen = 0;
  PedestrianWalker line;
};

// Steps walker records only. Render objects and the skeleton stay on the
// thread that owns them; this one never calls into either.
struct PedSim {
  std::mutex mu;
  std::condition_variable_any cv;
  bool posted = false;
  bool ready = false;
  float dt = 0.f;
  std::vector<PedestrianCar> cars;
  PedestrianLines* walk = nullptr;
  std::vector<PedSlot> in, out;
  void run(std::stop_token stop);
  std::jthread thread{[this](std::stop_token stop) { run(stop); }};
};

void PedSim::run(std::stop_token stop) {
  std::unique_lock lock(mu);
  while (!stop.stop_requested()) {
    cv.wait(lock, stop, [&] { return posted; });
    if (stop.stop_requested()) return;
    auto slots = std::move(in);
    const float step_dt = dt;
    const std::vector<PedestrianCar> obstacles = cars;
    PedestrianLines* lines = walk;
    // Keep the job busy until its result is published. Otherwise the main
    // thread can submit another snapshot from before this step completed.
    lock.unlock();
    if (lines && step_dt > 0.f) {
      // All walkers in one step: they steer around each other.
      std::vector<PedestrianWalker> walkers;
      walkers.reserve(slots.size());
      for (const auto& slot : slots) walkers.push_back(slot.line);
      lines->step(walkers, step_dt, obstacles.data(), static_cast<int>(obstacles.size()));
      for (size_t i = 0; i < slots.size(); ++i) slots[i].line = walkers[i];
    }
    lock.lock();
    out = std::move(slots);
    posted = false;
    ready = true;
  }
}

PedSim& ped_sim() {
  static PedSim sim;
  return sim;
}

// The chase camera's vertical field, the same angle mtxProj receives.
// A point outside that cone is the SLRR_rev "hidden" spawn.
struct PedView {
  float eye[3]{};
  float forward[3]{};
  float right[3]{};
  float up[3]{};
  float tan_x = 1.f;
  float tan_y = 1.f;
  bool ok = false;
};

bool ped_point_hidden(float x, float y, float z, void* user) {
  const auto* view = static_cast<const PedView*>(user);
  if (!view || !view->ok) return false;
  const float dx = x - view->eye[0];
  const float dy = y - view->eye[1];
  const float dz = z - view->eye[2];
  const float depth = dx * view->forward[0] + dy * view->forward[1] + dz * view->forward[2];
  if (depth <= 0.2f) return true;
  const float side = dx * view->right[0] + dy * view->right[1] + dz * view->right[2];
  const float lift = dx * view->up[0] + dy * view->up[1] + dz * view->up[2];
  return std::fabs(side) > depth * view->tan_x || std::fabs(lift) > depth * view->tan_y;
}

PedView ped_view() {
  PedView view;
  float at[3]{}, up[3] = {0.f, 1.f, 0.f}, fov = 42.f;
  if (!gameref_world_camera(view.eye, at, &fov, nullptr, nullptr, up)) return view;
  float fx = at[0] - view.eye[0], fy = at[1] - view.eye[1], fz = at[2] - view.eye[2];
  const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
  if (!(fl > 1e-4f)) return view;
  fx /= fl;
  fy /= fl;
  fz /= fl;
  float rx = up[1] * fz - up[2] * fy;
  float ry = up[2] * fx - up[0] * fz;
  float rz = up[0] * fy - up[1] * fx;
  float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
  if (!(rl > 1e-4f)) return view;
  rx /= rl;
  ry /= rl;
  rz /= rl;
  view.forward[0] = fx;
  view.forward[1] = fy;
  view.forward[2] = fz;
  view.right[0] = rx;
  view.right[1] = ry;
  view.right[2] = rz;
  view.up[0] = fy * rz - fz * ry;
  view.up[1] = fz * rx - fx * rz;
  view.up[2] = fx * ry - fy * rx;
  view.tan_y = std::tan(std::clamp(fov, 10.f, 140.f) * 0.5f * 3.14159265f / 180.f);
  view.tan_x = view.tan_y * (800.f / 600.f);
  view.ok = true;
  return view;
}

void tick_pedestrians(double seconds) {
  const float dt = static_cast<float>(std::min(seconds, 0.1));
  for (auto it = crowds.begin(); it != crowds.end();) {
    if (!gameref_get(it->first)) {
      clear_ped_agents(it->second);
      it = crowds.erase(it);
    } else {
      ++it;
    }
  }
  Object* chassis = part_controlled_chassis();
  if (!chassis || !runtime || !index) return;
  PartMatrix car;
  if (!scene_instances_script_matrix(chassis, car.data())) return;
  const float car_x = car[12], car_y = car[13], car_z = car[14];
  PedView view = ped_view();
  PedestrianLines& lines = city_walk();
  {
    PedSim& sim = ped_sim();
    std::lock_guard lock(sim.mu);
    if (sim.ready) {
      for (const PedSlot& slot : sim.out) {
        auto found = crowds.find(slot.crowd);
        if (found == crowds.end() || slot.index >= found->second.agents.size()) continue;
        PedAgent& agent = found->second.agents[slot.index];
        if (agent.sim_gen != slot.gen || !agent.line.ok) continue;
        agent.line = slot.line;
        // Left through a gate with no row behind it: hide the body.
        if (!agent.line.ok) {
          agent.placed = false;
          ++agent.sim_gen;
        }
      }
      sim.ready = false;
    }
  }
  const bool routed = lines.rows() > 0;
  void* root = chassis_root(chassis);
  PartMatrix parent_m;
  const bool rooted = root && root != chassis &&
                      scene_instances_script_matrix(root, parent_m.data());
  const PartMatrix parent_inv = rooted ? part_matrix_inverse_rigid(parent_m) : eye_matrix();
  static double clock = 0;
  clock += seconds;
  for (auto& entry : crowds) {
    PedCrowd& crowd = entry.second;
    if (crowd.kinds.size() != crowd.types.size()) crowd.kinds.resize(crowd.types.size());
    int usable = 0;
    for (size_t i = 0; i < crowd.types.size(); ++i) {
      if (crowd.kinds[i].ready) {
        if (crowd.kinds[i].render) ++usable;
        continue;
      }
      const auto* type = gameref_get(crowd.types[i]);
      const uint32_t id = type ? static_cast<uint32_t>(type->type_id) : 0;
      if (!id || !parse_ped_kind(id, &crowd.kinds[i])) {
        crowd.kinds[i].ready = true;
        crowd.kinds[i].render = 0;
        if (id) std::fprintf(stderr, "[ped] type 0x%x has no walk render\n", id);
        continue;
      }
      ++usable;
    }
    // Far enough that people are already on the sidewalk before they enter
    // the view. 150 m clipped them into existence along the street.
    constexpr float kRadius = 400.f;
    const float slider = std::max(0.f, host_game_options().pedestrian_density);
    // The map coefficients are people per square metre of sidewalk. At the
    // slider maximum that asked for about twenty people inside 150 m.
    constexpr float kDensityScale = 4.f;
    crowd.density = crowd.map_density * slider * kDensityScale;
    float area = 0.f;
    int want = 0;
    if (crowd.density > 0.f && usable > 0 && routed) {
      // StreetLegal stores people per square metre. The tick truncates
      // density times the walkable area, with no fixed headcount.
      area = lines.area_within(car_x, car_y, car_z, kRadius);
      want = static_cast<int>(crowd.density * area);
    }
    auto retire = [](PedAgent& agent) {
      agent.line.ok = false;
      agent.placed = false;
      ++agent.sim_gen;
      if (auto* hidden = gameref_get(agent.render)) hidden->empty = true;
    };
    auto make_agent = [&]() -> PedAgent* {
      PedAgent agent;
      const int kind_count = static_cast<int>(crowd.kinds.size());
      agent.kind = -1;
      for (int step = 0; step < kind_count; ++step) {
        const int candidate = (crowd.next_kind + step) % kind_count;
        if (!crowd.kinds[static_cast<size_t>(candidate)].render) continue;
        agent.kind = candidate;
        crowd.next_kind = (candidate + 1) % kind_count;
        break;
      }
      if (agent.kind < 0) return nullptr;
      const PedKind& kind = crowd.kinds[agent.kind];
      Object* render = runtime->new_object("java.util.resource.RenderRef");
      auto* node = gameref_upsert(render, GameRefKind::RenderRef);
      gameref_set_type(*node, static_cast<int32_t>(kind.render));
      node->script = render;
      node->alias = "pedestrian";
      node->empty = true;
      node->matrix_override = true;
      if (rooted) gameref_set_parent(render, root);
      if (kind.anim) {
        character_bind_animation(render, render, static_cast<int32_t>(kind.anim), *index);
        character_play(render, true);
        const float duration = character_duration(render);
        if (duration > 0.f) character_seek(render, ped_unit() * duration);
      }
      agent.render = render;
      crowd.agents.push_back(agent);
      return &crowd.agents.back();
    };
    if (!crowd.preloaded && usable > 0 && want > 0) {
      std::vector<uint32_t> ids;
      for (const PedKind& kind : crowd.kinds)
        if (kind.render) ids.push_back(kind.render);
      scene_instances_preload(ids);
      crowd.preloaded = true;
    }
    int active = 0;
    PedAgent* spare = nullptr;
    for (auto& agent : crowd.agents) {
      if (agent.line.ok) ++active;
      else if (!spare) spare = &agent;
    }
    if (clock >= crowd.prune_at) {
      std::vector<PedAgent*> order;
      for (auto& agent : crowd.agents)
        if (agent.line.ok) order.push_back(&agent);
      std::sort(order.begin(), order.end(), [&](const PedAgent* a, const PedAgent* b) {
        const float da = std::hypot(a->line.x - car_x, a->line.z - car_z);
        const float db = std::hypot(b->line.x - car_x, b->line.z - car_z);
        return da > db;
      });
      int remaining = static_cast<int>(order.size());
      for (PedAgent* agent : order) {
        const float away = std::hypot(agent->line.x - car_x, agent->line.z - car_z);
        if (remaining > want || away > kRadius + 50.f) {
          retire(*agent);
          --remaining;
        }
      }
      active = remaining;
      crowd.prune_at = clock + 0.5;
    }
    PedestrianBlock car_block{car_x, car_y, car_z, 2.5f};
    if (const auto* body = gameref_get(chassis)) {
      car_block.vx = body->vel[0];
      car_block.vy = body->vel[1];
      car_block.vz = body->vel[2];
      if (auto mesh = scene_instances_geometry(static_cast<uint32_t>(body->type_id))) {
        const float scale = mesh->position_scale > 0.f ? mesh->position_scale : 0.01f;
        float extent = 0.f;
        for (int axis = 0; axis < 3; ++axis) {
          const float span = (mesh->info.bbox_max[axis] - mesh->info.bbox_min[axis]) * scale;
          extent += span * span;
        }
        const float radius = 0.5f * std::sqrt(extent) + 1.f;
        // A part resource that is not the body mesh reports a city-sized box
        // and would reject every sidewalk point after the first spawn.
        if (radius >= 1.f && radius <= 12.f) car_block.radius = radius;
      }
    }
    if (routed && active < want && clock >= crowd.refill_at) {
      // A full refill in one tick is the hitch, and it puts people on the
      // street in front of the camera. A few per frame, off screen, walk in.
      const int batch = 3;
      bool stalled = false;
      for (int made = 0; active < want && made < batch; ++made) {
        if (!spare) {
          for (auto& agent : crowd.agents)
            if (!agent.line.ok) {
              spare = &agent;
              break;
            }
        }
        if (!spare) spare = make_agent();
        if (!spare) {
          stalled = true;
          break;
        }
        std::vector<PedestrianBlock> taken{car_block};
        for (const auto& other : crowd.agents)
          if (&other != spare && other.line.ok)
            taken.push_back({other.line.x, other.line.y, other.line.z, 2.f});
        if (!lines.spawn(car_x, car_y, car_z, kRadius, 12.f, ped_rng, spare->line,
                         taken.data(), static_cast<int>(taken.size()),
                         &ped_point_hidden, &view)) {
          stalled = true;
          break;
        }
        spare->x = spare->line.x;
        spare->z = spare->line.z;
        spare->yaw = spare->line.yaw;
        spare->placed = true;
        ++spare->sim_gen;
        spare = nullptr;
        ++active;
      }
      if (!crowd.stocked && (active >= want || stalled)) crowd.stocked = true;
      crowd.refill_at = stalled ? clock + 0.25 : clock;
    }
    if (crowd.density > 0.f && clock >= crowd.report_at) {
      std::fprintf(stderr,
                   "[ped] want=%d active=%d density=%.5f slider=%.2f area=%.0f radius=%.1f\n",
                   want, active, crowd.density, slider, area, car_block.radius);
      crowd.report_at = clock + 2.0;
    }
    if (!crowd.announced && active > 0) {
      std::fprintf(stderr, "[ped] %d walkers\n", active);
      crowd.announced = true;
    }
    for (auto& agent : crowd.agents) {
      if (!agent.render || !gameref_get(agent.render)) continue;
      auto* node = gameref_get(agent.render);
      if (!agent.line.ok) {
        node->empty = true;
        continue;
      }
      if (routed) {
        // The walker already turns at the original rate (pi per metre).
        agent.yaw = agent.line.yaw;
        agent.x = agent.line.x;
        agent.z = agent.line.z;
        agent.placed = true;
        // The original advances the walk clip by the distance moved plus
        // half the turn, so one metre walked is one second of clip.
        character_set_speed(agent.render, agent.line.gait);
      }
      const PedKind& kind = crowd.kinds[static_cast<size_t>(agent.kind)];
      // Feet on the corridor point. The cfg render offset is not part of the
      // streamed mount; the mesh sole is the skinned bounds minimum.
      const float ground = routed ? agent.line.y : car_y;
      PartMatrix world = eye_matrix();
      const float cy = std::cos(agent.yaw), sy = std::sin(agent.yaw);
      world[0] = cy;
      world[2] = -sy;
      world[8] = sy;
      world[10] = cy;
      world[12] = agent.x;
      world[13] = ground - kind.sole;
      world[14] = agent.z;
      const PartMatrix local = rooted ? part_matrix_multiply(parent_inv, world) : world;
      std::copy(local.begin(), local.end(), node->local_matrix);
      node->matrix_override = true;
      // Kept in the scene so the skinned mesh is uploaded once, at spawn,
      // not on the frame the camera turns toward them. The draw itself
      // skips anyone behind the camera.
      node->empty = false;
    }
  }
  // Pause still consumes a finished job and publishes render poses above,
  // but must not submit zero-duration snapshots with the previous gait.
  if (dt <= 0.f) return;
  bool wake = false;
  {
    PedSim& sim = ped_sim();
    std::lock_guard lock(sim.mu);
    // ready may become true between the result-consumption lock above and
    // this lock. Do not enqueue stale agent positions over an unread result.
    if (sim.posted || sim.ready) {
      ped_backlog = std::min(0.1f, ped_backlog + dt);
    } else {
      sim.in.clear();
      for (auto& entry : crowds) {
        const auto& agents = entry.second.agents;
        for (size_t i = 0; i < agents.size(); ++i) {
          if (!agents[i].line.ok) continue;
          PedSlot slot;
          slot.crowd = entry.first;
          slot.index = i;
          slot.gen = agents[i].sim_gen;
          slot.line = agents[i].line;
          sim.in.push_back(std::move(slot));
        }
      }
      if (!sim.in.empty()) {
        // Frames that passed while the worker was busy still count.
        sim.dt = std::min(0.1f, dt + ped_backlog);
        ped_backlog = 0.f;
        sim.cars.clear();
        PedestrianCar car{car_x, car_y, car_z, 0.f, 0.f};
        if (const auto* body = gameref_get(chassis)) {
          car.vx = body->vel[0];
          car.vz = body->vel[2];
        }
        sim.cars.push_back(car);
        sim.walk = &lines;
        sim.posted = true;
        wake = true;
      }
    }
  }
  if (wake) ped_sim().cv.notify_one();
}

// Hinged panels (flap_hinge.hpp) run at a fixed 240 Hz on frame time, so a
// script open or close also moves in the garage, where the chassis is
// suspended. A released car carries its free panels: its velocity change over
// the frame, its wind and gravity act on them in the parent frame. The main
// physics frame never moves for a panel.
constexpr double kFlapStep = 1.0 / 240;
double flap_elapsed = 0;

// An unlatched panel of a moving car against the static scene and native
// objects, with the oriented box of its bounds in its current pose: the same
// box/sphere and wall-triangle queries collide_chassis uses. The box is swept
// from the last frame's centre so a fast pass cannot step over a trunk. A hit
// turns the panel out of the obstacle, swings it shut or tears it off
// (flap_contact); the car itself does not feel the panel's contacts.
// Returns false without a contact.
bool flap_world_contact(Object *part, Record &rec, const physics::FlapGeometry &geometry,
                        const PartMatrix &parent_world, const physics::RigidBody &body,
                        physics::FlapContact &outcome) {
  using physics::Vec3;
  const auto world = part_matrix_multiply(parent_world, node_local(part));
  const Vec3 lo = rec.flap_box_min, hi = rec.flap_box_max, mid = (lo + hi) * .5;
  const float half[3] = {std::max(float(hi.x - lo.x) * .5f, .02f),
                         std::max(float(hi.y - lo.y) * .5f, .02f),
                         std::max(float(hi.z - lo.z) * .5f, .02f)};
  PartMatrix box = world;
  const Vec3 centre{world[0] * mid.x + world[4] * mid.y + world[8] * mid.z + world[12],
                    world[1] * mid.x + world[5] * mid.y + world[9] * mid.z + world[13],
                    world[2] * mid.x + world[6] * mid.y + world[10] * mid.z + world[14]};
  const Vec3 from = rec.flap_world_valid ? rec.flap_world_centre : centre;
  rec.flap_world_centre = centre;
  rec.flap_world_valid = true;
  const Vec3 path = centre - from;
  // A teleport is not a sweep.
  const int samples = path.length() > 5 ? 1
      : static_cast<int>(std::clamp(std::ceil(path.length() / .15), 1.0, 6.0));
  SceneBodyContact hit;
  bool found = false;
  for (int sample = 1; sample <= samples && !found; ++sample) {
    const Vec3 at = path.length() > 5 ? centre : from + path * (double(sample) / samples);
    box[12] = float(at.x);
    box[13] = float(at.y);
    box[14] = float(at.z);
    SceneBodyContact shape, wall;
    const bool shape_hit = scene_instances_contact_box(box.data(), half, shape);
    const bool wall_hit = scene_instances_contact_walls(box.data(), half, wall);
    if (!shape_hit && !wall_hit) continue;
    hit = shape_hit && (!wall_hit || shape.penetration >= wall.penetration) ? shape : wall;
    found = true;
  }
  if (!found) return false;
  // Into the parent frame.
  const Vec3 point{hit.point[0] - parent_world[12], hit.point[1] - parent_world[13],
                   hit.point[2] - parent_world[14]};
  const auto inward = [&](Vec3 v) {
    return Vec3{parent_world[0] * v.x + parent_world[1] * v.y + parent_world[2] * v.z,
                parent_world[4] * v.x + parent_world[5] * v.y + parent_world[6] * v.z,
                parent_world[8] * v.x + parent_world[9] * v.y + parent_world[10] * v.z};
  };
  const Vec3 local_point = inward(point);
  const Vec3 local_normal = inward({hit.normal[0], hit.normal[1], hit.normal[2]});
  // The panel point's velocity: the car at that point plus the swing.
  const Vec3 axis{geometry.axis[0], geometry.axis[1], geometry.axis[2]};
  const Vec3 arm = local_point - Vec3{geometry.pivot[0], geometry.pivot[1], geometry.pivot[2]};
  const Vec3 swing = axis.cross(arm - axis * arm.dot(axis)) * rec.flap.rate;
  const Vec3 velocity =
      inward(body.point_velocity({hit.point[0], hit.point[1], hit.point[2]})) + swing;
  const double p[3] = {local_point.x, local_point.y, local_point.z};
  const double n[3] = {local_normal.x, local_normal.y, local_normal.z};
  outcome = physics::flap_contact(rec.flap, geometry, p, n, -velocity.dot(local_normal),
                                  hit.penetration);
  return true;
}

void advance_flaps(double seconds) {
  using physics::Vec3;
  flap_elapsed = std::min(flap_elapsed + std::min(seconds, .1), .1);
  const int steps = static_cast<int>(flap_elapsed / kFlapStep);
  flap_elapsed -= steps * kFlapStep;
  std::vector<Object *> torn;
  for (auto &[part, rec] : records) {
    if (!rec.state.parent) {
      if (rec.flap_parent) flap_geometry_for(part, rec);  // taken off: reset
      continue;
    }
    if (rec.flap.latched && rec.flap.angle == 0) {
      // Latched panels only need their mount check, which a toggle or a hit
      // performs before it changes the state.
      rec.flap_carrier_valid = rec.flap_struck = false;
      continue;
    }
    physics::FlapGeometry geometry;
    if (!flap_geometry_for(part, rec, &geometry)) continue;
    PartMatrix frame;
    if (!scene_instances_script_matrix(rec.state.parent, frame.data())) continue;
    // World to parent frame (the script frames are rigid).
    const auto inward = [&](Vec3 v) {
      return Vec3{frame[0] * v.x + frame[1] * v.y + frame[2] * v.z,
                  frame[4] * v.x + frame[5] * v.y + frame[6] * v.z,
                  frame[8] * v.x + frame[9] * v.y + frame[10] * v.z};
    };
    const auto outward = [&](const double (&p)[3]) {
      return Vec3{frame[0] * p[0] + frame[4] * p[1] + frame[8] * p[2] + frame[12],
                  frame[1] * p[0] + frame[5] * p[1] + frame[9] * p[2] + frame[13],
                  frame[2] * p[0] + frame[6] * p[1] + frame[10] * p[2] + frame[14]};
    };
    physics::FlapLoads loads;
    const Vec3 gravity = inward({0, -9.81, 0});
    loads.gravity[0] = gravity.x;
    loads.gravity[1] = gravity.y;
    loads.gravity[2] = gravity.z;
    Object *chassis = car(part);
    Record *vehicle = chassis ? record(chassis) : nullptr;
    // A panel bent beyond its closed pose no longer latches (2% RMS strain
    // halves the cage condition).
    rec.flap.latch_broken = rec.deformation && rec.deformation->condition() < .5f;
    const bool carried = vehicle && vehicle->physics_ready && vehicle->physics_released &&
                         !vehicle->physics_suspended;
    if (carried) {
      const auto &body = vehicle->dynamics.body;
      const Vec3 pivot = outward(geometry.pivot);
      const double centre[3] = {geometry.pivot[0] + geometry.com[0],
                                geometry.pivot[1] + geometry.com[1],
                                geometry.pivot[2] + geometry.com[2]};
      const Vec3 velocity = body.point_velocity(pivot), omega = body.omega();
      const Vec3 air = inward(vehicle->dynamics.aero.wind - body.point_velocity(outward(centre)));
      loads.air[0] = air.x;
      loads.air[1] = air.y;
      loads.air[2] = air.z;
      if (rec.flap_carrier_valid && !rec.flap_struck) {
        const Vec3 dv = velocity - rec.flap_carrier_velocity;
        const Vec3 dw = omega - rec.flap_carrier_omega;
        // A teleport or a reset is not a push.
        if (dv.length() < 25 && dw.length() < 15) {
          const Vec3 local_dv = inward(dv), local_dw = inward(dw);
          const double v[3] = {local_dv.x, local_dv.y, local_dv.z};
          const double w[3] = {local_dw.x, local_dw.y, local_dw.z};
          physics::flap_carrier_impulse(rec.flap, geometry, v, w);
        }
      }
      rec.flap_carrier_velocity = velocity;
      rec.flap_carrier_omega = omega;
      rec.flap_carrier_valid = true;
    } else {
      rec.flap_carrier_valid = false;
    }
    rec.flap_struck = false;
    bool moved = false;
    for (int i = 0; i < steps; ++i)
      moved = physics::flap_step(rec.flap, geometry, loads, kFlapStep) || moved;
    if (moved) place_child(rec.state.parent, rec.state.parent_slot, false);
    // An open panel of a driven car meets the world in the pose just drawn.
    // Not in the garage, where the car is suspended and a door may open into
    // the lift.
    physics::FlapContact contact = physics::FlapContact::Blocked;
    if (carried && !rec.flap.latched && rec.flap_box &&
        flap_world_contact(part, rec, geometry, frame, vehicle->dynamics.body, contact)) {
      if (contact == physics::FlapContact::Torn) {
        torn.push_back(part);
        continue;
      }
      // A push turns it; a stop it was pressed onto may have moved it too.
      place_child(rec.state.parent, rec.state.parent_slot, false);
      moved = true;
    } else if (!carried || rec.flap.latched) {
      rec.flap_world_valid = false;
    }
    if (!moved) continue;
    // Children follow through the render tree; only linked visuals inside the
    // panel's subtree need their anchors again.
    for (const auto &item : part_installed_tree(part))
      if (const auto *installed = record(item.object); installed && !installed->links.empty())
        relink_one(item.object);
  }
  // After the loop: tearing off changes the assembly. detach_panel makes the
  // panel a loose body with the car's velocity at its pose.
  for (Object *part : torn)
    if (auto *panel = record(part))
      if (auto *chassis = car(part))
        if (auto *vehicle = record(chassis)) detach_panel(part, *panel, chassis, *vehicle);
}

} // namespace

void part_set_wind(double speed, double heading_degrees) {
  world_wind.speed = std::isfinite(speed) ? std::max(0.0, speed) : 0.0;
  world_wind.heading = std::isfinite(heading_degrees) ? heading_degrees : 0.0;
}

void part_pause_engine_audio(bool paused) {
  // Trigger sounds are short one-shots, not resumable engine loops. Retire
  // them on pause; resuming a held button is not another press.
  if (paused)
    for (auto &[part, rec] : records) {
      host_sfx_stop(rec.nitro_voice);
      rec.nitro_voice = 0;
    }
  // These voice IDs belong to the engine/exhaust loops, including any
  // currently stopped chassis whose audio was last updated by the solver.
  for (const auto& [table, items] : sfx_items)
    for (const auto& item : items)
      if (item.voice) host_sfx_pause_loop(item.voice, paused);
  for (const auto &[part, rec] : records)
    for (const int32_t voice : {rec.traffic_voice[0], rec.traffic_voice[1], rec.traffic_voice[2],
                                rec.radio_voice, rec.slide_voice})
      if (voice) host_sfx_pause_loop(voice, paused);
}

namespace {
// The physics thread (#203). It runs one batch at a time; the host waits for
// it only in part_physics_end, part_physics_join and part_physics_begin.
class PhysicsWorker {
public:
  void start(SimBatch &batch) {
    std::lock_guard lock(mutex_);
    batch_ = &batch;
    done_ = false;
    error_ = {};
    work_.notify_one();
  }
  void wait() {
    std::exception_ptr error;
    {
      std::unique_lock lock(mutex_);
      finished_.wait(lock, [&] { return done_; });
      std::swap(error, error_);
    }
    if (error) std::rethrow_exception(error);
  }
  bool on_thread() const { return std::this_thread::get_id() == thread_.get_id(); }

private:
  void run(std::stop_token stop) {
    // Frame profiler zones belong to the host thread; the host adds this
    // thread's batch time itself.
    perf::worker_thread() = true;
    std::unique_lock lock(mutex_);
    while (true) {
      work_.wait(lock, stop, [&] { return batch_ != nullptr; });
      if (stop.stop_requested()) return;
      SimBatch *batch = batch_;
      lock.unlock();
      std::exception_ptr error;
      try {
        simulate_batch(*batch);
      } catch (...) {
        error = std::current_exception();
      }
      lock.lock();
      batch_ = nullptr;
      error_ = error;
      done_ = true;
      finished_.notify_all();
    }
  }
  std::mutex mutex_;
  std::condition_variable_any work_;
  std::condition_variable finished_;
  SimBatch *batch_ = nullptr;
  bool done_ = true;
  std::exception_ptr error_;
  std::jthread thread_{[this](std::stop_token stop) { run(stop); }};
};
PhysicsWorker &physics_worker() {
  static PhysicsWorker worker;
  return worker;
}

// The free-running physics thread (#380). It steps an open batch one solver
// step at a time on the wall clock (1 ms at 1000 Hz), independently of the
// frame rate, and catches up after a short stall. The host pauses it once per
// frame (part_physics_begin): the thread finishes its current step, the host
// commits the steps run so far, prepares the next batch from scripts and
// input, and resumes it. part_physics_join also pauses it, until that sync.
class PhysicsClock {
public:
  using Clock = std::chrono::steady_clock;
  // `scale` is game seconds per wall second: time warp, 0 while paused.
  void resume(SimBatch &batch, double scale) {
    std::lock_guard lock(mutex_);
    // Game time runs on while the host syncs: re-anchor it at the old scale,
    // so the steps owed for the pause are run on resume, not lost.
    const auto now = Clock::now();
    anchor_game_ = game_time(now);
    anchor_wall_ = now;
    scale_ = std::clamp(scale, 0.0, 8.0);
    batch_ = &batch;
    batch_base_ = total_steps_ - batch.steps_done;
    paused_ = false;
    work_.notify_all();
  }
  // Returns once no step is running; the batch is the host's again.
  void pause() {
    std::exception_ptr error;
    {
      std::unique_lock lock(mutex_);
      paused_ = true;
      work_.notify_all();
      idle_.wait(lock, [&] { return !stepping_; });
      batch_ = nullptr;
      std::swap(error, error_);
    }
    if (error) std::rethrow_exception(error);
  }
  bool on_thread() const { return std::this_thread::get_id() == thread_.get_id(); }
  // Busy time (profiler ticks) since the last take, once per sync.
  int64_t take_busy_ticks() { return busy_ticks_.exchange(0); }
  // Waits, at most `max_seconds`, until the thread has run the steps due now,
  // so a thread that falls behind slows the frame rate rather than the game
  // (the time beyond the 0.1 s cap is still dropped). Returns the wait.
  double catch_up(double max_seconds) {
    std::unique_lock lock(mutex_);
    if (!batch_ || paused_ || scale_ <= 0) return 0;
    const auto start = Clock::now();
    const uint64_t due = static_cast<uint64_t>(game_time(start) / physics::VehicleDynamics::timestep);
    if (total_steps_ + 1 >= due) return 0;
    idle_.wait_until(lock, start + std::chrono::duration_cast<Clock::duration>(
                                       std::chrono::duration<double>(max_seconds)),
                     [&] { return paused_ || error_ || total_steps_ + 1 >= due; });
    return std::chrono::duration<double>(Clock::now() - start).count();
  }
  // Game seconds dropped since the last take because the thread fell behind.
  double take_dropped_seconds() { return dropped_.exchange(0); }
  // Changes the solver rate while paused, keeping the game time already run.
  void set_rate(unsigned hz) {
    std::lock_guard lock(mutex_);
    const double done = total_steps_ * physics::VehicleDynamics::timestep;
    physics::VehicleDynamics::set_frequency(hz);
    total_steps_ = static_cast<uint64_t>(std::llround(done / physics::VehicleDynamics::timestep));
  }

private:
  void run(std::stop_token stop) {
    perf::worker_thread() = true;
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
      work_.wait(lock, stop, [&] { return batch_ != nullptr && !paused_; });
      if (stop.stop_requested()) return;
      const double step = physics::VehicleDynamics::timestep;
      const auto now = Clock::now();
      uint64_t due = static_cast<uint64_t>(game_time(now) / step);
      // After a stall, drop the time beyond a tenth of a second instead of
      // running a long burst the host would then wait for.
      const uint64_t cap = total_steps_ + physics::VehicleDynamics::frequency / 10;
      if (due > cap) {
        anchor_game_ -= (due - cap) * step;
        dropped_ = dropped_ + (due - cap) * step;
        due = cap;
      }
      if (total_steps_ < due) {
        stepping_ = true;
        SimBatch *batch = batch_;
        lock.unlock();
        std::exception_ptr error;
        const auto started = perf::now_ticks();
        try {
          simulate_step(*batch);
        } catch (...) {
          error = std::current_exception();
        }
        busy_ticks_ += perf::now_ticks() - started;
        lock.lock();
        ++total_steps_;
        stepping_ = false;
        if (error) {
          error_ = error;
          paused_ = true;
        }
        idle_.notify_all();
        continue;
      }
      if (scale_ <= 0) {
        work_.wait(lock, stop, [&] { return paused_ || batch_ == nullptr; });
        continue;
      }
      // The wall time at which game time reaches the next step.
      const double wait = ((total_steps_ + 1) * step - game_time(now)) / scale_;
      work_.wait_until(lock, stop, now + std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(std::max(0.0, wait))), [&] { return paused_; });
    }
  }
  std::mutex mutex_;
  std::condition_variable_any work_;
  std::condition_variable idle_;
  SimBatch *batch_ = nullptr;
  bool paused_ = true, stepping_ = false;
  double game_time(Clock::time_point now) const {
    if (anchor_wall_ == Clock::time_point{}) return 0;
    return anchor_game_ + std::chrono::duration<double>(now - anchor_wall_).count() * scale_;
  }
  double scale_ = 0;
  Clock::time_point anchor_wall_{};
  double anchor_game_ = 0;
  uint64_t total_steps_ = 0, batch_base_ = 0;
  std::exception_ptr error_;
  std::atomic<int64_t> busy_ticks_{0};
  std::atomic<double> dropped_{0};
  std::jthread thread_{[this](std::stop_token stop) { run(stop); }};
};
PhysicsClock &physics_clock() {
  static PhysicsClock clock;
  return clock;
}
// Free-running mode (#380): the game turns it on at boot; tests and the
// fixed-step benchmark keep the frame-locked batches.
bool physics_free_running = false;
bool physics_clock_running = false;
// Adaptive solver rate (#380). The free-running thread must keep real time:
// when it drops time it halves the rate (down to kMinAdaptiveHz), and it
// doubles back towards the requested rate once it has room. The Physics Lab
// gives the same handling from 250 to 1000 Hz.
constexpr unsigned kMinAdaptiveHz = 250;
unsigned physics_requested_hz = 0;  // 0: the rate when free-running starts
PartPhysicsClockStats clock_stats;
std::chrono::steady_clock::time_point physics_last_sync{};
bool physics_threaded = true;
SimBatch physics_batch;
bool physics_batch_running = false, physics_batch_pending = false;
double physics_frame_seconds = 0;
unsigned physics_frame_ticks = 0;
} // namespace

void part_physics_set_threaded(bool threaded) {
  part_physics_join();
  physics_threaded = threaded;
}

unsigned part_physics_steps_ahead(unsigned host_ticks) {
  unsigned steps = 0;
  for (unsigned t = 0; t < host_ticks; ++t)
    steps += physics::VehicleDynamics::steps_in_host_tick(host_tick_clock + t);
  return steps;
}

PartPhysicsClockStats part_physics_clock_stats() {
  auto stats = clock_stats;
  stats.hz = physics::VehicleDynamics::frequency;
  stats.free_running = physics_threaded && physics_free_running;
  return stats;
}

void part_physics_trace(const std::string& path) {
  part_physics_join();
  physics_trace = {};
  physics_trace.path = path;
}

void part_physics_set_free_running(bool free_running) {
  part_physics_join();
  physics_free_running = free_running;
  physics_requested_hz = physics::VehicleDynamics::frequency;
}

void part_physics_join() {
  if (physics_clock_running && !physics_clock().on_thread()) {
    // Stays paused until the next part_physics_begin resumes it.
    physics_clock_running = false;
    physics_clock().pause();
  }
  if (!physics_batch_running || physics_worker().on_thread()) return;
  physics_batch_running = false;
  physics_worker().wait();
}

namespace {
// The host's part after a batch ran: results into the records, then what the
// frame draws from them.
// Force feedback from the controlled car's front tyres, after each committed
// batch. A frame without one (pause, a modal, a script holding the car) keeps
// the torque for a moment, then lets go of the wheel.
physics::SteeringFeedback steering_feedback;
double feedback_idle = 0;
void update_force_feedback(double frame_seconds, bool committed) {
  Object* chassis = part_controlled_chassis();
  Record* rec = chassis ? record(chassis) : nullptr;
  const bool driving = rec && rec->config && rec->config->native_kind == "car" &&
                       rec->config->has_steering && rec->physics_released &&
                       !rec->state.parent && !osd_has_modal();
  if (!driving) {
    steering_feedback.reset();
    feedback_idle = 0;
    host_input_force_feedback_stop();
    return;
  }
  if (!committed) {
    feedback_idle += frame_seconds;
    if (feedback_idle > .25) host_input_force_feedback_stop();
    return;
  }
  feedback_idle = 0;
  const auto& dynamics = rec->dynamics;
  std::array<physics::SteeringFeedbackWheel, 2> front;
  for (size_t i = 0; i < front.size(); ++i) {
    const auto& s = dynamics.state[i];
    front[i] = {s.contact && !dynamics.wheels[i].metal_contact, s.force, s.slip_angle,
                s.tire_force.lateral, dynamics.drive.wheels[i].tyre, s.speed};
  }
  const auto out = steering_feedback.update(front, dynamics.body.velocity.length(),
                                            std::max(frame_seconds, 1e-3), feedback_jolt);
  feedback_jolt = 0;
  host_input_force_feedback(out.torque, out.damper, out.detail, out.impact, out.vibration,
                            out.vibration_hz, static_cast<float>(frame_seconds));
}

void finish_physics_frame(double frame_seconds, double alpha, bool committed) {
  if (committed) {
    if (perf::enabled()) perf::add(perf::Zone::Vehicles, physics_batch.thread_ticks);
    commit_batch(physics_batch);
    for (auto &[key, rec] : records) {
      if (rec.config->native_kind == "car") {
        update_engine_audio(rec, rec.engine_running);
        // Native traffic is excluded from tyre sounds and marks: with ~100
        // cars it would cost the frame, and it rarely slides.
        const bool tyres = !rec.native_vehicle && rec.physics_released && !rec.state.parent;
        update_slide_audio(rec, tyres);
        if (tyres) update_skid_marks(rec, frame_seconds);
      }
    }
    update_traffic_engine_audio();
  }
  update_force_feedback(frame_seconds, committed);
  // After the batch, so the hinges see this frame's carrier motion.
  advance_flaps(frame_seconds);
  publish_render_poses(alpha);
  // After the solver, so a flap or a wheel placement cannot put the column back.
  pose_steering_wheels();
  pose_cockpit_needles();
  pose_driver();
  {perf::Scope scope(perf::Zone::Pedestrians);tick_pedestrians(frame_seconds);}
}

// Free-running frame (#380): pause the clock, commit what it ran since the
// last sync, publish, then hand it the next open batch.
double last_sync_seconds = 0;  // wall time the last sync kept the thread paused
void free_running_sync(double seconds) {
  const auto sync_start = std::chrono::steady_clock::now();
  unsigned rate_change = 0;
  const double waited = physics_clock_running ? physics_clock().catch_up(.05) : 0.0;
  part_physics_join();
  const auto now = std::chrono::steady_clock::now();
  const double wall = physics_last_sync.time_since_epoch().count()
      ? std::chrono::duration<double>(now - physics_last_sync).count() : 0.0;
  physics_last_sync = now;
  const bool ran = physics_batch.open && physics_batch.steps_done > 0;
  physics_batch.thread_ticks = physics_clock().take_busy_ticks();
  {
    // Statistics over about one second for the telemetry overlay.
    static double window_wall = 0, window_dropped = 0, window_sync = 0, window_wait = 0;
    static uint64_t window_steps = 0;
    static int64_t window_busy = 0;
    static unsigned room_windows = 0;
    window_dropped += physics_clock().take_dropped_seconds();
    window_sync += last_sync_seconds;
    window_wait += waited;
    window_wall += wall;
    window_steps += physics_batch.steps_done;
    window_busy += physics_batch.thread_ticks;
    if (window_wall >= 1) {
      clock_stats.steps_per_second = window_steps / window_wall;
      clock_stats.thread_load = window_busy * 1e-9 / window_wall;  // profiler ticks are ns
      clock_stats.dropped = window_dropped / window_wall;
      clock_stats.host_wait = window_wait / window_wall;
      // Room for twice the rate: twice the load fits in 80 % of the time
      // the thread is not paused by the sync.
      const double room = std::max(0.0, 1 - window_sync / window_wall) * .8;
      const unsigned hz = physics::VehicleDynamics::frequency;
      // Behind: time dropped, or frames waiting on the thread for over a tenth
      // of the time.
      const bool behind = window_dropped > .02 * window_wall || window_wait > .1 * window_wall;
      if (behind && hz / 2 >= kMinAdaptiveHz) {
        rate_change = hz / 2;
        room_windows = 0;
      } else if (hz < physics_requested_hz && !behind &&
                 clock_stats.thread_load * 2 < room) {
        if (++room_windows >= 3) {
          rate_change = std::min(physics_requested_hz, hz * 2);
          room_windows = 0;
        }
      } else {
        room_windows = 0;
      }
      window_dropped = 0;
      window_sync = 0;
      window_wait = 0;
      window_wall = 0;
      window_steps = 0;
      window_busy = 0;
    }
  }
  {
    struct Clear {
      ~Clear() { physics_batch = {}; }
    } clear;
    finish_physics_frame(seconds, 1.0, ran);
  }
  if (!repair_relink_pending.empty()) {
    for (Object* part : repair_relink_pending)
      if (record(part)) relink_one(part);
    repair_relink_pending.clear();
  }
  if (rate_change && rate_change != physics::VehicleDynamics::frequency) {
    std::fprintf(stderr, "[phys] solver rate %u -> %u Hz (%s)%c",
                 physics::VehicleDynamics::frequency, rate_change,
                 rate_change < physics::VehicleDynamics::frequency ? "falling behind real time"
                                                                   : "room again", 10);
    physics_clock().set_rate(rate_change);
  }
  {perf::Scope scope(perf::Zone::Collision);scene_instances_prepare_physics();}
  // Contacts and props are prepared for about one and a half frames of travel.
  const double frame = wall > 0 ? std::min(wall, .1) : kPhysicsTick;
  const unsigned horizon = static_cast<unsigned>(
      std::ceil(frame * 1.5 * physics::VehicleDynamics::frequency));
  physics_batch = prepare_batch(0, true, horizon);
  physics_batch.vehicles.reserve(physics_batch.cars.size());
  // Game seconds per wall second: time warp, and 0 while the game is paused.
  const double scale = wall > 0 ? seconds / wall : (seconds > 0 ? 1.0 : 0.0);
  physics_clock_running = true;
  last_sync_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - sync_start).count();
  physics_clock().resume(physics_batch, scale);
}
}  // namespace

void part_physics_begin(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0)
    throw std::invalid_argument("invalid vehicle elapsed time");
  if (physics_threaded && physics_free_running) {
    free_running_sync(seconds);
    return;
  }
  // A frame that never reached part_physics_end still commits its batch first.
  if (physics_batch_pending) part_physics_end();
  if (!repair_relink_pending.empty()) {
    // One local relink per repaired part, after the complete stock repair walk.
    // The empty-set check is the only steady-state cost.
    for (Object* part : repair_relink_pending)
      if (record(part)) relink_one(part);
    repair_relink_pending.clear();
  }
  // Ignore excess time after a long stall so the next frame can still render.
  physics_elapsed += std::min(seconds, kPhysicsTick * kMaxPhysicsTicksPerFrame);
  constexpr double tolerance = kPhysicsTick * 1e-10;
  unsigned ticks = 0;
  while (physics_elapsed + tolerance >= kPhysicsTick) {
    ++ticks;
    physics_elapsed = std::max(0.0, physics_elapsed - kPhysicsTick);
  }
  physics_frame_seconds = seconds;
  physics_frame_ticks = ticks;
  physics_batch_pending = true;
  if (!ticks) return;
  {perf::Scope scope(perf::Zone::Collision);scene_instances_prepare_physics();}
  physics_batch = prepare_batch(ticks);
  if (physics_threaded) {
    physics_batch_running = true;
    physics_worker().start(physics_batch);
  } else {
    simulate_batch(physics_batch);
  }
}

void part_physics_end() {
  if (!physics_batch_pending) return;
  physics_batch_pending = false;
  if (physics_frame_ticks) {
    // Clear the batch even when the physics thread failed.
    struct Clear {
      ~Clear() { physics_batch = {}; }
    } clear;
    part_physics_join();
    finish_physics_frame(physics_frame_seconds, physics_elapsed / kPhysicsTick, true);
    return;
  }
  finish_physics_frame(physics_frame_seconds, physics_elapsed / kPhysicsTick, false);
}

void part_physics_advance(double seconds) {
  part_physics_begin(seconds);
  part_physics_end();
}

void part_set_pedestrian_density(void* ground, float density) {
  if (!ground || !std::isfinite(density)) return;
  density = std::max(0.f, density);
  // GroundRef.setPedestrianDensity already multiplied by the slider. Undo that
  // so a later move of the OptionsDialog control changes the live count.
  PedCrowd& crowd = crowds[ground];
  const float slider = host_game_options().pedestrian_density;
  crowd.map_density = slider > 1e-4f ? density / slider : density;
  crowd.density = crowd.map_density * std::max(0.f, slider);
}

void part_add_pedestrian_type(void* ground, void* type) {
  if (!ground || !type) return;
  PedCrowd& crowd = crowds[ground];
  if (std::find(crowd.types.begin(), crowd.types.end(), type) != crowd.types.end())
    return;
  crowd.types.push_back(type);
  crowd.kinds.emplace_back();
}

void part_remove_pedestrian_type(void* ground, void* type) {
  if (!ground) return;
  const auto crowd = crowds.find(ground);
  if (crowd == crowds.end()) return;
  const auto found = std::find(crowd->second.types.begin(), crowd->second.types.end(), type);
  if (found == crowd->second.types.end()) return;
  const auto index = static_cast<size_t>(found - crowd->second.types.begin());
  crowd->second.types.erase(found);
  if (index < crowd->second.kinds.size())
    crowd->second.kinds.erase(crowd->second.kinds.begin() + static_cast<std::ptrdiff_t>(index));
  clear_ped_agents(crowd->second);
}

float part_pedestrian_distance(void* ground, float x, float y, float z, int32_t type_id) {
  // Pedestrian_distance @ 0x00589BE0 (slrr-reverse): nearest live walker,
  // filtered by type id unless it is 0; -1 for an empty or unmatched list.
  if (!ground || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return -1.f;
  const auto crowd = crowds.find(ground);
  if (crowd == crowds.end()) return -1.f;
  float best = -1.f;
  for (const PedAgent& agent : crowd->second.agents) {
    if (!agent.line.ok || agent.kind < 0 ||
        static_cast<size_t>(agent.kind) >= crowd->second.types.size())
      continue;
    if (type_id != 0) {
      const auto* type = gameref_get(crowd->second.types[static_cast<size_t>(agent.kind)]);
      if (!type || type->type_id != type_id) continue;
    }
    const float dx = agent.line.x - x, dy = agent.line.y - y, dz = agent.line.z - z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (best < 0.f || d < best) best = d;
  }
  return best;
}

void part_drop_actors() {
  for (auto& entry : drivers) release_driver(entry.second);
  drivers.clear();
  for (auto& entry : crowds) clear_ped_agents(entry.second);
  crowds.clear();
  skid_marks_clear();
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

namespace {
// Top-down (x, z) box of drawn meshes in the car frame.
struct TopBox {
  float x0 = std::numeric_limits<float>::max(), x1 = -std::numeric_limits<float>::max();
  float z0 = std::numeric_limits<float>::max(), z1 = -std::numeric_limits<float>::max();
  bool valid() const { return x1 > x0 && z1 > z0; }
  bool any() const { return x1 >= x0 && z1 >= z0; }
};
void add_render_box(const PartMatrix &part_frame, Object *render, TopBox &box) {
  const auto *node = gameref_get(render);
  if (!node || node->empty || node->light_flags || !node->type_id) return;
  const auto mesh = scene_instances_geometry(static_cast<uint32_t>(node->type_id));
  if (!mesh || !mesh->info.ok) return;
  const auto m = part_matrix_multiply(part_frame, node_local(render));
  const float s = mesh->position_scale;
  const auto &lo = mesh->info.bbox_min, &hi = mesh->info.bbox_max;
  for (int corner = 0; corner < 8; ++corner) {
    const float p[3] = {(corner & 1 ? hi[0] : lo[0]) * s, (corner & 2 ? hi[1] : lo[1]) * s,
                        (corner & 4 ? hi[2] : lo[2]) * s};
    const float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    const float z = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
    box.x0 = std::min(box.x0, x);
    box.x1 = std::max(box.x1, x);
    box.z0 = std::min(box.z0, z);
    box.z1 = std::max(box.z1, z);
  }
}
// The gauge describes installed locations, not a panel's bent/open pose. Use
// closed CFG mounts plus intentional setSlotPos, excluding damage slot offsets.
PartMatrix hud_installed_frame(const Record& parent, int slot_id, Object* part) {
  const auto* child = record(part);
  if (!child) return node_local(part);
  const auto mount = std::find_if(parent.config->slots.begin(), parent.config->slots.end(),
      [&](const PartSlot& s) { return s.id == slot_id; });
  const auto mate = std::find_if(child->config->slots.begin(), child->config->slots.end(),
      [&](const PartSlot& s) { return s.id == child->state.own_slot; });
  if (mount == parent.config->slots.end() || mate == child->config->slots.end())
    return node_local(part);
  const auto scripted = [](const Record& rec, const PartSlot& source) {
    PartSlot slot = source;
    if (const auto it = rec.slot_script_pose.find(slot.id); it != rec.slot_script_pose.end()) {
      if (it->second.pos)
        std::copy(it->second.p, it->second.p + 3, slot.pose.position.begin());
      if (it->second.ypr)
        std::copy(it->second.y, it->second.y + 3, slot.pose.rotation.begin());
    }
    return slot;
  };
  PartMatrix a, b;
  if (!part_slot_matrix(*parent.config, scripted(parent, *mount), a) ||
      !part_slot_matrix(*child->config, scripted(*child, *mate), b)) return node_local(part);
  return part_matrix_multiply(a, part_matrix_inverse_rigid(b));
}
// Engine, body and wheel parts in the rest chassis frame. Prefer chassis-only
// bounds so removing a panel cannot rescale/reassign the surviving indicators.
// Wheels keep their corner from the chassis slot (type 101 + corner).
void hud_damage_parts(const Record &rec, VehicleHudState &state) {
  state.part_count = 0;
  state.chassis_condition = std::clamp(rec.state.wear * rec.state.tear, 0.f, 1.f);
  struct Placed {
    VehicleHudPart part;
    float x, z;
  };
  std::array<Placed, std::tuple_size_v<decltype(state.parts)>> placed;
  size_t count = 0;
  TopBox car;
  for (Object *render : rec.renders) add_render_box(eye_matrix(), render, car);
  const bool chassis_bounds = car.valid();
  const auto visit = [&](auto &&visit, Object *part, const PartMatrix &frame,
                         int corner, int depth) -> void {
    const auto *child = record(part);
    if (!child || depth > 16) return;
    // Running gear and interior (category 3 and others) have no blip.
    const int category = child->config->category;
    const bool shown = corner >= 0 || category == 1 || category == 2;
    const auto kind = corner >= 0 ? VehicleHudPart::Kind::Wheel
        : category == 1 ? VehicleHudPart::Kind::Engine : VehicleHudPart::Kind::Body;
    TopBox box;
    for (Object *render : child->renders) add_render_box(frame, render, box);
    if (!chassis_bounds && category == 2 && corner < 0 && box.any()) {
      car.x0 = std::min(car.x0, box.x0);
      car.x1 = std::max(car.x1, box.x1);
      car.z0 = std::min(car.z0, box.z0);
      car.z1 = std::max(car.z1, box.z1);
    }
    if (shown && box.any() && count < placed.size()) {
      auto &entry = placed[count++];
      entry.part.kind = kind;
      entry.part.quarterpanel = kind == VehicleHudPart::Kind::Body &&
          runtime->tree_instance_of(part, "java.game.parts.bodypart.Quarterpanel");
      entry.part.corner = static_cast<int8_t>(corner);
      entry.part.condition = std::clamp(child->state.wear * child->state.tear, 0.f, 1.f);
      entry.x = .5f * (box.x0 + box.x1);
      entry.z = .5f * (box.z0 + box.z1);
    }
    for (const auto &[slot, grandchild] : child->children)
      visit(visit, grandchild,
            part_matrix_multiply(frame, hud_installed_frame(*child, slot, grandchild)),
            corner, depth + 1);
  };
  for (const auto &[slot_id, child] : rec.children) {
    int corner = -1;
    for (const auto &slot : rec.config->slots)
      if (slot.id == slot_id && slot.type >= 101 && slot.type <= 104) corner = slot.type - 101;
    visit(visit, child, hud_installed_frame(rec, slot_id, child), corner, 0);
  }
  if (!car.valid()) {
    // No chassis mesh or panel: fall back to the extent of the parts.
    for (size_t i = 0; i < count; ++i) {
      car.x0 = std::min(car.x0, placed[i].x);
      car.x1 = std::max(car.x1, placed[i].x);
      car.z0 = std::min(car.z0, placed[i].z);
      car.z1 = std::max(car.z1, placed[i].z);
    }
    if (!car.valid()) return;
  }
  for (size_t i = 0; i < count; ++i) {
    auto part = placed[i].part;
    part.x = 2.f * (placed[i].x - car.x0) / (car.x1 - car.x0) - 1.f;
    part.z = 2.f * (placed[i].z - car.z0) / (car.z1 - car.z0) - 1.f;
    state.parts[state.part_count++] = part;
  }
}
}  // namespace

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
  state.rpm = gauge_rpm(*rec);
  state.speed = gauge_speed(*rec);
  state.mileage = rec->mileage * .01;
  state.unit_scale = 1000.f / host_game_options().unit_meters;
  const double capacity = nitro_capacity(rec->state.object);
  const float nitro_rate = comp(rec->state.object, "consumption_nitro");
  state.nitro = capacity > 0 && std::isfinite(nitro_rate) && nitro_rate > 0
      ? static_cast<float>(std::clamp(rec->nitro_remaining / capacity, 0., 1.)) : 0.f;
  hud_damage_parts(*rec, state);
  for (const auto &[slot, child] : rec->children) {
    if (rec->engine_running)
      if (Object *dyno = part_field(child, "dynodata"))
        state.turbo = dyno_boost_fraction(dyno, static_cast<float>(state.rpm)) *
                      static_cast<float>(rec->dynamics.drive.throttle);
  }
  state.gear = rec->physics_released ? rec->dynamics.gear : 0;
  // The lamp lights past 5 % of the travel, not on an axis resting near zero.
  state.handbrake = rec->dynamics.drive.handbrake > kHandbrakeFull * .05;
  return true;
}

void part_animate_external_wheels(Object *car, float metres, float steer) {
  auto *rec = record(as_part(car));
  if (!rec || !rec->external_motion || !std::isfinite(metres) ||
      !std::isfinite(steer))
    return;
  for (auto &wheel : rec->inline_wheels) {
    wheel.spin =
        std::remainder(wheel.spin - metres / wheel.radius, 6.283185307179586);
    PartPose rotation;
    rotation.rotation = {std::clamp(steer, -.6f, .6f) * wheel.steer_factor,
                         float(wheel.spin), 0};
    auto local = part_matrix_multiply(
        wheel.base,
        part_matrix_multiply(part_pose_matrix(rotation), wheel.mesh));
    if (auto *n = gameref_get(wheel.node)) {
      std::copy(local.begin(), local.end(), n->local_matrix);
      n->matrix_override = true;
    }
  }
}
void part_set_external_motion(Object *car, bool external) {
  auto *rec = record(as_part(car));
  if (rec) {
    rec->external_motion = external;
    rec->physics_ready = false;
  }
}
bool part_published_local(Object *car, float *local16) {
  auto *rec = record(as_part(car));
  auto *node = gameref_get(car);
  if (!rec || !node || rec->external_motion || !rec->physics_ready ||
      rec->physics_suspended || !node->matrix_override)
    return false;
  std::copy_n(node->local_matrix, 16, local16);
  return true;
}
bool part_set_external_pose(Object *car, const float *local16, double dt) {
  auto *rec = record(as_part(car));
  auto *node = gameref_get(car);
  if (!rec || !node || !rec->external_motion)
    return false;
  for (int i = 0; i < 16; ++i)
    if (!std::isfinite(local16[i]))
      return false;
  const float previous[3] = {node->pos[0], node->pos[1], node->pos[2]};
  const float previous_yaw = node->ypr[0];
  std::copy_n(local16, 16, node->local_matrix);
  node->matrix_override = true;
  std::copy_n(local16 + 12, 3, node->pos);
  // Same Euler extraction as dynamics_sync, so readers of pos/ypr agree.
  const double pitch = std::asin(std::clamp(-double(local16[9]), -1.0, 1.0));
  node->ypr[1] = static_cast<float>(pitch);
  if (std::fabs(std::cos(pitch)) > 1e-6) {
    node->ypr[0] = static_cast<float>(std::atan2(local16[8], local16[10]));
    node->ypr[2] = static_cast<float>(std::atan2(local16[1], local16[5]));
  } else {
    node->ypr[0] = static_cast<float>(std::atan2(-local16[2], local16[0]));
    node->ypr[2] = 0;
  }
  if (dt > 0) {
    const float d[3] = {node->pos[0] - previous[0], node->pos[1] - previous[1],
                        node->pos[2] - previous[2]};
    for (int i = 0; i < 3; ++i)
      node->vel[i] = static_cast<float>(d[i] / dt);
    // Signed travel along the car's forward (z) axis turns the wheels; the
    // yaw change per metre gives a plausible steering angle, as for
    // transform-driven traffic.
    const float advance = d[0] * local16[8] + d[1] * local16[9] + d[2] * local16[10];
    const float turn = std::remainder(node->ypr[0] - previous_yaw, 6.2831853f);
    part_animate_external_wheels(
        car, advance,
        std::fabs(advance) > .001f ? std::atan(2.5f * turn / advance) : 0.f);
  }
  return true;
}
bool part_physics_state(Object *key, PartPhysicsState &state) {
  auto *rec = record(as_part(key));
  if (!rec)
    return false;
  PartMatrix world;
  state = {};
  state.nitro_remaining = rec->nitro_remaining;
  state.nitro_capacity = nitro_capacity(rec->state.object);
  state.nitro_active = rec->nitro_active;
  state.nitro_flame = rec->nitro_flame;
  for (size_t axle = 0; axle < rec->dynamics.anti_roll.size(); ++axle) {
    state.anti_roll_stiffness[axle] = rec->dynamics.anti_roll[axle].stiffness;
    state.anti_roll_damping[axle] = rec->dynamics.anti_roll[axle].damping;
  }
  state.active = rec->physics_ready && !rec->physics_suspended &&
                 scene_instances_script_matrix(as_part(key), world.data());
  if (!rec->physics_ready)
    return true;
  state.mass = rec->dynamics.body.mass;
  state.height = rec->dynamics.body.frame()[13];
  state.vertical_speed = rec->dynamics.body.velocity.y;
  state.ticks = rec->dynamics.ticks;
  state.backlog = rec->dynamics.backlog();
  state.speed_kph = rec->dynamics.body.velocity.length() * 3.6;
  state.rpm = rec->dynamics.engine_omega * 9.54929658551372;
  state.yaw_rate = rec->dynamics.body.omega().y;
  state.steering_help = rec->steering_help;
  state.steering_request = rec->steering_request * std::max(0.f, rec->config->max_steer);
  state.steering_correction = rec->steering_correction;
  state.steering_angle = chassis_steering_angle(*rec);
  state.gear = rec->dynamics.gear;
  state.throttle = rec->dynamics.drive.throttle;
  state.brake = rec->dynamics.drive.brake;
  state.clutch = rec->dynamics.drive.clutch;
  state.abs_active = rec->dynamics.abs_active();
  state.asr_active = rec->dynamics.asr_active();
  state.reference_speed = rec->dynamics.reference_speed();
  for (size_t i = 0; i < 4; ++i) {
    state.brake_temperature[i] = rec->dynamics.state[i].brake_temperature;
    state.brake_pressure[i] = rec->dynamics.state[i].brake_pressure;
    state.length[i] = rec->dynamics.state[i].length;
    state.force[i] = rec->dynamics.state[i].force;
    state.contact[i] = rec->dynamics.state[i].contact;
    state.geometry_solved[i] = rec->geometry_solved[i];
    const auto &center = rec->dynamics.wheels[i].center;
    state.wheel_center[i] = {center.x, center.y, center.z};
    const auto& wheel = rec->dynamics.state[i];
    state.tires[i] = {rec->dynamics.drive.wheels[i].steer,
                     wheel.slip_speed, wheel.lateral_speed, wheel.tire_force.longitudinal,
                     wheel.tire_force.lateral, wheel.spin * rec->dynamics.wheels[i].radius,
                     rec->dynamics.drive.wheels[i].weight};
  }
  return true;
}

Object* part_controlled_chassis() {
  const auto* target = gameref_find_live(controlled_vehicle);
  return target ? as_part(static_cast<Object*>(target->key)) : nullptr;
}

std::vector<PartVehiclePosition> part_vehicle_positions() {
  std::vector<PartVehiclePosition> out;
  for (auto& [object, rec] : records) {
    if (rec.config->native_kind != "car" || rec.state.parent) continue;
    PartMatrix frame;
    if (!scene_instances_script_matrix(object, frame.data())) continue;
    const auto* state = gameref_get(object);
    if (!state || state->streamed_hidden) continue;
    PartVehiclePosition vehicle;
    vehicle.chassis = object;
    vehicle.id = state->live_id ? state->live_id : state->type_id;
    const auto& body = rec.dynamics.body;
    if (rec.physics_ready) {
      vehicle.position[0] = static_cast<float>(body.position.x);
      vehicle.position[1] = static_cast<float>(body.position.y);
      vehicle.position[2] = static_cast<float>(body.position.z);
    } else {
      std::copy(frame.begin() + 12, frame.begin() + 15, vehicle.position);
    }
    out.push_back(vehicle);
  }
  return out;
}

Object* part_chassis(Object* object) { return as_part(object); }

Object* part_debug_chassis(Object* object) {
  return car(as_part(object));
}

void *part_shadow_source(void *render, float *frame) {
  for (int guard = 0; render && guard < 64; ++guard) {
    if (auto *rec = record(static_cast<Object *>(render));
        rec && rec->config->native_kind == "car")
      return scene_instances_render_matrix(render, frame) ? render : nullptr;
    auto *node = gameref_get(render);
    render = node ? node->parent : nullptr;
  }
  return nullptr;
}
bool part_camera_pose(void* car, int camera, float* matrix) {
  auto* chassis = as_part(static_cast<Object*>(car));
  const auto* rec = record(chassis);
  PartMatrix frame;
  // Follow the drawn car, so the view does not step against it.
  if (!rec || !scene_instances_render_matrix(chassis, frame.data())) return false;
  if (camera >= 0) {
    if (static_cast<size_t>(camera) >= rec->config->cameras.size()) return false;
    const PartMatrix body = frame;
    frame = part_matrix_multiply(frame, part_pose_matrix(rec->config->cameras[camera]));
    const auto* controlled = gameref_find_live(controlled_vehicle);
    if (controlled && rec->physics_released &&
        as_part(static_cast<Object*>(controlled->key)) == chassis) {
      const auto& lean = rec->cockpit_offset;
      for (int i = 0; i < 3; ++i)
        frame[12 + i] += static_cast<float>(body[i] * lean.x + body[4 + i] * lean.y +
                                            body[8 + i] * lean.z);
    }
  }
  std::copy(frame.begin(), frame.end(), matrix);
  return true;
}
bool part_car_velocity(void* car, float* velocity) {
  const auto* rec = record(as_part(static_cast<Object*>(car)));
  if (!rec || !velocity) return false;
  const auto& v = rec->dynamics.body.velocity;
  velocity[0] = static_cast<float>(v.x);
  velocity[1] = static_cast<float>(v.y);
  velocity[2] = static_cast<float>(v.z);
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
  // A new world drops the old one's batch unpublished.
  part_physics_join();
  physics_batch = {};
  physics_batch_pending = false;
  runtime = &jvm;
  for (auto &[key, rec] : records) release_side(rec);
  records.clear();
  paint_texture_clear_all();
  repair_relink_pending.clear();
  // Dyno sessions belong to the old world's Java objects. The registry is
  // deliberately weak during GC, so world teardown must release it explicitly.
  dyno_clear_sessions();
  loose_panels.clear();
  settled_wheel_releases.clear();
  free_bodies.clear();
  invalid_dynamic_types.clear();
  controlled_vehicle = 0;
  hud_preview = {};
  hud_preview_directory.clear();
  physics_elapsed = 0;
  sfx_items.clear();
  configurations.clear();
  part_masses.clear();
  slot_aliases.clear();
  index = &resources;
  jvm.natives().register_method("java.game.Vehicle", "getSpeedSquare", "()F",
    [](Object *self, const std::vector<Value> &) {
      const auto *rec = record(as_part(self));
      return Value::make_float(rec && rec->physics_ready
          ? static_cast<float>(rec->dynamics.body.velocity.dot(rec->dynamics.body.velocity))
          : 0.f);
    });
  jvm.natives().register_method("java.game.Vehicle", "getHorn", "()I",
    [](Object* self, const std::vector<Value>&) {
      const auto* rec = record(as_part(self));
      if (!rec) return Value::make_int(0);
      const int slot = rec->horn_down ? 1 : rec->horn_slot;
      const auto horn = rec->horns.find(slot);
      return Value::make_int(horn != rec->horns.end() && horn->second.sfx != 0);
    });
  jvm.natives().register_method("java.game.Vehicle", "hasCrime", "()F",
    [](Object* self, const std::vector<Value>&) {
      // PE 0x440BF0 returns the active zone speed limit in m/s, or -1.
      // Zone-event producers are not implemented yet; never invent a limit.
      Object* chassis = as_part(self);
      const auto* node = gameref_get(chassis);
      if (!node || node->empty) return Value::make_float(-1.f);
      const auto found = chassis->fields.find("crime_speed");
      if (found == chassis->fields.end()) return Value::make_float(-1.f);
      const float limit = found->second.tag == ValueTag::Float ? found->second.v.f :
          found->second.tag == ValueTag::Int ? float(found->second.v.i) : -1.f;
      return Value::make_float(std::isfinite(limit) && limit >= 0.f ? limit : -1.f);
    });
  const std::string owner = "java.game.parts.Part";
  auto bind = [&](const char *name, const char *signature, NativeFn fn) {
    jvm.natives().register_method(owner, name, signature, std::move(fn));
  };
  auto info = [](Object *self, const std::vector<Value> &args) {
    auto *state = gameref_get(self);
    auto *p = record(self);
    const int query = number(args, 0);
    if (query == 56) return Value::make_int(race_traffic_pointer(self));
    if (query == 7 && race_traffic_pointer(self)) return Value::make_int(5);
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
    // GII_CATEGORY: installed and loose parts, the chassis included, are
    // GIR_CAT_PART (9). Painter, CarMarket and the garage drop test it on
    // the picked object. GIR_CAT_VEHICLE (5) is not reported yet.
    if (query == 7)
      return Value::make_int(p ? 9 : 0);
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
  // Chassis.getMass is its own native (empty TREE). The exe (0x0043CAA0)
  // returns the assembled vehicle body's mass, not the chassis part's: it is
  // what CarInfo (kg/HP), the Garage and the catalog delivery price
  // (chassis.getMass()*5.8) read. Same sum the physics body is built from.
  jvm.natives().register_method(
      "java.game.parts.bodypart.Chassis", "getMass", "()F",
      [](Object *s, const std::vector<Value> &) {
        if (!record(s))
          return Value::make_float(0.f);
        float mass = 0, com[3];
        assembly_mass_center(s, mass, com);
        return Value::make_float(mass);
      });
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
    // A Painter texture (and one a save or skin file brought back) is a
    // texture too, though no pack holds it.
    const RpkEntry *entry = id && index ? index->resolve(id) : nullptr;
    if (!paint_texture_exists(id) && (!entry || entry->typeof_ != 7))
      return Value::make_int(old);
    assign_texture(canonical(s), *p, id);
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
         if (!p || i < 0 || static_cast<size_t>(i) >= p->config->slots.size())
           return Value::make_obj(nullptr);
         const auto damage = p->slot_damage.find(i);
         // Part.save emits a record only for a non-null damage string or an
         // attachment. Undamaged slots must not masquerade as empty strings.
         return Value::make_obj(damage == p->slot_damage.end()
             ? nullptr : jvm.new_string(damage->second));
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
          invalidate_body_support(canonical(s));
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
         if (!pos && !ori) {
           p->slot_pose.erase(slot);
           p->slot_script_pose.erase(slot);
         } else {
           auto &ov = p->slot_pose[slot];
           auto &scripted = p->slot_script_pose[slot];
           if (pos) {
             ov.p[0] = comp(pos, "x");
             ov.p[1] = comp(pos, "y");
             ov.p[2] = comp(pos, "z");
             std::copy(ov.p, ov.p + 3, scripted.p);
             ov.pos = scripted.pos = true;
           }
           if (ori) {
             ov.y[0] = comp(ori, "y");
             ov.y[1] = comp(ori, "p");
             ov.y[2] = comp(ori, "r");
             std::copy(ov.y, ov.y + 3, scripted.y);
             ov.ypr = scripted.ypr = true;
           }
         }
         if (p->state.parent && slot == p->state.own_slot)
           place_child(p->state.parent, p->state.parent_slot);
         else
           place_child(canonical(s), slot);
          invalidate_body_support(canonical(s));
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
  // Part.getFlap() is flap(0), toggleFlap() is flap(1). 1 means open: opened
  // by a script or swung open by a hit (advance_flaps).
  bind("flap", "(I)I", [](Object *s, const std::vector<Value> &a) {
    auto *p = record(s);
    if (!p)
      return Value::make_int(-1);
    flap_geometry_for(canonical(s), *p);
    if (number(a, 0) == 1)
      physics::flap_toggle(p->flap);
    return Value::make_int(p->flap.open ? 1 : 0);
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
    const bool setter = std::string_view(name).starts_with("set");
    jvm.natives().register_method("java.game.parts.WheelRef", name, signature,
      [setter,fn=std::move(fn)](Object* self,const std::vector<Value>& args) {
        if (setter) for (const auto& arg:args) {
          if (arg.tag==ValueTag::Float && !std::isfinite(arg.v.f)) return Value::make_void();
          if (arg.tag==ValueTag::Obj && arg.v.o)
            for (const char* field:{"x","y","z","p","r"}) {
              const auto found=arg.v.o->fields.find(field);
              if(found!=arg.v.o->fields.end() && found->second.tag==ValueTag::Float &&
                 !std::isfinite(found->second.v.f)) return Value::make_void();
            }
        }
        return fn(self,args);
      });
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
  // OpenSLRR native declared by the patched WheelRef: the lateral relaxation
  // length Tyre.SetInflation derives from the pressure.
  wheel("setRelaxation", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.relaxation = scalar(a, 0, .3f); });
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
            for (int i = 0; i < 6; ++i) {
              w.ic_offset[i] = scalar(a, static_cast<size_t>(i), 0);
              w.ic[i] = w.ic_offset[i] + w.pos[i % 3];
            }
          });
        });
  wheel("setBrake", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.brake = scalar(a, 0, 0); });
  });
  wheel("setHBrake", "(F)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) { w.hbrake = scalar(a, 0, 0); });
  });
  // OpenSLRR native declared by the patched WheelRef (#446): the pad
  // compound (0 street, 1 fast road, 2 race), rotor type (0 solid,
  // 1 grooved, 2 vented, 3 carbon) and rotor diameter in metres.
  wheel("setBrakeSpec", "(FFF)V", [put](Object *s, const std::vector<Value> &a) {
    return put(s, [&](WheelPhys &w) {
      w.brake_pad = scalar(a, 0, 0);
      w.brake_rotor = scalar(a, 1, 0);
      w.brake_diameter = std::max(0.f, scalar(a, 2, 0));
    });
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
    for (const auto& needle : r.needles) visit(needle.node);
    for (const auto& link : r.links) visit(link.node);
    for (auto* sfx : r.sfx) visit(sfx);
  }
  for (const auto& entry : sfx_items) visit(entry.first);
  for (const auto& entry : drivers) visit(entry.second.render);
  for (const auto& entry : crowds) {
    visit(static_cast<Object*>(entry.first));
    for (void* type : entry.second.types) visit(static_cast<Object*>(type));
    for (const auto& agent : entry.second.agents) visit(agent.render);
  }
}
} // namespace slrr
