// Java traffic lifecycle adapted from SLRR_rev v221 cbb99147 traffic-native.ts.
// Streaming/controller policy is a host model, not original native pointer/AI
// parity.
#include "slrr/gameref.hpp"
#include "slrr/host_game_options.hpp"
#include "slrr/jvm.hpp"
#include "slrr/object.hpp"
#include "slrr/part_config.hpp"
#include "slrr/part_runtime.hpp"
#include "slrr/race_runtime.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/scene_instances.hpp"
#include "slrr/section_machine.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
namespace slrr {
namespace {
struct Model {
  uint32_t type = 0;
  float weight = 0;
  float begin = 0, end = 0, wheelbase = 0;
};
struct Registration {
  int32_t id = 0;
  void *ground = nullptr;
  uint64_t ground_incarnation = 0;
  Object *car = nullptr;
  uint64_t car_incarnation = 0;
  Object *target = nullptr;
  uint64_t target_incarnation = 0;
  bool owned = false, visible = false, notify = false, notified = false,
       unplaced = true, kinematic = false;
  int mode = 0;
  double distant_next = 0, stop_since = -1;
  uint32_t cleared_crossing = 0;
  size_t edge = 0;
  float along = 0, speed = 0, cruise = 0;
};
struct Seed {
  Model model;
  RoadPoint position{};
  int remaining = 0;
};
struct Population {
  uint64_t incarnation = 0;
  std::vector<Model> models;
  std::vector<Seed> seeds;
  std::unordered_set<uint32_t> blocked;
  std::unordered_map<uint32_t, double> crossings;
  // Signal-clock time of the first populated tick. The first fill happens
  // behind the loading screen and may place cars in view.
  double started = -1;
};
struct Event {
  Object *target;
  uint64_t incarnation;
  Object *ground;
  uint64_t ground_incarnation;
  std::string text;
};
std::unordered_map<void *, Population> populations;
std::unordered_map<int32_t, Registration> registrations;
std::vector<Event> events;
SectionMachine *sections = nullptr;
RpkIndex *resources = nullptr;
int32_t next_id = 1;
uint32_t random_state = 0x8f12345;
double clock_seconds = 0;
uint64_t signal_revision = 1;
float random() {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return float(random_state & 0xffffff) / 16777216.f;
}
float number(const std::vector<Value> &args, size_t i, float fallback = 0) {
  if (i >= args.size())
    return fallback;
  return args[i].tag == ValueTag::Float ? args[i].v.f
         : args[i].tag == ValueTag::Int ? float(args[i].v.i)
                                        : fallback;
}
Object *object(const std::vector<Value> &args, size_t i) {
  return i < args.size() && args[i].tag == ValueTag::Obj ? args[i].v.o
                                                         : nullptr;
}
std::optional<RoadPoint> point(Object *o) {
  if (!o)
    return {};
  RoadPoint p;
  for (int i = 0; i < 3; ++i) {
    auto it = o->fields.find(i == 0 ? "x" : i == 1 ? "y" : "z");
    if (it == o->fields.end())
      return {};
    p[i] = it->second.tag == ValueTag::Float ? it->second.v.f
           : it->second.tag == ValueTag::Int
               ? float(it->second.v.i)
               : std::numeric_limits<float>::quiet_NaN();
    if (!std::isfinite(p[i]))
      return {};
  }
  return p;
}
void *canonical(void *key) {
  auto *n = gameref_get(key);
  return n ? n->key : nullptr;
}
uint32_t model_type(Object *object) {
  if (!object)
    return 0;
  if (auto *n = gameref_get(object); n && n->type_id)
    return uint32_t(n->type_id);
  auto it = object->fields.find("id");
  return it != object->fields.end() && it->second.tag == ValueTag::Int
             ? uint32_t(it->second.v.i)
             : 0;
}
Population *population(void *key) {
  auto *n = gameref_get(key);
  if (!n)
    return nullptr;
  auto &p = populations[n->key];
  if (p.incarnation != n->incarnation) {
    p = {};
    p.incarnation = n->incarnation;
    ++signal_revision;
  }
  return &p;
}
RoadPoint position(Object *car) {
  PartMatrix frame;
  if (!scene_instances_script_matrix(car, frame.data()))
    return {};
  return {frame[12], frame[13], frame[14]};
}
// The world camera, sampled once per traffic tick.
struct View {
  bool valid = false;
  float eye[3]{}, forward[2]{}, tan_half = 0, limit = 0;
} view;
void update_view() {
  float at[3], fov = 60, zfar = 3000;
  view.valid = gameref_world_camera(view.eye, at, &fov, nullptr, &zfar);
  if (!view.valid)
    return;
  const float fx = at[0] - view.eye[0], fz = at[2] - view.eye[2],
              fl = std::hypot(fx, fz);
  view.forward[0] = fl > 1e-4f ? fx / fl : 0;
  view.forward[1] = fl > 1e-4f ? fz / fl : 0;
  // Wide enough for a 2:1 window.
  view.tan_half =
      std::tan(std::clamp(fov, 10.f, 120.f) * 3.1415927f / 360) * 2;
  view.limit = std::min(zfar, host_game_options().video.view_distance);
}
// The camera sees a point within the view distance and cone, with a
// car-length margin. Buildings are not tested, so a hidden street can still
// count as seen.
bool in_view(const RoadPoint &p) {
  if (!view.valid)
    return false;
  const float dx = p[0] - view.eye[0], dz = p[2] - view.eye[2],
              d = std::hypot(dx, dz);
  if (d > view.limit)
    return false;
  if (d < 8 || (view.forward[0] == 0 && view.forward[1] == 0))
    return true;
  const float half = std::atan(view.tan_half) + std::atan(6 / d);
  return (view.forward[0] * dx + view.forward[1] * dz) / d >
         std::cos(std::min(half, 3.1415927f));
}
bool belongs(void *key, void *world) {
  for (int i = 0; key && i < 64; ++i) {
    auto *n = gameref_get(key);
    if (!n)
      return false;
    if (n->key == world)
      return true;
    key = n->parent;
  }
  return false;
}
void queue(Registration &r, bool add) {
  if (add == r.notified)
    return;
  if (add && !r.notify)
    return;
  auto *car = gameref_get(r.car);
  if (!car)
    return;
  if (!r.target && sections) {
    r.target = sections->actual();
    auto *target = gameref_get(r.target);
    r.target_incarnation = target ? target->incarnation : 0;
  }
  if (r.target)
    events.push_back({r.target, r.target_incarnation,
                      static_cast<Object *>(r.ground), r.ground_incarnation,
                      (add ? "car_add " : "car_rem ") +
                          std::to_string(gameref_make_live(r.car)) +
                          (add ? " " + std::to_string(r.id) : "")});
  r.notified = add;
}
// remTrafficCar keeps the car: Bot.followCar/startRace detach a streamed car
// from traffic before driving it, and Java then owns its destruction.
void remove(int32_t id, bool destroy_car = true) {
  auto found = registrations.find(id);
  if (found == registrations.end())
    return;
  auto r = found->second;
  queue(r, false);
  registrations.erase(found);
  if (auto *n = gameref_get(r.car); n && n->incarnation == r.car_incarnation) {
    n->streamed_hidden = false;
    if (r.kinematic)
      part_set_external_motion(r.car, false);
    if (r.owned && destroy_car)
      part_destroy(r.car);
  }
}
bool blocked(const Population &p, const RoadRoutes::Edge &e) {
  for (auto id : e.roads)
    if (p.blocked.contains(id))
      return true;
  auto cross = p.crossings.find(e.to);
  return cross != p.crossings.end() && cross->second > clock_seconds;
}
size_t choose_transition(const std::vector<size_t> &choices,
                         const RoadRoutes &graph) {
  // Upstream chooses a destination branch first, then one of its lanes. A wide
  // outgoing street must not be more likely simply because it has more lanes.
  std::vector<uint32_t> branches;
  for (auto i : choices) {
    auto branch = graph.traffic_edges()[i].branch;
    if (std::find(branches.begin(), branches.end(), branch) == branches.end())
      branches.push_back(branch);
  }
  auto branch = branches[std::min<size_t>(size_t(random() * branches.size()),
                                          branches.size() - 1)];
  std::vector<size_t> lanes;
  for (auto i : choices)
    if (graph.traffic_edges()[i].branch == branch)
      lanes.push_back(i);
  return lanes[std::min<size_t>(size_t(random() * lanes.size()),
                                lanes.size() - 1)];
}
void publish(Registration &r, const RoadRoutes &graph, float at) {
  const auto &e = graph.traffic_edges()[r.edge];
  RoadRoute route;
  route.points = e.points;
  auto p = road_at(route, at);
  auto q = road_at(route, std::min(e.length, at + .25f));
  auto previous = road_at(route, std::max(0.f, at - .25f));
  auto direction = road_normalize(
      {q[0] - previous[0], q[1] - previous[1], q[2] - previous[2]});
  SceneFloorHit floor;
  if (scene_instances_sample_floor(p[0], p[2], p[1] + 4, r.car, floor))
    p[1] = floor.height;
  gameref_set_matrix(r.car, p[0], p[1], p[2],
                     std::atan2(-direction[0], -direction[2]), 0, 0);
  // setMatrix input is a contact point. Park computes the loaded wheel
  // contact origin; a body-mesh bounding box is not a tyre contact plane.
  part_park_chassis(r.car);
  if (auto *n = gameref_get(r.car))
    for (int i = 0; i < 3; ++i)
      n->vel[i] = direction[i] * r.speed;
}
bool place(Registration &r, const RoadRoutes &graph, const Population &p,
           const RoadPoint &focus, bool initial, bool unseen = false) {
  for (int attempt = 0; attempt < 96; ++attempt) {
    size_t edge =
        std::min<size_t>(size_t(random() * graph.traffic_edges().size()),
                         graph.traffic_edges().size() - 1);
    const auto &e = graph.traffic_edges()[edge];
    if (blocked(p, e) || e.crossing || e.next.empty() || e.length < 15)
      continue;
    float at = std::clamp(random() * e.length, 5.f, e.length - 5);
    RoadRoute line;
    line.points = e.points;
    auto candidate = road_at(line, at);
    float distance = road_distance(candidate, focus);
    if (initial && (distance < 45 || distance > 160))
      continue;
    // A car must not appear where the player is looking.
    if (unseen && in_view(candidate))
      continue;
    if (initial && distance < 80) {
      PartMatrix frame;
      if (auto *player = part_controlled_chassis();
          player && scene_instances_script_matrix(player, frame.data())) {
        float ahead = -(candidate[0] - focus[0]) * frame[8] -
                      (candidate[2] - focus[2]) * frame[10];
        if (ahead > distance * .25f)
          continue;
      }
    }
    bool occupied = false;
    for (const auto &entry : registrations)
      if (entry.first != r.id && entry.second.visible &&
          road_distance(candidate, position(entry.second.car)) < 12) {
        occupied = true;
        break;
      }
    if (occupied)
      continue;
    r.edge = edge;
    r.along = at;
    r.unplaced = false;
    // A candidate can be checked before allocating a vehicle.
    if (r.car)
      publish(r, graph, at);
    return true;
  }
  return false;
}
int32_t add_car(const RpkIndex &rpk, void *ground, Object *car, Object *where,
                bool owned) {
  population(ground);
  auto *g = gameref_get(ground);
  auto *c = gameref_get(car);
  if (!g || !c)
    return 0;
  for (const auto &entry : registrations)
    if (entry.second.ground == g->key && entry.second.car == c->key &&
        entry.second.car_incarnation == c->incarnation)
      return entry.first;
  if (next_id == INT32_MAX)
    return 0;
  Registration r;
  r.id = next_id++;
  r.ground = g->key;
  r.ground_incarnation = g->incarnation;
  r.car = static_cast<Object *>(c->key);
  r.car_incarnation = c->incarnation;
  r.owned = owned;
  r.notify = owned;
  r.cruise = (15 + random() * 20) / 3.6f;
  auto cfg = load_part_config(rpk, uint32_t(c->type_id));
  r.kinematic = !part_chassis(r.car) || cfg.wheels.size() != 4;
  if (r.kinematic)
    part_set_external_motion(r.car, true);
  if (auto p = point(where)) {
    gameref_set_matrix(r.car, (*p)[0], (*p)[1], (*p)[2], 0, 0, 0);
    r.unplaced = false;
    auto graph = race_ground_roads(rpk, ground);
    if (auto projected = graph ? graph->traffic_project(*p) : std::nullopt) {
      r.edge = projected->edge;
      r.along = projected->along;
      publish(r, *graph, r.along);
    } else {
      r.unplaced = true;
    }
  }
  c->streamed_hidden = true;
  registrations.emplace(r.id, r);
  return r.id;
}
} // namespace
void race_traffic_forget(void *key) {
  populations.erase(key);
  std::vector<int32_t> ids;
  for (const auto &e : registrations)
    if (e.second.ground == key || e.second.car == key)
      ids.push_back(e.first);
  for (auto id : ids)
    remove(id);
}
void race_traffic_reset() {
  ++signal_revision;
  registrations.clear();
  populations.clear();
  events.clear();
  next_id = 1;
  clock_seconds = 0;
}
void *race_traffic_ground(Object *car) {
  const auto *node = gameref_get(car);
  if (!node)
    return nullptr;
  // SLRR_rev TrafficNative.Registration retains the original map/ground.
  // Native maps can be GameType resources rather than enum GroundRef nodes.
  for (const auto &[id, r] : registrations)
    if (r.car == node->key && r.car_incarnation == node->incarnation) {
      const auto *ground = gameref_get(r.ground);
      return ground && ground->incarnation == r.ground_incarnation
                 ? ground->key : nullptr;
    }
  return nullptr;
}
int32_t race_traffic_pointer(Object *car) {
  auto *n = gameref_get(car);
  if (!n)
    return 0;
  for (const auto &e : registrations)
    if (e.second.car == n->key && e.second.car_incarnation == n->incarnation)
      return e.first;
  return 0;
}
void race_halt_path(const RpkIndex &rpk, void *ground, const RoadPoint &a,
                    const RoadPoint &b) {
  auto graph = race_ground_roads(rpk, ground);
  auto *p = population(ground);
  if (!graph || !p)
    return;
  auto route = graph->query(a, b);
  if (!route)
    return;
  std::unordered_set<uint32_t> selected(route->roads.begin(),
                                        route->roads.end());
  std::vector<std::pair<uint32_t, uint32_t>> pairs;
  for (const auto &e : graph->edges())
    if (std::any_of(e.roads.begin(), e.roads.end(),
                    [&](auto id) { return selected.contains(id); }))
      pairs.push_back({std::min(e.from, e.to), std::max(e.from, e.to)});
  for (const auto &e : graph->edges())
    if (std::find(pairs.begin(), pairs.end(),
                  std::pair{std::min(e.from, e.to), std::max(e.from, e.to)}) !=
        pairs.end())
      for (auto id : e.roads)
        p->blocked.insert(id);
}
void race_halt_cross(const RpkIndex &rpk, void *ground,
                     const RoadPoint &position, float duration) {
  if (!std::isfinite(duration) || duration <= 0)
    return;
  auto graph = race_ground_roads(rpk, ground);
  auto *p = population(ground);
  if (!graph || !p)
    return;
  uint32_t node = 0;
  float best = std::numeric_limits<float>::infinity();
  for (const auto &e : graph->edges()) {
    float a = road_distance(position, e.points.front()),
          b = road_distance(position, e.points.back());
    if (a < best) {
      best = a;
      node = e.from;
    }
    if (b < best) {
      best = b;
      node = e.to;
    }
  }
  if (!node)
    return;
  double expiry = clock_seconds + std::min(duration, 86400.f);
  p->crossings[node] = std::max(p->crossings[node], expiry);
}
void race_clear_halts(void *ground) {
  if (auto *p = population(ground)) {
    p->blocked.clear();
    p->crossings.clear();
  }
}
bool race_traffic_halted(void *ground, const RoadPoint &position, Object *car,
                         float *stop_distance) {
  float ignored = 0;
  float &stop = stop_distance ? *stop_distance : ignored;
  stop = 0;
  auto *p = population(ground);
  if (!p || !resources)
    return false;
  auto graph = race_ground_roads(*resources, ground);
  if (graph)
    if (auto street = graph->project(position))
      for (auto road : graph->edges()[street->edge].roads)
        if (p->blocked.contains(road))
          return true;
  PartMatrix frame;
  RoadPoint heading{};
  if (car && scene_instances_script_matrix(car, frame.data()))
    heading = {-frame[8], -frame[9], -frame[10]};
  auto projection =
      graph ? graph->traffic_project(position, car ? &heading : nullptr, true)
            : std::nullopt;
  if (!projection)
    return false;
  const auto &e = graph->traffic_edges()[projection->edge];
  for (auto road : e.roads)
    if (p->blocked.contains(road))
      return true;
  // A traffic lane ends where the junction begins. The car's centre stops a
  // few metres short so its nose stays out of the junction, and the driver
  // brakes over the whole approach rather than locking at a fixed distance.
  const float remaining = e.length - projection->along;
  stop = std::max(0.f, remaining - 3);
  const auto *node = car ? gameref_get(car) : nullptr;
  const float speed = node ? std::hypot(node->vel[0], node->vel[2]) : 0;
  // Too close to stop at a comfortable 3 m/s2 when the light turns yellow.
  const bool committed = speed * speed / 6 > stop + 1;
  auto cross = p->crossings.find(e.to);
  if (cross != p->crossings.end() && cross->second > clock_seconds &&
      remaining < 22)
    return true;
  auto color = graph->traffic_signal(projection->edge, clock_seconds);
  Registration *registered = nullptr;
  if (car)
    for (auto &entry : registrations)
      if (entry.second.car == car) {
        registered = &entry.second;
        break;
      }
  if (registered) {
    if (registered->edge != projection->edge) {
      registered->cleared_crossing = 0;
      registered->stop_since = -1;
    }
    registered->edge = projection->edge;
    registered->along = projection->along;
    if ((color == 2 && remaining < 14) ||
        (color == 1 && remaining < 22 && committed))
      registered->cleared_crossing = e.to;
    if (registered->cleared_crossing == e.to)
      return false;
    if (color == -2 && remaining < 22) {
      if (remaining < 6 && speed < .5f) {
        if (registered->stop_since < 0)
          registered->stop_since = clock_seconds;
        if (clock_seconds - registered->stop_since >= 5) {
          registered->cleared_crossing = e.to;
          return false;
        }
      } else
        registered->stop_since = -1;
      return true;
    }
  }
  if (color == 1 && committed)
    return false;
  return (color == 0 || color == 1) && remaining < 22;
}
void register_traffic_natives(Jvm &vm, RpkIndex &rpk, SectionMachine &machine) {
  race_traffic_reset();
  sections = &machine;
  resources = &rpk;
  vm.natives().register_method(
      "java.util.resource.GroundRef", "addTrafficCar",
      "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;)I",
      [&rpk](Object *self, const std::vector<Value> &a) {
        auto *car = object(a, 0);
        if (auto *chassis = part_chassis(car))
          car = chassis;
        return Value::make_int(add_car(rpk, self, car, object(a, 1), false));
      });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "remTrafficCar", "(I)V",
      [](Object *self, const std::vector<Value> &a) {
        auto id = int32_t(number(a, 0));
        auto it = registrations.find(id);
        if (it != registrations.end() && it->second.ground == canonical(self))
          remove(id, false);
        return Value::make_void();
      });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "notifyTrafficCar", "(II)I",
      [](Object *self, const std::vector<Value> &a) {
        auto it = registrations.find(int32_t(number(a, 0)));
        if (it == registrations.end() || it->second.ground != canonical(self))
          return Value::make_int(0);
        bool enabled = number(a, 1) != 0;
        if (!enabled)
          queue(it->second, false);
        it->second.notify = enabled;
        if (enabled && it->second.visible)
          queue(it->second, true);
        return Value::make_int(1);
      });
  auto model = [](Object *self, const std::vector<Value> &a) {
    auto *p = population(self);
    auto type = model_type(object(a, 0));
    if (!p || !type)
      return Value::make_int(0);
    constexpr size_t offset = 1;
    float weight = number(a, offset);
    if (!std::isfinite(weight) || weight <= 0)
      return Value::make_int(0);
    Model m{type, weight, number(a, offset + 1), number(a, offset + 2),
            number(a, offset + 3)};
    if (!m.type)
      return Value::make_int(0);
    for (size_t i = 0; i < p->models.size(); ++i)
      if (p->models[i].type == m.type) {
        p->models[i].weight += weight;
        return Value::make_int(int32_t(i + 1));
      }
    p->models.push_back(m);
    return Value::make_int(int32_t(p->models.size()));
  };
  vm.natives().register_method(
      "java.util.resource.GroundRef", "addTrafficN",
      "(Ljava.util.resource.GameRef;IFFF)I",
      [model](Object *self, const std::vector<Value> &a) {
        return model(self, a);
      });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "addTrafficP",
      "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;IFFF)I",
      [](Object *self, const std::vector<Value> &a) {
        auto *p = population(self);
        auto type = model_type(object(a, 0));
        auto position = point(object(a, 1));
        float count = number(a, 2);
        if (!p || !type || !position || !std::isfinite(count) || count <= 0)
          return Value::make_int(0);
        Seed seed;
        seed.model = {type, 1, number(a, 3), number(a, 4), number(a, 5)};
        seed.position = *position;
        seed.remaining = int(std::min(count, 50.f));
        if (!seed.remaining)
          return Value::make_int(0);
        p->seeds.push_back(seed);
        return Value::make_int(seed.remaining);
      });
  vm.natives().register_method("java.util.resource.GroundRef", "delTraffic",
                               "()V",
                               [](Object *self, const std::vector<Value> &) {
                                 auto key = canonical(self);
                                 std::vector<int32_t> ids;
                                 for (const auto &e : registrations)
                                   if (e.second.ground == key)
                                     ids.push_back(e.first);
                                 for (auto id : ids)
                                   remove(id);
                                 populations.erase(key);
                                 return Value::make_void();
                               });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "setTrafficCarBehaviour", "(II)V",
      [](Object *self, const std::vector<Value> &a) {
        auto it = registrations.find(int32_t(number(a, 0)));
        if (it != registrations.end() && it->second.ground == canonical(self))
          it->second.mode = int(number(a, 1));
        return Value::make_void();
      });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "haltTrafficCross",
      "(Ljava.lang.Vector3;F)V",
      [&rpk](Object *self, const std::vector<Value> &a) {
        auto p = point(object(a, 0));
        if (p)
          race_halt_cross(rpk, self, *p, number(a, 1));
        return Value::make_void();
      });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "haltTrafficPath",
      "(Ljava.lang.Vector3;Ljava.lang.Vector3;)V",
      [&rpk](Object *self, const std::vector<Value> &a) {
        auto x = point(object(a, 0)), y = point(object(a, 1));
        if (x && y)
          race_halt_path(rpk, self, *x, *y);
        return Value::make_void();
      });
  vm.natives().register_method("java.util.resource.GroundRef", "clearHalts",
                               "()V",
                               [](Object *self, const std::vector<Value> &) {
                                 race_clear_halts(self);
                                 return Value::make_void();
                               });
}
uint64_t race_signal_revision() { return signal_revision; }
int race_signal_at(const RoadPoint &position) {
  if (!resources)
    return -1;
  int result = -1;
  float best = 64.f;
  for (const auto &entry : populations) {
    if (!belongs(entry.first, scene_instances_world()))
      continue;
    auto graph = race_ground_roads(*resources, entry.first);
    if (!graph)
      continue;
    for (const auto &fixture : graph->signals()) {
      float d = road_distance(position, fixture.position);
      if (d < best) {
        best = d;
        result = graph->signal(fixture.crossing, fixture.branch, clock_seconds);
      }
    }
  }
  return result;
}
void race_traffic_tick(Jvm &vm, float seconds, double elapsed_seconds) {
  if (!std::isfinite(seconds) || seconds < 0 || !resources)
    return;
  auto phase = [](double seconds) {
    return uint64_t(seconds / 13) * 2 + (std::fmod(seconds, 13) >= 10);
  };
  auto old_phase = phase(clock_seconds);
  // Slow frames must not stretch a 13-second signal slot into minutes. The
  // caller passes elapsed time with pause/time-warp applied, independently
  // of the bounded motion step. Tests/fixed-step callers can omit it.
  if (elapsed_seconds < 0)
    elapsed_seconds = seconds;
  if (std::isfinite(elapsed_seconds) && elapsed_seconds > 0)
    clock_seconds += elapsed_seconds;
  if (old_phase != phase(clock_seconds))
    ++signal_revision;
  auto *world = scene_instances_world();
  update_view();
  auto *focus_car = part_controlled_chassis();
  const auto focus = focus_car ? position(focus_car) : RoadPoint{};
  const float density = host_game_options().traffic_density;
  const int cap =
      std::isfinite(density) ? int(std::clamp(density, 0.f, 1.f) * 100) : 0;
  std::vector<void *> ground_keys;
  for (const auto &e : populations)
    ground_keys.push_back(e.first);
  for (void *key : ground_keys) {
    if (!world || !focus_car || !belongs(key, world) || seconds == 0)
      continue;
    auto graph = race_ground_roads(*resources, key);
    auto *p = population(key);
    if (!graph || !p || graph->traffic_edges().empty())
      continue;
    if (p->started < 0)
      p->started = clock_seconds;
    const bool unseen = clock_seconds - p->started > 3;
    std::vector<int32_t> ids;
    for (const auto &e : registrations)
      if (e.second.ground == key)
        ids.push_back(e.first);
    std::sort(ids.begin(), ids.end());
    float model_weight = 0;
    for (const auto &model : p->models)
      model_weight += model.weight;
    int registered = 0;
    for (auto id : ids)
      if (!registrations.at(id).owned)
        ++registered;
    // Each original addTrafficCar entry has weight one, just like upstream's
    // catalog. It does not reserve half the population ahead of road traffic.
    const int rival_cap =
        model_weight > 0
            ? int(std::round(cap * registered / (model_weight + registered)))
            : std::min(cap, registered);
    int visible = 0, rivals = 0;
    for (auto id : ids) {
      auto it = registrations.find(id);
      if (it == registrations.end())
        continue;
      auto &r = it->second;
      auto *n = gameref_get(r.car);
      if (!n || n->incarnation != r.car_incarnation) {
        remove(id);
        continue;
      }
      if (r.edge >= graph->traffic_edges().size()) {
        r.edge = 0;
        r.unplaced = true;
      }
      // A distant car stays while the player can still see it.
      const auto where = position(r.car);
      const bool far = road_distance(where, focus) > (r.owned ? 220 : 380) &&
                       !(unseen && in_view(where));
      if (r.visible &&
          (visible >= cap || (!r.owned && rivals >= rival_cap) || far ||
           blocked(*p, graph->traffic_edges()[r.edge]))) {
        queue(r, false);
        r.visible = false;
        n->streamed_hidden = true;
        if (r.owned) {
          remove(id);
          continue;
        }
        r.distant_next = 0;
      }
      if (r.visible) {
        ++visible;
        if (!r.owned)
          ++rivals;
      }
    }
    for (auto id : ids) {
      auto it = registrations.find(id);
      if (it == registrations.end())
        continue;
      auto &r = it->second;
      auto *n = gameref_get(r.car);
      if (!n)
        continue;
      if (!r.visible && !r.owned && clock_seconds >= r.distant_next) {
        r.distant_next = clock_seconds + 5;
        if (r.unplaced) {
          if (place(r, *graph, *p, focus, true, unseen))
            r.unplaced = false;
        } else if (road_distance(position(r.car), focus) > 300) {
          auto near = graph->traffic_project(position(r.car));
          if (near) {
            r.edge = near->edge;
            r.along = near->along + r.speed * 5;
            // Coarse updates own hidden rivals only; visible chassis keep
            // physical motion.
            for (int hop = 0;
                 hop < 32 && r.along > graph->traffic_edges()[r.edge].length;
                 ++hop) {
              const auto &e = graph->traffic_edges()[r.edge];
              std::vector<size_t> choices;
              for (size_t i : e.next)
                if (!blocked(*p, graph->traffic_edges()[i]))
                  choices.push_back(i);
              if (choices.empty()) {
                r.along = e.length;
                break;
              }
              r.along -= e.length;
              r.edge = choose_transition(choices, *graph);
            }
            publish(r, *graph,
                    std::min(r.along, graph->traffic_edges()[r.edge].length));
          }
        }
      }
      if (!r.visible && !r.owned && visible < cap && rivals < rival_cap) {
        if (r.unplaced && !place(r, *graph, *p, focus, true, unseen))
          continue;
        if (road_distance(position(r.car), focus) > 300 ||
            (unseen && in_view(position(r.car))))
          continue;
        r.visible = true;
        n->streamed_hidden = false;
        publish(r, *graph, r.along);
        ++visible;
        ++rivals;
        if (!r.kinematic) {
          race_set_traffic_mode(*resources, r.car);
        }
        queue(r, true);
      }
      if (!r.visible || !r.kinematic)
        continue;
      const auto &e = graph->traffic_edges()[r.edge];
      float desired = r.cruise;
      if (blocked(*p, e))
        desired = 0;
      auto signal = graph->traffic_signal(r.edge, clock_seconds);
      if (signal == 2 && e.length - r.along <= 2 + r.speed * seconds)
        r.cleared_crossing = e.to;
      if (r.cleared_crossing == e.to)
        signal = 2;
      const float stop = std::max(0.f, e.length - r.along - 2.f);
      if (signal == 0 || signal == 1)
        desired = std::min(desired, std::sqrt(8 * stop));
      for (const auto &entry : registrations) {
        const auto &peer = entry.second;
        if (peer.id != r.id && peer.visible && peer.ground == key &&
            peer.edge == r.edge && peer.along > r.along)
          desired = std::min(
              desired, std::sqrt(8 * std::max(0.f, peer.along - r.along - 8)));
      }
      r.speed += std::clamp(desired - r.speed, -4.f * seconds, .8f * seconds);
      float advance = r.speed * seconds;
      if (signal == 0 || signal == 1)
        advance = std::min(advance, stop);
      r.along += advance;
      if (advance == 0)
        r.speed = 0;
      if (r.along > e.length) {
        std::vector<size_t> choices;
        for (size_t i : e.next) {
          const auto &next = graph->traffic_edges()[i];
          if (!blocked(*p, next))
            choices.push_back(i);
        }
        if (choices.empty())
          r.along = e.length;
        else {
          r.along -= e.length;
          r.cleared_crossing = 0;
          r.edge = choose_transition(choices, *graph);
        }
      }
      const float previous_yaw = n->ypr[0];
      publish(r, *graph,
              std::min(r.along, graph->traffic_edges()[r.edge].length));
      const float delta = std::remainder(n->ypr[0] - previous_yaw, 6.2831853f);
      part_animate_external_wheels(
          r.car, advance,
          advance > .001f ? std::atan(2.5f * delta / advance) : 0);
    }
    // Positioned reinsertion is a finite request, never a permanent model
    // weight.
    for (auto &seed : p->seeds)
      while (seed.remaining > 0 && visible < cap) {
        auto projected = graph->traffic_project(seed.position);
        if (!projected || blocked(*p, graph->traffic_edges()[projected->edge]))
          break;
        Object *car = part_create(vm, *resources, static_cast<Object *>(key),
                                  seed.model.type, "positioned traffic");
        if (!car) {
          seed.remaining = 0;
          break;
        }
        auto id = add_car(*resources, key, car, nullptr, true);
        auto it = registrations.find(id);
        if (it == registrations.end()) {
          part_destroy(car);
          break;
        }
        auto &r = it->second;
        r.edge = projected->edge;
        r.along = projected->along;
        r.unplaced = false;
        r.visible = true;
        publish(r, *graph, r.along);
        if (auto *n = gameref_get(car))
          n->streamed_hidden = false;
        if (!r.kinematic)
          race_set_traffic_mode(*resources, car);
        queue(r, true);
        ++visible;
        --seed.remaining;
      }
    std::erase_if(p->seeds,
                  [](const Seed &seed) { return seed.remaining <= 0; });
    // Original Java model weights choose appearance; the user density bounds
    // the active host population independently, as in the v221 stream model.
    // Unseen road can run short; retry next frame rather than search on.
    for (int attempt = 0, misses = 0; visible < cap && !p->models.empty() &&
                                      attempt < cap * 3 && (!unseen || misses < 8);
         ++attempt) {
      float total = 0;
      for (const auto &m : p->models)
        total += m.weight;
      if (!std::isfinite(total) || total <= 0)
        break;
      float choice = random() * total;
      const Model *selected = &p->models.back();
      for (const auto &m : p->models) {
        choice -= m.weight;
        if (choice < 0) {
          selected = &m;
          break;
        }
      }
      // Placement rejection must not construct and destroy a complete CFG
      // assembly every frame. Upstream also selects a safe spawn before it
      // creates the traffic vehicle. Keep the same search/safety budget.
      Registration candidate;
      if (!place(candidate, *graph, *p, focus, true, unseen)) {
        ++misses;
        continue;
      }
      Object *car = part_create(vm, *resources, static_cast<Object *>(key),
                                selected->type, "streamed traffic");
      if (!car)
        continue;
      const auto id = add_car(*resources, key, car, nullptr, true);
      auto it = registrations.find(id);
      if (it == registrations.end()) {
        part_destroy(car);
        continue;
      }
      auto &r = it->second;
      r.edge = candidate.edge;
      r.along = candidate.along;
      r.unplaced = false;
      publish(r, *graph, r.along);
      r.visible = true;
      if (auto *n = gameref_get(car))
        n->streamed_hidden = false;
      if (!r.kinematic) {
        race_set_traffic_mode(*resources, car);
      }
      queue(r, true);
      ++visible;
    }
  }
  // Deliver after placement and after native entry has returned. Original City
  // creates its own TrafficTracker and marker; the host never duplicates them.
  auto pending = std::move(events);
  events.clear();
  for (const auto &e : pending) {
    auto *target = gameref_get(e.target);
    if (!target || target->incarnation != e.incarnation ||
        !(target->event_mask & 0x10) || !gameref_get(e.ground) ||
        gameref_get(e.ground)->incarnation != e.ground_incarnation)
      continue;
    auto *message = vm.new_string(e.text);
    Jvm::RootScope roots(vm, [&](const GcRootVisitor &visit) {
      visit(e.target);
      visit(e.ground);
      visit(message);
    });
    vm.invoke(e.target->class_name, "handleEvent",
              "(Ljava.util.resource.GameRef;ILjava.lang.String;)V",
              {Value::make_obj(e.target), Value::make_obj(e.ground),
               Value::make_int(0x10), Value::make_obj(message)},
              false);
  }
}
} // namespace slrr
