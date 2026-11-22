#include "slrr/race_runtime.hpp"
#include "slrr/gameref.hpp"
#include "slrr/jvm.hpp"
#include "slrr/object.hpp"
#include "slrr/rpk_index.hpp"
#include "slrr/scene_instances.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <unordered_map>
#include <unordered_set>
namespace slrr {
namespace {
struct Ground {
  uint64_t incarnation = 0, generation = 0;
  std::string root;
  int32_t type = 0;
  std::shared_ptr<RoadRoutes> roads;
  std::optional<RoadRoute> route;
};
std::unordered_map<void *, Ground> grounds;
void *plot_ground = nullptr;
Ground *ground_state(const RpkIndex &rpk, void *key) {
  auto *n = gameref_get(key);
  if (!n)
    return nullptr;
  auto &g = grounds[n->key];
  if (g.incarnation == n->incarnation && g.generation == rpk.generation() &&
      g.root == rpk.game_root() && g.type == n->type_id)
    return &g;
  g = {};
  g.incarnation = n->incarnation;
  g.generation = rpk.generation();
  g.root = rpk.game_root();
  g.type = n->type_id;
  auto bytes = rpk.read_payload(static_cast<uint32_t>(n->type_id));
  std::string text(bytes.begin(), bytes.end());
  // Same selected-ground lookup as SLRR_rev v221 engine/map-roads.ts.
  const std::regex pattern(R"(maps[\\/][^\s,\x00";]+\.trc)", std::regex::icase);
  std::string file;
  for (std::sregex_iterator it(text.begin(), text.end(), pattern), end;
       it != end; ++it) {
    std::string p = it->str();
    std::replace(p.begin(), p.end(), '\\', '/');
    if (!file.empty() && file != p)
      return &g;
    file = p;
  }
  if (file.empty())
    return &g;
  std::ifstream in(std::filesystem::path(rpk.game_root()) / file,
                   std::ios::binary | std::ios::ate);
  if (!in)
    return &g;
  auto size = in.tellg();
  if (size <= 0 || size > 128 * 1024 * 1024)
    return &g;
  std::vector<uint8_t> data(static_cast<size_t>(size));
  in.seekg(0);
  in.read(reinterpret_cast<char *>(data.data()), size);
  if (!in)
    return &g;
  RoadNetwork network;
  std::string error;
  if (trc_read(data.data(), data.size(), &network, &error)) {
    std::vector<RoadPoint> signals;
    std::vector<uint32_t> maps{uint32_t(n->type_id)};
    std::unordered_set<uint32_t> visited;
    for (size_t i = 0; i < maps.size(); ++i) {
      if (!visited.insert(maps[i]).second)
        continue;
      auto map = scene_instances_ground_map(rpk, maps[i]);
      maps.insert(maps.end(), map.exterior_maps.begin(),
                  map.exterior_maps.end());
      for (const auto &placement : map.placements) {
        auto bytes = rpk.read_payload(placement.type);
        if (bytes.size() > 16 &&
            std::equal(bytes.begin(), bytes.begin() + 4, "ICFG") &&
            std::equal(bytes.begin() + 8, bytes.begin() + 16, "trlight\0"))
          signals.push_back(placement.position);
      }
    }
    g.roads = std::make_shared<RoadRoutes>(network, signals);
  }
  return &g;
}
float number(const Value &v) {
  return v.tag == ValueTag::Float ? v.v.f
         : v.tag == ValueTag::Int ? float(v.v.i)
                                  : std::numeric_limits<float>::quiet_NaN();
}
std::optional<RoadPoint> point(Object *o) {
  if (!o)
    return {};
  RoadPoint p;
  for (int i = 0; i < 3; ++i) {
    const auto it = o->fields.find(i == 0 ? "x" : i == 1 ? "y" : "z");
    if (it == o->fields.end())
      return {};
    p[i] = number(it->second);
    if (!std::isfinite(p[i]))
      return {};
  }
  return p;
}
Object *vector(Jvm &vm, const RoadPoint &p) {
  auto *o = vm.new_object("java.lang.Vector3");
  for (int i = 0; i < 3; ++i)
    o->fields[i == 0 ? "x" : i == 1 ? "y" : "z"] = Value::make_float(p[i]);
  return o;
}
Object *object(const std::vector<Value> &args, size_t i) {
  return i < args.size() && args[i].tag == ValueTag::Obj ? args[i].v.o
                                                         : nullptr;
}
} // namespace
std::shared_ptr<const RoadRoutes> race_ground_roads(const RpkIndex &rpk,
                                                    void *key) {
  auto *g = ground_state(rpk, key);
  return g ? g->roads : nullptr;
}
std::optional<RoadRoute> race_query_route(const RpkIndex &rpk, void *key,
                                          const RoadPoint &a,
                                          const RoadPoint &b) {
  auto *g = ground_state(rpk, key);
  if (!g)
    return {};
  g->route = g->roads ? g->roads->query(a, b) : std::nullopt;
  plot_ground = g->route ? gameref_get(key)->key : nullptr;
  return g->route;
}
const RoadRoute *race_last_route(void *key) {
  auto *n = gameref_get(key);
  if (!n)
    return nullptr;
  auto it = grounds.find(n->key);
  return it != grounds.end() && it->second.incarnation == n->incarnation &&
                 it->second.route
             ? &*it->second.route
             : nullptr;
}
const RoadRoute *race_plot_route() { return race_last_route(plot_ground); }
void race_forget(void *key) {
  if (plot_ground == key)
    plot_ground = nullptr;
  grounds.erase(key);
  race_brain_forget(key);
  race_traffic_forget(key);
}
void race_reset() {
  plot_ground = nullptr;
  grounds.clear();
  race_brains_reset();
  race_traffic_reset();
}
void register_race_natives(Jvm &vm, RpkIndex &rpk, SectionMachine &sections) {
  register_traffic_natives(vm, rpk, sections);
  vm.natives().register_method(
      "java.util.resource.GroundRef", "getNearestCross",
      "(Ljava.lang.Vector3;F)Ljava.lang.Vector3;",
      [&vm, &rpk](Object *self, const std::vector<Value> &args) {
        auto p = point(object(args, 0));
        auto graph = race_ground_roads(rpk, self);
        auto cross = p && graph && args.size() > 1
                         ? graph->nearest_cross(*p, number(args[1]))
                         : std::nullopt;
        return Value::make_obj(cross ? vector(vm, *cross) : nullptr);
      });
  auto query = [&rpk](Object *self, const std::vector<Value> &args) {
    auto a = point(object(args, 0)), b = point(object(args, 1));
    if (a && b)
      return race_query_route(rpk, self, *a, *b);
    plot_ground = nullptr;
    if (auto *g = ground_state(rpk, self))
      g->route.reset();
    return std::optional<RoadRoute>{};
  };
  vm.natives().register_method(
      "java.util.resource.GroundRef", "getStartDirection",
      "(Ljava.lang.Vector3;Ljava.lang.Vector3;)Ljava.lang.Vector3;",
      [&vm, query](Object *self, const std::vector<Value> &args) {
        auto r = query(self, args);
        return Value::make_obj(r && road_distance(r->direction, {}) > 1e-8f
                                   ? vector(vm, r->direction)
                                   : nullptr);
      });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "findRoute",
      "(Ljava.lang.Vector3;Ljava.lang.Vector3;)F",
      [query](Object *self, const std::vector<Value> &args) {
        auto r = query(self, args);
        return Value::make_float(r ? r->length : -1.f);
      });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "getRouteLength",
      "(Ljava.lang.Vector3;Ljava.lang.Vector3;)F",
      [query](Object *self, const std::vector<Value> &args) {
        auto r = query(self, args);
        float length = r ? r->length : -1.f;
        // City compares police-to-player road distance with 10 m to pull a
        // driver over. For two cars a few metres apart on one level, the
        // graph's lane offsets and junction anchors only inflate it.
        auto a = point(object(args, 0)), b = point(object(args, 1));
        if (a && b && std::fabs((*a)[1] - (*b)[1]) < 2.5f &&
            std::hypot((*a)[0] - (*b)[0], (*a)[2] - (*b)[2]) < 20) {
          const float direct = road_distance(*a, *b);
          length = length < 0 ? direct : std::min(length, direct);
        }
        return Value::make_float(length);
      });
  vm.natives().register_method("java.util.resource.GroundRef", "getRouteLength",
                               "()F",
                               [](Object *self, const std::vector<Value> &) {
                                 auto *r = race_last_route(self);
                                 return Value::make_float(r ? r->length : -1.f);
                               });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "getRoutePos", "(F)Ljava.lang.Vector3;",
      [&vm](Object *self, const std::vector<Value> &args) {
        auto *r = race_last_route(self);
        float at = args.empty() ? 0 : number(args[0]);
        return Value::make_obj(
            r && std::isfinite(at) ? vector(vm, road_at(*r, at)) : nullptr);
      });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "getRouteDist", "(Ljava.lang.Vector3;)F",
      [](Object *self, const std::vector<Value> &args) {
        auto *r = race_last_route(self);
        auto p = point(object(args, 0));
        if (!r || !p)
          return Value::make_float(-1);
        float best = std::numeric_limits<float>::infinity(), along = 0,
              result = 0;
        for (size_t i = 1; i < r->points.size(); ++i) {
          auto a = r->points[i - 1], b = r->points[i];
          float length = road_distance(a, b), t = 0;
          for (int k = 0; k < 3; ++k)
            t += ((*p)[k] - a[k]) * (b[k] - a[k]);
          t = length > 0 ? std::clamp(t / (length * length), 0.f, 1.f) : 0;
          RoadPoint near;
          for (int k = 0; k < 3; ++k)
            near[k] = a[k] + (b[k] - a[k]) * t;
          float d = road_distance(*p, near);
          if (d < best) {
            best = d;
            result = along + t * length;
          }
          along += length;
        }
        return Value::make_float(result);
      });
  vm.natives().register_method(
      "java.util.resource.GroundRef", "alignToRoad",
      "(Ljava.lang.Vector3;)[Ljava.lang.Vector3;",
      [&vm, &rpk](Object *self, const std::vector<Value> &args) {
        auto p = point(object(args, 0));
        auto graph = race_ground_roads(rpk, self);
        auto aligned = p && graph ? graph->project(*p) : std::nullopt;
        if (!aligned)
          return Value::make_obj(nullptr);
        auto *array = vm.new_object("[Ljava.lang.Vector3;");
        Jvm::RootScope roots(vm,
                             [&](const GcRootVisitor &visit) { visit(array); });
        array->is_array = true;
        array->fields["length"] = Value::make_int(2);
        array->slots.resize(2);
        array->slots[0] = Value::make_obj(vector(vm, aligned->position));
        array->slots[1] = Value::make_obj(vector(vm, aligned->direction));
        return Value::make_obj(array);
      });
}
} // namespace slrr
